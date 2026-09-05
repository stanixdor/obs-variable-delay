#include "core/multistream-queue.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using dynamic_delay::SharedEncodedPacket;
using dynamic_delay::core::MultistreamQueue;
using Result = MultistreamQueue::PushResult;

void check(const bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

SharedEncodedPacket packet(const int64_t dts, const bool video = true, const bool keyframe = false,
			   const std::size_t size = 32)
{
	return {std::make_shared<const std::vector<uint8_t>>(size, uint8_t{7}), video, keyframe, dts, dts};
}

void reconnect_waits_for_new_video_keyframe_and_aligned_audio()
{
	MultistreamQueue queue;
	check(queue.push(packet(0, false), 0) == Result::WaitingForKeyframe, "audio cannot begin an epoch");
	check(queue.push(packet(0), 0) == Result::WaitingForKeyframe, "dependent video cannot begin an epoch");
	check(queue.push(packet(-66'666, true, true), 0) == Result::Accepted, "negative priming IDR is valid");
	check(queue.epoch_dts_usec() == -66'666, "the IDR defines the local DTS epoch");
	check(queue.push(packet(-70'000, false), 1) == Result::BeforeEpoch, "audio older than IDR must be discarded");
	check(queue.push(packet(0, false), 2) == Result::Accepted, "aligned audio may follow");
	queue.reset();
	check(queue.empty() && queue.waiting_for_keyframe(), "reconnect drops every old queued packet");
	check(queue.push(packet(100'000), 3) == Result::WaitingForKeyframe, "reconnect cannot reuse a previous GOP");
}

void destination_queues_share_immutable_payloads_without_copying()
{
	MultistreamQueue first;
	MultistreamQueue second;
	const auto shared = packet(0, true, true);
	check(shared.data.use_count() == 1, "one payload allocation before fanout");
	check(first.push(shared, 0) == Result::Accepted, "first destination accepts keyframe");
	check(second.push(shared, 0) == Result::Accepted, "second destination accepts same keyframe");
	check(shared.data.use_count() == 3, "fanout retains references, not cloned byte buffers");
	SharedEncodedPacket delivered;
	check(first.pop(delivered, 1), "first queue delivers");
	check(delivered.data.get() == shared.data.get(), "delivered payload identity is preserved");
	second.reset();
	check(shared.data.use_count() == 2, "stopping second output releases only its reference");
}

void byte_and_packet_limits_restart_only_the_slow_destination()
{
	MultistreamQueue slow{{64, 8, 2'000'000}};
	MultistreamQueue healthy{{128, 8, 2'000'000}};
	for (int64_t dts = 0; dts <= 40'000; dts += 20'000) {
		const auto shared = packet(dts, true, dts == 0);
		check(healthy.push(shared, static_cast<uint64_t>(dts)) == Result::Accepted, "healthy queue continues");
		const auto result = slow.push(shared, static_cast<uint64_t>(dts));
		check(result == (dts == 40'000 ? Result::Overflow : Result::Accepted), "slow queue enforces bytes");
	}
	check(slow.bytes() == 0 && slow.waiting_for_keyframe(), "overflow clears the complete old GOP");
	check(healthy.size() == 3, "one target overflowing cannot discard another target's packets");
	MultistreamQueue countBound{{1024, 1, 2'000'000}};
	check(countBound.push(packet(0, true, true), 0) == Result::Accepted, "first count-bounded packet");
	check(countBound.push(packet(1), 1) == Result::Overflow, "packet-count bound is independent of bytes");
	MultistreamQueue oversized{{16, 8, 2'000'000}};
	check(oversized.push(packet(0, true, true), 0) == Result::Overflow, "oversized IDR cannot exceed hard bound");
}

void queue_duration_bounds_cover_media_time_and_wall_time()
{
	MultistreamQueue media{{1024, 8, 100}};
	check(media.push(packet(0, true, true), 0) == Result::Accepted, "initial packet");
	check(media.push(packet(101), 1) == Result::Overflow, "media-time backlog is bounded despite quick arrival");
	MultistreamQueue wall{{1024, 8, 100}};
	check(wall.push(packet(0, true, true), 0) == Result::Accepted, "initial wall packet");
	check(wall.push(packet(1), 101) == Result::Overflow, "wall-time backlog is bounded despite unchanged media");
	MultistreamQueue idle{{1024, 8, 100}};
	check(idle.push(packet(0, true, true), 0) == Result::Accepted, "initial idle packet");
	SharedEncodedPacket delivered;
	check(!idle.pop(delivered, 101), "stale packet cannot escape because the producer stopped submitting");
	check(idle.empty() && idle.waiting_for_keyframe(), "stale dequeue forces a new epoch");
}

void b_frame_composition_and_timestamp_extremes_are_safe()
{
	MultistreamQueue queue;
	auto keyframe = packet(-100'000, true, true);
	keyframe.ptsUsec = 0;
	check(queue.push(keyframe, 1) == Result::Accepted, "B-frame IDR is retained");
	auto bFrame = packet(-50'000);
	bFrame.ptsUsec = -75'000;
	check(queue.push(bFrame, 2) == Result::Accepted, "negative composition offset is retained");
	SharedEncodedPacket delivered;
	check(queue.pop(delivered, 3) && delivered.ptsUsec - delivered.dtsUsec == 100'000,
	      "positive composition survives fanout");
	check(queue.pop(delivered, 3) && delivered.ptsUsec - delivered.dtsUsec == -25'000,
	      "negative composition survives fanout");
	queue.reset();
	check(queue.push(packet(std::numeric_limits<int64_t>::min(), true, true), 0) == Result::Accepted,
	      "minimum timestamp cannot underflow validation");
	check(queue.push(packet(std::numeric_limits<int64_t>::max()), 1) == Result::Overflow,
	      "extreme ordered timestamp delta saturates the queue limit without signed overflow");
}

void invalid_packets_and_clock_regression_do_not_corrupt_accounting()
{
	MultistreamQueue queue;
	check(queue.push({}, 0) == Result::Invalid, "null payload is rejected");
	check(queue.push(packet(0, true, true, 0), 0) == Result::Invalid, "empty payload is rejected");
	check(queue.bytes() == 0, "invalid packets allocate no queue budget");
	check(queue.push(packet(0, true, true), 100) == Result::Accepted, "valid packet starts queue");
	check(queue.push(packet(1), 99) == Result::Overflow, "monotonic-clock regression discards epoch");
	check(queue.bytes() == 0, "clock regression releases retained byte accounting");
}
} // namespace

int main()
{
	try {
		reconnect_waits_for_new_video_keyframe_and_aligned_audio();
		destination_queues_share_immutable_payloads_without_copying();
		byte_and_packet_limits_restart_only_the_slow_destination();
		queue_duration_bounds_cover_media_time_and_wall_time();
		b_frame_composition_and_timestamp_extremes_are_safe();
		invalid_packets_and_clock_regression_do_not_corrupt_accounting();
		std::cout << "6/6 production multistream queue cases passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
