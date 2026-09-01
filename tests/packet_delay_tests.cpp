#include "packet_delay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace obs_delay::core;

namespace {

class TestFailure final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

#define CHECK(expression)                                                                    \
	do {                                                                                       \
		if (!(expression)) {                                                                    \
			std::ostringstream message;                                                           \
			message << __FILE__ << ':' << __LINE__ << ": CHECK(" #expression ") failed";       \
			throw TestFailure{message.str()};                                                     \
		}                                                                                        \
	} while (false)

#define CHECK_EQ(left, right) CHECK((left) == (right))

SharedPayload payload_of(std::size_t size, std::byte value = std::byte{0x2a})
{
	std::vector<std::byte> bytes(size, value);
	return SharedPayload::copy(bytes);
}

Packet packet(StreamKey stream, std::int64_t pts, std::int64_t dts,
	std::size_t payload_size = 1, TimeBase time_base = {1, 1'000},
	bool keyframe = true)
{
	return {{stream, pts, dts, 1, time_base, keyframe, 0}, payload_of(payload_size)};
}

void begin_activation(PacketDelay &delay, Duration target, TimePoint now = 0ms)
{
	CHECK_EQ(delay.request_activate(target, now), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Preparing);
	CHECK_EQ(delay.begin_filling(now), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Filling);
}

void commit_activation(PacketDelay &delay, TimePoint now)
{
	CHECK(delay.ready_for_delayed(now));
	CHECK_EQ(delay.commit_delayed(now), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Delayed);
}

template<typename Exception, typename Function>
void check_throws(Function &&function)
{
	bool threw = false;
	try {
		std::forward<Function>(function)();
	} catch (const Exception &) {
		threw = true;
	}
	CHECK(threw);
}

void shared_payload_tracks_real_retained_memory()
{
	const std::array input{std::byte{1}, std::byte{2}, std::byte{3}};
	auto copied = SharedPayload::copy(input);
	CHECK_EQ(copied.size(), 3U);
	CHECK(copied.retained_size() >= copied.size());
	CHECK(copied.data() != input.data());
	CHECK_EQ(copied.data()[1], std::byte{2});

	struct Owner {
		std::array<std::byte, 64> bytes{};
	};
	auto owner = std::make_shared<Owner>();
	std::weak_ptr<Owner> weak = owner;
	auto alias = SharedPayload::alias(owner, owner->bytes.data(), 2, 64);
	owner.reset();
	CHECK(!weak.expired());
	CHECK_EQ(alias.size(), 2U);
	CHECK_EQ(alias.retained_size(), 64U);
	auto moved = std::move(alias);
	CHECK(alias.empty());
	CHECK_EQ(alias.retained_size(), 0U);
	moved = {};
	CHECK(weak.expired());

	check_throws<std::invalid_argument>([] {
		(void)SharedPayload::from_vector(nullptr);
	});
	check_throws<std::invalid_argument>([] {
		(void)SharedPayload::alias({}, reinterpret_cast<const std::byte *>(1), 1);
	});
	check_throws<std::invalid_argument>([] {
		auto storage = std::make_shared<std::byte>();
		(void)SharedPayload::alias(storage, storage.get(), 2, 1);
	});
}

void bypass_never_buffers_or_retimestamps()
{
	PacketDelay delay;
	auto input = packet({StreamKind::Video, 0, 7}, 123, 120, 5);
	const auto *data = input.payload.data();
	auto output = delay.push(std::move(input), 10ms);
	CHECK_EQ(output.size(), 1U);
	CHECK(!output[0].delayed);
	CHECK_EQ(output[0].packet.metadata.pts, 123);
	CHECK_EQ(output[0].packet.metadata.dts, 120);
	CHECK_EQ(output[0].packet.payload.data(), data);
	const auto metrics = delay.metrics(10ms);
	CHECK_EQ(metrics.state, DelayState::Bypass);
	CHECK_EQ(metrics.buffered_packets, 0U);
	CHECK_EQ(metrics.retained_bytes, 0U);
	CHECK_EQ(metrics.bypassed_packets, 1U);
	CHECK_EQ(to_string(delay.state()), "Bypass");
}

void filling_requires_real_coverage_and_explicit_commit()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	CHECK(delay.poll(100ms).empty());
	CHECK_EQ(delay.state(), DelayState::Filling);
	CHECK(!delay.ready_for_delayed(100ms));
	CHECK_EQ(delay.metrics(100ms).fill_progress, 0.0);

	CHECK(delay.push(packet({StreamKind::Video, 0, 1}, 100, 100), 100ms).empty());
	CHECK(!delay.ready_for_delayed(199ms));
	CHECK_EQ(delay.commit_delayed(199ms), RequestResult::NotReady);
	CHECK(delay.poll(200ms).empty());
	CHECK_EQ(delay.state(), DelayState::Filling);
	CHECK(delay.push(packet({StreamKind::Video, 0, 1}, 200, 200), 200ms).empty());
	CHECK(delay.ready_for_delayed(200ms));
	CHECK(delay.metrics(200ms).ready_for_delayed);

	// Readiness is observational: no packet leaves until the host commits its cut.
	CHECK(delay.poll(200ms).empty());
	CHECK_EQ(delay.state(), DelayState::Filling);
	commit_activation(delay, 200ms);
	auto output = delay.poll(200ms);
	CHECK_EQ(output.size(), 1U);
	CHECK(output[0].delayed);
	CHECK_EQ(output[0].received_at, 100ms);
	CHECK_EQ(output[0].packet.metadata.dts, 200);
}

void every_observed_stream_must_cover_the_target()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 2}, 0, 0), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 2}, 50, 50), 50ms).empty());
	// Future packets establish an interleave watermark without changing which
	// packet is old enough to be the delayed cut candidate.
	CHECK(delay.push(packet({StreamKind::Video, 0, 2}, 100, 100), 100ms).empty());
	CHECK(!delay.ready_for_delayed(100ms));
	CHECK_EQ(delay.metrics(100ms).fill_progress, 0.5);
	CHECK(!delay.metrics(100ms).ready_for_delayed);
	CHECK(delay.push(packet({StreamKind::Audio, 0, 2}, 150, 150), 150ms).empty());
	CHECK(delay.ready_for_delayed(150ms));
	commit_activation(delay, 150ms);
	auto output = delay.poll(150ms);
	CHECK_EQ(output.size(), 2U);
	CHECK_EQ(output[0].packet.metadata.stream.kind, StreamKind::Video);
	CHECK_EQ(output[1].packet.metadata.stream.kind, StreamKind::Audio);
}

void explicit_roster_blocks_missing_or_gapped_streams()
{
	const StreamKey video{StreamKind::Video, 0, 20};
	const StreamKey audio{StreamKind::Audio, 0, 20};
	const StreamKey optional_audio{StreamKind::Audio, 1, 20};
	DelayConfig roster_config;
	roster_config.required_streams = {video, audio};
	PacketDelay roster{roster_config};
	begin_activation(roster, 100ms);
	CHECK(roster.push(packet(video, 0, 0), 0ms).empty());
	CHECK(roster.push(packet(optional_audio, 0, 0), 0ms).empty());
	CHECK(roster.push(packet(video, 100, 100), 100ms).empty());
	CHECK(!roster.ready_for_delayed(100ms));
	CHECK(roster.push(packet(audio, 100, 100), 100ms).empty());
	CHECK(roster.push(packet(audio, 200, 200), 200ms).empty());
	CHECK(roster.ready_for_delayed(200ms));
	commit_activation(roster, 200ms);
	const auto roster_output = roster.poll(200ms);
	CHECK(roster_output.size() >= 2U);
	bool saw_video = false;
	bool saw_audio = false;
	for (const auto &item : roster_output) {
		CHECK(item.packet.metadata.stream != optional_audio);
		saw_video = saw_video || item.packet.metadata.stream == video;
		saw_audio = saw_audio || item.packet.metadata.stream == audio;
	}
	CHECK(saw_video);
	CHECK(saw_audio);

	DelayConfig gap_config;
	gap_config.max_delay = 3s;
	gap_config.max_stream_gap = 500ms;
	gap_config.required_streams = {video};
	PacketDelay gapped{gap_config};
	begin_activation(gapped, 2s);
	CHECK(gapped.push(packet(video, 0, 0), 0ms).empty());
	CHECK(gapped.push(packet(video, 2'000, 2'000), 2s).empty());
	CHECK(!gapped.ready_for_delayed(2s));
	CHECK(gapped.metrics(2s).fill_progress < 1.0);

	PacketDelay contiguous{gap_config};
	begin_activation(contiguous, 2s);
	for (int time : {0, 500, 1'000, 1'500, 2'000})
		CHECK(contiguous.push(packet(video, time, time), Duration{time}).empty());
	CHECK(contiguous.ready_for_delayed(2s));

	DelayConfig stale_config;
	stale_config.max_stream_gap = 100ms;
	stale_config.required_streams = {video};
	PacketDelay stale{stale_config};
	begin_activation(stale, 100ms);
	CHECK(stale.push(packet(video, 0, 0), 0ms).empty());
	CHECK(stale.push(packet(video, 100, 100), 100ms).empty());
	CHECK(stale.ready_for_delayed(100ms));
	CHECK(!stale.ready_for_delayed(10s));
	CHECK(stale.metrics(10s).fill_progress < 1.0);
}

void delayed_cut_starts_each_video_lane_on_a_keyframe()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 3}, 0, 0, 2,
		{1, 1'000}, false), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 3}, 0, 0), 0ms).empty());
	CHECK(!delay.ready_for_delayed(100ms));
	CHECK(delay.push(packet({StreamKind::Video, 0, 3}, 20, 20, 3,
		{1, 1'000}, true), 20ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 3}, 20, 20), 20ms).empty());
	CHECK(!delay.ready_for_delayed(119ms));
	CHECK(delay.push(packet({StreamKind::Video, 0, 3}, 120, 120, 1,
		{1, 1'000}, false), 120ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 3}, 120, 120), 120ms).empty());
	CHECK(delay.ready_for_delayed(120ms));
	commit_activation(delay, 120ms);
	auto output = delay.poll(120ms);
	CHECK_EQ(output.size(), 2U);
	for (const auto &item : output) {
		if (item.packet.metadata.stream.kind == StreamKind::Video)
			CHECK(item.packet.metadata.keyframe);
		CHECK(item.received_at >= 20ms);
	}
	CHECK(delay.metrics(120ms).pruned_retention_packets >= 2U);
}

void persistent_watermark_orders_across_separate_push_calls()
{
	DelayConfig config;
	config.stream_stall_timeout = 0ms;
	PacketDelay delay{config};
	begin_activation(delay, 0ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 4}, 0, 0), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 4}, 0, 0), 0ms).empty());
	commit_activation(delay, 0ms);
	CHECK_EQ(delay.poll(0ms).size(), 2U);

	// Audio 200 ms cannot overtake the still-empty video lane.
	CHECK(delay.push(packet({StreamKind::Audio, 0, 4}, 200, 200), 1ms).empty());
	auto video = delay.push(packet({StreamKind::Video, 0, 4}, 100, 100), 1ms);
	CHECK_EQ(video.size(), 1U);
	CHECK_EQ(video[0].packet.metadata.stream.kind, StreamKind::Video);
	CHECK_EQ(video[0].packet.metadata.dts, 100);
	auto audio = delay.push(packet({StreamKind::Video, 0, 4}, 300, 300), 2ms);
	CHECK_EQ(audio.size(), 1U);
	CHECK_EQ(audio[0].packet.metadata.stream.kind, StreamKind::Audio);
	CHECK_EQ(audio[0].packet.metadata.dts, 200);

	// A packet arriving below the persistent emitted watermark is fatal/fail-open.
	CHECK(delay.push(packet({StreamKind::Audio, 0, 4}, 50, 50), 3ms).empty());
	CHECK_EQ(delay.state(), DelayState::Error);
	CHECK(!delay.error_message().empty());
}

void timestamps_shift_once_and_preserve_pts_minus_dts()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	auto video = packet({StreamKind::Video, 0, 5}, 9'000, 8'100, 3,
		{1, 90'000}, true);
	video.metadata.duration = 3'000;
	video.metadata.flags = 17;
	CHECK(delay.push(std::move(video), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 5}, 18'000, 17'100, 1,
		{1, 90'000}, true), 100ms).empty());
	commit_activation(delay, 100ms);
	auto output = delay.poll(100ms);
	CHECK(!output.empty());
	CHECK_EQ(output[0].packet.metadata.pts, 18'000);
	CHECK_EQ(output[0].packet.metadata.dts, 17'100);
	CHECK_EQ(output[0].packet.metadata.pts - output[0].packet.metadata.dts, 900);
	CHECK_EQ(output[0].packet.metadata.duration, 3'000);
	CHECK_EQ(output[0].packet.metadata.flags, 17U);

	PacketDelay rational;
	begin_activation(rational, 1s);
	CHECK(rational.push(packet({StreamKind::Video, 0, 0}, 100, 90, 1,
		{1'001, 30'000}), 0ms).empty());
	CHECK(rational.push(packet({StreamKind::Video, 0, 0}, 130, 120, 1,
		{1'001, 30'000}), 1s).empty());
	commit_activation(rational, 1s);
	auto rounded = rational.poll(1s);
	CHECK_EQ(rounded[0].packet.metadata.pts, 30'100);
	CHECK_EQ(rounded[0].packet.metadata.dts, 30'090);
	CHECK_EQ(rounded[0].packet.metadata.pts - rounded[0].packet.metadata.dts, 10);
}

void delay_can_increase_and_decrease_while_active()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	for (int time : {0, 50, 100})
		CHECK(delay.push(packet({StreamKind::Video, 0, 6}, time, time),
			Duration{time}).empty());
	commit_activation(delay, 100ms);
	CHECK_EQ(delay.poll(100ms).size(), 1U);
	auto at_150 = delay.push(packet({StreamKind::Video, 0, 6}, 150, 150), 150ms);
	CHECK_EQ(at_150.size(), 1U);
	CHECK_EQ(at_150[0].packet.metadata.dts, 150);

	CHECK_EQ(delay.request_change_delay(200ms, 150ms), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Preparing);
	CHECK_EQ(delay.target_delay(), 200ms);
	// Old 100 ms delayed output continues until the host confirms its cover.
	auto old_branch = delay.push(packet({StreamKind::Video, 0, 6}, 200, 200), 200ms);
	CHECK_EQ(old_branch.size(), 1U);
	CHECK_EQ(old_branch[0].packet.metadata.dts, 200);
	CHECK_EQ(delay.begin_filling(200ms), RequestResult::Accepted);
	for (int time : {250, 300, 350})
		CHECK(delay.push(packet({StreamKind::Video, 0, 6}, time, time),
			Duration{time}).empty());
	CHECK(!delay.ready_for_delayed(349ms));
	commit_activation(delay, 350ms);
	auto increased = delay.poll(350ms);
	CHECK_EQ(increased.size(), 1U);
	CHECK_EQ(increased[0].packet.metadata.dts, 350);

	CHECK_EQ(delay.request_change_delay(50ms, 350ms), RequestResult::Accepted);
	CHECK_EQ(delay.begin_filling(350ms), RequestResult::Accepted);
	CHECK(delay.ready_for_delayed(350ms));
	commit_activation(delay, 350ms);
	auto decreased = delay.poll(350ms);
	CHECK_EQ(decreased.size(), 1U);
	CHECK(decreased[0].packet.metadata.dts > 350);
	CHECK_EQ(decreased[0].packet.metadata.pts -
		decreased[0].packet.metadata.dts, 0);
	CHECK_EQ(delay.target_delay(), 50ms);
}

void timestamp_epoch_absorbs_gop_overshoot_on_delay_reduction()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 22}, 0, 0), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 22}, 100, 100), 100ms).empty());
	commit_activation(delay, 100ms);
	CHECK_EQ(delay.poll(100ms)[0].packet.metadata.dts, 100);
	CHECK(delay.push(packet({StreamKind::Video, 0, 22}, 140, 140, 1,
		{1, 1'000}, true), 140ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 22}, 150, 150, 1,
		{1, 1'000}, false), 150ms).empty());
	auto old_epoch = delay.push(packet({StreamKind::Video, 0, 22}, 200, 200, 1,
		{1, 1'000}, false), 200ms);
	CHECK_EQ(old_epoch.size(), 1U);
	CHECK_EQ(old_epoch[0].packet.metadata.dts, 200);

	CHECK_EQ(delay.request_change_delay(50ms, 200ms), RequestResult::Accepted);
	CHECK_EQ(delay.begin_filling(200ms), RequestResult::Accepted);
	commit_activation(delay, 200ms);
	auto new_epoch = delay.poll(200ms);
	CHECK(!new_epoch.empty());
	CHECK(new_epoch[0].packet.metadata.keyframe);
	CHECK(new_epoch[0].packet.metadata.dts > old_epoch[0].packet.metadata.dts);
	CHECK_EQ(new_epoch[0].packet.metadata.pts -
		new_epoch[0].packet.metadata.dts, 0);
}

void b_frame_reorder_horizons_are_cleared_at_every_epoch()
{
	const StreamKey video{StreamKind::Video, 0, 27};
	const StreamKey audio{StreamKind::Audio, 0, 27};
	const StreamKey cover_video{StreamKind::Video, 0, 900};
	const StreamKey cover_audio{StreamKind::Audio, 0, 900};
	PacketDelay activation;
	begin_activation(activation, 100ms);
	CHECK(activation.push(packet(video, 200, 0, 1,
		{1, 1'000}, true), 0ms).empty());
	CHECK(activation.push(packet(audio, 0, 0), 0ms).empty());
	CHECK(activation.push(packet(video, 100, 100, 1,
		{1, 1'000}, false), 100ms).empty());
	CHECK(activation.push(packet(audio, 100, 100), 100ms).empty());
	// The host-owned cover encoder is outside push(). Its PTS may reorder, but
	// the maximum presentation horizon must still constrain the delayed epoch.
	CHECK(activation.observe_external_output(
		{cover_video, 800, 300, 1, {1, 1'000}, true, 0}));
	CHECK(activation.observe_external_output(
		{cover_audio, 300, 300, 1, {1, 1'000}, true, 0}));
	CHECK(activation.observe_external_output(
		{cover_video, 350, 400, 1, {1, 1'000}, false, 0}));
	CHECK(activation.observe_external_output(
		{cover_audio, 400, 400, 1, {1, 1'000}, true, 0}));
	commit_activation(activation, 100ms);
	auto delayed = activation.poll(100ms);
	CHECK_EQ(delayed.size(), 2U);
	const auto delayed_video = std::find_if(delayed.begin(), delayed.end(),
		[&](const auto &item) { return item.packet.metadata.stream == video; });
	const auto delayed_audio = std::find_if(delayed.begin(), delayed.end(),
		[&](const auto &item) { return item.packet.metadata.stream == audio; });
	CHECK(delayed_video != delayed.end());
	CHECK(delayed_audio != delayed.end());
	CHECK(delayed_video->packet.metadata.dts > 400);
	CHECK(delayed_video->packet.metadata.pts > 800);
	CHECK_EQ(delayed_video->packet.metadata.pts -
		delayed_video->packet.metadata.dts, 200);
	CHECK_EQ(delayed_video->packet.metadata.dts,
		delayed_audio->packet.metadata.dts);

	PacketDelay resize;
	begin_activation(resize, 100ms);
	CHECK(resize.push(packet(video, 200, 0, 1,
		{1, 1'000}, true), 0ms).empty());
	CHECK(resize.push(packet(video, 100, 100, 1,
		{1, 1'000}, false), 100ms).empty());
	commit_activation(resize, 100ms);
	auto high_pts = resize.poll(100ms);
	CHECK_EQ(high_pts[0].packet.metadata.pts, 300);
	auto reordered = resize.push(packet(video, 200, 200, 1,
		{1, 1'000}, true), 200ms);
	CHECK_EQ(reordered.size(), 1U);
	CHECK_EQ(reordered[0].packet.metadata.pts, 200);
	CHECK_EQ(resize.state(), DelayState::Delayed);
	CHECK_EQ(resize.request_change_delay(0ms, 200ms), RequestResult::Accepted);
	CHECK_EQ(resize.begin_filling(200ms), RequestResult::Accepted);
	commit_activation(resize, 200ms);
	auto resized = resize.poll(200ms);
	CHECK_EQ(resized.size(), 1U);
	CHECK(resized[0].packet.metadata.pts > high_pts[0].packet.metadata.pts);
	CHECK(resized[0].packet.metadata.dts > reordered[0].packet.metadata.dts);
	CHECK_EQ(resized[0].packet.metadata.pts -
		resized[0].packet.metadata.dts, 0);

	CHECK_EQ(resize.request_deactivate(200ms), RequestResult::Accepted);
	CHECK(resize.push(packet(video, 220, 210, 1,
		{1, 1'000}, true), 210ms).empty());
	CHECK(resize.ready_for_return());
	CHECK_EQ(resize.commit_return(210ms), RequestResult::Accepted);
	auto live = resize.poll(210ms);
	CHECK_EQ(live.size(), 1U);
	CHECK(live[0].packet.metadata.pts > resized[0].packet.metadata.pts);
	CHECK(live[0].packet.metadata.dts > resized[0].packet.metadata.dts);
	CHECK_EQ(live[0].packet.metadata.pts - live[0].packet.metadata.dts, 10);

	PacketDelay cancel;
	begin_activation(cancel, 100ms);
	CHECK(cancel.push(packet(video, 200, 0, 1,
		{1, 1'000}, true), 0ms).empty());
	CHECK(cancel.observe_external_output(
		{cover_video, 500, 50, 1, {1, 1'000}, true, 0}));
	CHECK_EQ(cancel.request_cancel(50ms), RequestResult::Accepted);
	CHECK(cancel.push(packet(video, 60, 60, 1,
		{1, 1'000}, true), 60ms).empty());
	CHECK(cancel.ready_for_return());
	CHECK_EQ(cancel.commit_return(60ms), RequestResult::Accepted);
	auto cancelled_live = cancel.poll(60ms);
	CHECK_EQ(cancelled_live.size(), 1U);
	CHECK(cancelled_live[0].packet.metadata.pts > 500);
	CHECK(cancelled_live[0].packet.metadata.dts > 50);
	CHECK_EQ(cancelled_live[0].packet.metadata.pts -
		cancelled_live[0].packet.metadata.dts, 0);
}

void an_uncommitted_dynamic_change_can_be_cancelled()
{
	PacketDelay delay;
	begin_activation(delay, 10ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 7}, 0, 0), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 7}, 10, 10), 10ms).empty());
	commit_activation(delay, 10ms);
	CHECK_EQ(delay.poll(10ms).size(), 1U);
	CHECK_EQ(delay.request_change_delay(50ms, 10ms), RequestResult::Accepted);
	CHECK_EQ(delay.request_cancel(10ms), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Delayed);
	CHECK_EQ(delay.target_delay(), 10ms);

	CHECK_EQ(delay.request_change_delay(50ms, 10ms), RequestResult::Accepted);
	CHECK_EQ(delay.begin_filling(10ms), RequestResult::Accepted);
	CHECK_EQ(delay.target_delay(), 50ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 7}, 20, 20), 20ms).empty());
	CHECK_EQ(delay.request_cancel(20ms), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Delayed);
	CHECK_EQ(delay.target_delay(), 10ms);
	auto resumed = delay.poll(20ms);
	CHECK(!resumed.empty());
}

void returning_waits_for_live_keyframe_and_commit()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	for (int time : {0, 50, 100})
		CHECK(delay.push(packet({StreamKind::Video, 0, 8}, time, time),
			Duration{time}).empty());
	commit_activation(delay, 100ms);
	CHECK_EQ(delay.poll(100ms).size(), 1U);
	CHECK_EQ(delay.request_deactivate(100ms), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Returning);
	CHECK(!delay.ready_for_return());

	auto during_return = delay.push(packet({StreamKind::Video, 0, 8}, 110, 110, 1,
		{1, 1'000}, false), 110ms);
	for (const auto &item : during_return)
		CHECK(item.delayed);
	CHECK(!delay.ready_for_return());
	CHECK_EQ(delay.commit_return(110ms), RequestResult::NotReady);

	auto keyframe_carrier = delay.push(packet({StreamKind::Video, 0, 8}, 120, 120, 1,
		{1, 1'000}, true), 120ms);
	for (const auto &item : keyframe_carrier)
		CHECK(item.delayed);
	CHECK(delay.ready_for_return());
	CHECK(delay.metrics(120ms).ready_for_return);
	CHECK_EQ(delay.finish_return(120ms), RequestResult::NotReady);
	CHECK_EQ(delay.commit_return(120ms), RequestResult::Accepted);
	auto live_cut = delay.poll(120ms);
	CHECK(!live_cut.empty());
	CHECK(!live_cut[0].delayed);
	CHECK_EQ(live_cut[0].packet.metadata.stream.kind, StreamKind::Video);
	CHECK(live_cut[0].packet.metadata.keyframe);
	auto live = delay.push(packet({StreamKind::Video, 0, 8}, 130, 130, 1,
		{1, 1'000}, false), 130ms);
	CHECK_EQ(live.size(), 1U);
	CHECK(!live[0].delayed);
	CHECK_EQ(delay.finish_return(130ms), RequestResult::Accepted);
	CHECK_EQ(delay.state(), DelayState::Bypass);
}

void return_aligns_audio_at_or_after_the_video_cut()
{
	PacketDelay delay;
	begin_activation(delay, 100ms);
	for (int time : {0, 50, 100}) {
		CHECK(delay.push(packet({StreamKind::Video, 0, 21}, time, time),
			Duration{time}).empty());
		CHECK(delay.push(packet({StreamKind::Audio, 0, 21}, time, time),
			Duration{time}).empty());
	}
	commit_activation(delay, 100ms);
	CHECK_EQ(delay.poll(100ms).size(), 2U);
	CHECK_EQ(delay.request_deactivate(100ms), RequestResult::Accepted);
	(void)delay.push(packet({StreamKind::Audio, 0, 21}, 110, 110), 110ms);
	(void)delay.push(packet({StreamKind::Video, 0, 21}, 120, 120, 1,
		{1, 1'000}, true), 120ms);
	(void)delay.push(packet({StreamKind::Audio, 0, 21}, 120, 120), 120ms);
	CHECK(delay.ready_for_return());
	CHECK_EQ(delay.commit_return(120ms), RequestResult::Accepted);
	auto live = delay.poll(120ms);
	CHECK_EQ(live.size(), 2U);
	for (const auto &item : live)
		CHECK(item.received_at >= 120ms);
	const auto video = std::find_if(live.begin(), live.end(), [](const auto &item) {
		return item.packet.metadata.stream.kind == StreamKind::Video;
	});
	CHECK(video != live.end());
	CHECK(video->packet.metadata.keyframe);
}

void multi_video_return_protects_each_keyframe_until_commit()
{
	const StreamKey video_a{StreamKind::Video, 0, 23};
	const StreamKey video_b{StreamKind::Video, 1, 23};
	const StreamKey audio{StreamKind::Audio, 0, 23};
	DelayConfig config;
	config.required_streams = {video_a, video_b, audio};
	PacketDelay delay{config};
	std::int64_t last_dts = std::numeric_limits<std::int64_t>::min();
	const auto observe_monotonic = [&](const std::vector<OutputPacket> &items) {
		for (const auto &item : items) {
			CHECK(item.packet.metadata.dts >= last_dts);
			CHECK_EQ(item.packet.metadata.pts - item.packet.metadata.dts, 0);
			last_dts = item.packet.metadata.dts;
		}
	};
	begin_activation(delay, 50ms);
	for (int time : {0, 50}) {
		CHECK(delay.push(packet(video_a, time, time), Duration{time}).empty());
		CHECK(delay.push(packet(video_b, time, time), Duration{time}).empty());
		CHECK(delay.push(packet(audio, time, time), Duration{time}).empty());
	}
	commit_activation(delay, 50ms);
	auto initial = delay.poll(50ms);
	CHECK_EQ(initial.size(), 3U);
	observe_monotonic(initial);
	CHECK_EQ(delay.request_deactivate(50ms), RequestResult::Accepted);
	observe_monotonic(delay.push(packet(video_a, 60, 60, 1,
		{1, 1'000}, true), 60ms));
	observe_monotonic(delay.push(packet(audio, 60, 60), 60ms));
	CHECK(!delay.ready_for_return());
	// A's early keyframe is consumed so the delayed branch keeps moving while B
	// is missing; the return transaction then waits for A's next keyframe.
	observe_monotonic(delay.poll(120ms));
	observe_monotonic(delay.push(packet(video_b, 120, 120, 1,
		{1, 1'000}, true), 120ms));
	observe_monotonic(delay.push(packet(audio, 120, 120), 120ms));
	CHECK(!delay.ready_for_return());
	observe_monotonic(delay.push(packet(video_a, 130, 130, 1,
		{1, 1'000}, true), 130ms));
	observe_monotonic(delay.push(packet(audio, 130, 130), 130ms));
	CHECK(delay.ready_for_return());
	CHECK_EQ(delay.commit_return(130ms), RequestResult::Accepted);
	auto live = delay.poll(130ms);
	observe_monotonic(live);
	std::unordered_map<StreamKey, bool, StreamKeyHash> first_video_is_keyframe;
	for (const auto &item : live) {
		if (item.packet.metadata.stream.kind == StreamKind::Video &&
			!first_video_is_keyframe.contains(item.packet.metadata.stream))
			first_video_is_keyframe[item.packet.metadata.stream] =
				item.packet.metadata.keyframe;
	}
	CHECK_EQ(first_video_is_keyframe.size(), 2U);
	CHECK(first_video_is_keyframe.at(video_a));
	CHECK(first_video_is_keyframe.at(video_b));

	auto live_continuation = delay.push(packet(video_a, 140, 140, 1,
		{1, 1'000}, false), 140ms);
	CHECK_EQ(live_continuation.size(), 1U);
	observe_monotonic(live_continuation);
	CHECK_EQ(delay.finish_return(140ms), RequestResult::Accepted);
	auto bypass = delay.push(packet(video_a, 150, 150, 1,
		{1, 1'000}, false), 150ms);
	CHECK_EQ(bypass.size(), 1U);
	observe_monotonic(bypass);
	const StreamKey next_generation{StreamKind::Video, 0, 24};
	auto regenerated = delay.push(packet(next_generation, 0, 0), 151ms);
	CHECK_EQ(regenerated.size(), 1U);
	observe_monotonic(regenerated);
}

void stalled_return_lanes_and_new_optional_video_do_not_deadlock()
{
	const StreamKey video_a{StreamKind::Video, 0, 25};
	const StreamKey video_b{StreamKind::Video, 1, 25};
	const StreamKey optional_video{StreamKind::Video, 2, 25};
	DelayConfig config;
	config.required_streams = {video_a, video_b};
	config.stream_stall_timeout = 20ms;
	PacketDelay delay{config};
	begin_activation(delay, 50ms);
	for (int time : {0, 50}) {
		CHECK(delay.push(packet(video_a, time, time), Duration{time}).empty());
		CHECK(delay.push(packet(video_b, time, time), Duration{time}).empty());
	}
	commit_activation(delay, 50ms);
	CHECK_EQ(delay.poll(50ms).size(), 2U);
	CHECK_EQ(delay.request_deactivate(50ms), RequestResult::Accepted);
	(void)delay.push(packet(optional_video, 61, 61, 1,
		{1, 1'000}, false), 61ms);
	(void)delay.push(packet(video_a, 62, 62, 1,
		{1, 1'000}, true), 62ms);
	CHECK(!delay.ready_for_return());
	CHECK(delay.ready_for_return(71ms));
	CHECK_EQ(delay.commit_return(71ms), RequestResult::Accepted);
	auto live = delay.poll(71ms);
	for (const auto &item : live)
		CHECK(item.packet.metadata.stream != optional_video);
}

void return_without_keyframe_requirement_preserves_the_live_cut()
{
	const StreamKey video{StreamKind::Video, 0, 26};
	const StreamKey audio{StreamKind::Audio, 0, 26};
	DelayConfig config;
	config.require_video_keyframe = false;
	config.required_streams = {video, audio};
	PacketDelay delay{config};
	begin_activation(delay, 10ms);
	for (int time : {0, 10}) {
		CHECK(delay.push(packet(video, time, time, 1,
			{1, 1'000}, false), Duration{time}).empty());
		CHECK(delay.push(packet(audio, time, time), Duration{time}).empty());
	}
	commit_activation(delay, 10ms);
	CHECK_EQ(delay.poll(10ms).size(), 2U);
	CHECK_EQ(delay.request_deactivate(10ms), RequestResult::Accepted);
	CHECK(delay.ready_for_return(10ms));
	CHECK(delay.push(packet(video, 20, 20, 1,
		{1, 1'000}, false), 20ms).empty());
	CHECK(delay.push(packet(audio, 20, 20), 20ms).empty());
	CHECK_EQ(delay.commit_return(20ms), RequestResult::Accepted);
	auto live = delay.poll(20ms);
	CHECK_EQ(live.size(), 4U);
	bool saw_video = false;
	bool saw_audio = false;
	for (const auto &item : live) {
		CHECK(item.received_at >= 10ms);
		saw_video = saw_video || item.packet.metadata.stream == video;
		saw_audio = saw_audio || item.packet.metadata.stream == audio;
	}
	CHECK(saw_video);
	CHECK(saw_audio);
}

void cancel_uses_the_same_transactional_return_protocol()
{
	PacketDelay preparing;
	CHECK_EQ(preparing.request_activate(1s, 0ms), RequestResult::Accepted);
	CHECK_EQ(preparing.request_cancel(0ms), RequestResult::Accepted);
	CHECK(preparing.ready_for_return());
	CHECK_EQ(preparing.commit_return(0ms), RequestResult::Accepted);
	CHECK_EQ(preparing.finish_return(0ms), RequestResult::Accepted);

	PacketDelay filling;
	begin_activation(filling, 1s);
	CHECK(filling.push(packet({StreamKind::Video, 0, 9}, 0, 0), 0ms).empty());
	CHECK_EQ(filling.request_cancel(10ms), RequestResult::Accepted);
	CHECK(!filling.ready_for_return());
	CHECK(filling.push(packet({StreamKind::Video, 0, 9}, 11, 11, 1,
		{1, 1'000}, true), 11ms).empty());
	CHECK(filling.ready_for_return());
	CHECK_EQ(filling.commit_return(11ms), RequestResult::Accepted);
	CHECK_EQ(filling.poll(11ms).size(), 1U);
	CHECK_EQ(filling.finish_return(11ms), RequestResult::Accepted);
}

void temporal_pruning_keeps_a_decodable_video_front()
{
	DelayConfig config;
	config.max_delay = 1s;
	config.retention_headroom = 20ms;
	PacketDelay delay{config};
	begin_activation(delay, 100ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 0, 0, 2,
		{1, 1'000}, true), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 10, 10, 2,
		{1, 1'000}, false), 10ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 50, 50, 2,
		{1, 1'000}, true), 50ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 60, 60, 2,
		{1, 1'000}, false), 60ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 150, 150, 2,
		{1, 1'000}, false), 150ms).empty());
	delay.prune(200ms);
	CHECK_EQ(delay.metrics(200ms).buffered_packets, 3U);
	CHECK_EQ(delay.metrics(200ms).pruned_retention_packets, 2U);
	CHECK(delay.ready_for_delayed(200ms));
	commit_activation(delay, 200ms);
	auto output = delay.poll(200ms);
	CHECK(!output.empty());
	CHECK(output[0].packet.metadata.keyframe);
	CHECK_EQ(output[0].received_at, 50ms);
}

void capacity_limits_include_payload_metadata_and_owner_memory()
{
	DelayConfig payload_config;
	payload_config.max_payload_bytes = 5;
	payload_config.max_retained_bytes = 1'000;
	payload_config.metadata_bytes_per_packet = 1;
	payload_config.max_buffered_packets = 10;
	PacketDelay oversized{payload_config};
	begin_activation(oversized, 1s);
	auto fail_open = oversized.push(packet({StreamKind::Video, 0, 11}, 0, 0, 6), 0ms);
	CHECK_EQ(oversized.state(), DelayState::Error);
	CHECK_EQ(fail_open.size(), 1U);
	CHECK(!fail_open[0].delayed);
	CHECK_EQ(oversized.metrics(0ms).rejected_capacity_packets, 1U);
	CHECK_EQ(oversized.metrics(0ms).buffered_packets, 0U);

	DelayConfig packet_config;
	packet_config.max_buffered_packets = 2;
	packet_config.metadata_bytes_per_packet = 64;
	packet_config.max_retained_bytes = 1'000;
	PacketDelay metadata_only{packet_config};
	begin_activation(metadata_only, 1s);
	CHECK(metadata_only.push(packet({StreamKind::Audio, 0, 0}, 0, 0, 0), 0ms).empty());
	CHECK(metadata_only.push(packet({StreamKind::Audio, 0, 0}, 1, 1, 0), 1ms).empty());
	auto third = metadata_only.push(packet({StreamKind::Audio, 0, 0}, 2, 2, 0), 2ms);
	CHECK_EQ(metadata_only.state(), DelayState::Error);
	CHECK_EQ(third.size(), 1U);

	DelayConfig retained_config;
	retained_config.max_buffered_packets = 10;
	retained_config.max_payload_bytes = 50;
	retained_config.max_retained_bytes = 200;
	retained_config.metadata_bytes_per_packet = 1;
	PacketDelay retained{retained_config};
	begin_activation(retained, 1s);
	auto owner = std::make_shared<std::array<std::byte, 100>>();
	Packet alias_packet{{{StreamKind::Video, 0, 0}, 0, 0, 1, {1, 1'000}, true, 0},
		SharedPayload::alias(owner, owner->data(), 1, owner->size())};
	auto alias_output = retained.push(std::move(alias_packet), 0ms);
	CHECK_EQ(retained.state(), DelayState::Error);
	CHECK_EQ(alias_output.size(), 1U);
}

void invalid_timebase_and_timestamp_overflow_fail_open()
{
	PacketDelay external;
	CHECK(!external.observe_external_output(
		{{StreamKind::Video, 0, 1}, 0, 0, 1, {0, 1}, true, 0}));
	CHECK_EQ(external.state(), DelayState::Error);
	CHECK_EQ(external.recover(0ms), RequestResult::Accepted);
	CHECK(external.observe_external_output(
		{{StreamKind::Video, 0, 1}, 10, 10, 1, {1, 1'000}, true, 0}));
	CHECK(!external.observe_external_output(
		{{StreamKind::Video, 0, 1}, 9, 9, 1, {1, 1'000}, false, 0}));
	CHECK_EQ(external.state(), DelayState::Error);

	PacketDelay exact_order;
	CHECK(exact_order.observe_external_output({{StreamKind::Audio, 0, 1},
		9'007'199'254'740'993LL, 9'007'199'254'740'993LL, 1,
		{1, 1}, true, 0}));
	CHECK(!exact_order.observe_external_output({{StreamKind::Audio, 1, 1},
		9'007'199'254'740'992LL, 9'007'199'254'740'992LL, 1,
		{2, 2}, true, 0}));
	CHECK_EQ(exact_order.state(), DelayState::Error);

	PacketDelay invalid;
	begin_activation(invalid, 10ms);
	auto invalid_output = invalid.push(packet({StreamKind::Video, 0, 0}, 0, 0, 1,
		{0, 90'000}), 0ms);
	CHECK_EQ(invalid.state(), DelayState::Error);
	CHECK_EQ(invalid_output.size(), 1U);
	CHECK_EQ(invalid.recover(1ms), RequestResult::Accepted);

	PacketDelay overflow;
	begin_activation(overflow, 1ms);
	CHECK(overflow.push(packet({StreamKind::Video, 0, 0},
		std::numeric_limits<std::int64_t>::max(),
		std::numeric_limits<std::int64_t>::max()), 0ms).empty());
	CHECK(overflow.push(packet({StreamKind::Video, 0, 0},
		std::numeric_limits<std::int64_t>::max(),
		std::numeric_limits<std::int64_t>::max()), 1ms).empty());
	CHECK(overflow.ready_for_delayed(1ms));
	CHECK_EQ(overflow.commit_delayed(1ms), RequestResult::InvalidState);
	CHECK_EQ(overflow.state(), DelayState::Error);

	DelayConfig conversion_config;
	conversion_config.max_delay = 1s;
	conversion_config.max_stream_gap = 0ms;
	PacketDelay conversion{conversion_config};
	begin_activation(conversion, 1s);
	const TimeBase unrepresentable_ticks{1,
		std::numeric_limits<std::int64_t>::max()};
	CHECK(conversion.push(packet({StreamKind::Audio, 0, 2}, 0, 0, 1,
		unrepresentable_ticks), 0ms).empty());
	CHECK(conversion.push(packet({StreamKind::Audio, 0, 2}, 1, 1, 1,
		unrepresentable_ticks), 1s).empty());
	CHECK(conversion.ready_for_delayed(1s));
	CHECK_EQ(conversion.commit_delayed(1s), RequestResult::InvalidState);
	CHECK_EQ(conversion.state(), DelayState::Error);

	PacketDelay prefix;
	begin_activation(prefix, 100ms);
	CHECK(prefix.push(packet({StreamKind::Video, 0, 3}, 0, 0, 1,
		{1, 1'000}, true), 0ms).empty());
	CHECK(prefix.push(packet({StreamKind::Video, 0, 3},
		std::numeric_limits<std::int64_t>::max(), 1, 1,
		{1, 1'000}, false), 1ms).empty());
	CHECK(prefix.push(packet({StreamKind::Video, 0, 3}, 100, 100, 1,
		{1, 1'000}, false), 100ms).empty());
	commit_activation(prefix, 200ms);
	auto valid_prefix = prefix.poll(200ms);
	CHECK_EQ(valid_prefix.size(), 1U);
	CHECK_EQ(valid_prefix[0].packet.metadata.dts, 100);
	CHECK_EQ(prefix.state(), DelayState::Error);
}

void clock_arithmetic_is_checked_at_extremes()
{
	PacketDelay regression;
	CHECK_EQ(regression.request_activate(10ms, 100ms), RequestResult::Accepted);
	CHECK_EQ(regression.begin_filling(110ms), RequestResult::Accepted);
	CHECK(regression.push(packet({StreamKind::Video, 0, 0}, 0, 0), 120ms).empty());
	CHECK(regression.poll(119ms).empty());
	CHECK_EQ(regression.state(), DelayState::Error);

	DelayConfig config;
	config.max_delay = 1ms;
	config.retention_headroom = 0ms;
	config.max_stream_gap = 0ms;
	PacketDelay extremes{config};
	const TimePoint minimum{std::numeric_limits<TimePoint::rep>::min()};
	const TimePoint maximum{std::numeric_limits<TimePoint::rep>::max()};
	begin_activation(extremes, 1ms, minimum);
	CHECK(extremes.push(packet({StreamKind::Video, 0, 0}, 0, 0), minimum).empty());
	CHECK(!extremes.ready_for_delayed(minimum));
	CHECK(extremes.push(packet({StreamKind::Video, 0, 0}, 1, 1), maximum).empty());
	CHECK_EQ(extremes.metrics(maximum).fill_progress, 1.0);
	CHECK(extremes.ready_for_delayed(maximum));
	CHECK_EQ(extremes.commit_delayed(maximum), RequestResult::Accepted);
	CHECK_EQ(extremes.poll(maximum).size(), 1U);
}

void zero_delay_still_requires_ready_then_commit()
{
	PacketDelay delay;
	begin_activation(delay, 0ms);
	CHECK(delay.push(packet({StreamKind::Video, 0, 0}, 9, 7), 0ms).empty());
	CHECK_EQ(delay.state(), DelayState::Filling);
	CHECK(delay.ready_for_delayed(0ms));
	CHECK(delay.poll(0ms).empty());
	commit_activation(delay, 0ms);
	auto output = delay.poll(0ms);
	CHECK_EQ(output.size(), 1U);
	CHECK_EQ(output[0].packet.metadata.pts, 9);
	CHECK_EQ(output[0].packet.metadata.dts, 7);
}

void validation_and_stream_metrics_are_deterministic()
{
	check_throws<std::invalid_argument>([] {
		DelayConfig config;
		config.max_buffered_packets = 0;
		(void)PacketDelay{config};
	});
	check_throws<std::invalid_argument>([] {
		DelayConfig config;
		config.metadata_bytes_per_packet = 0;
		(void)PacketDelay{config};
	});
	check_throws<std::invalid_argument>([] {
		DelayConfig config;
		config.max_streams = 0;
		(void)PacketDelay{config};
	});

	DelayConfig bounded_streams_config;
	bounded_streams_config.max_streams = 1;
	PacketDelay bounded_streams{bounded_streams_config};
	CHECK_EQ(bounded_streams.push(packet({StreamKind::Audio, 0, 1},
		0, 0), 0ms).size(), 1U);
	auto next_generation = bounded_streams.push(packet({StreamKind::Audio, 0, 2},
		0, 0), 0ms);
	CHECK_EQ(next_generation.size(), 1U);
	CHECK_EQ(bounded_streams.state(), DelayState::Bypass);
	auto excess_track = bounded_streams.push(packet({StreamKind::Audio, 1, 1},
		0, 0), 0ms);
	CHECK_EQ(excess_track.size(), 1U);
	CHECK_EQ(bounded_streams.state(), DelayState::Error);

	PacketDelay delay{DelayConfig{100ms, 10ms}};
	CHECK_EQ(delay.request_activate(-1ms, 0ms), RequestResult::InvalidDelay);
	CHECK_EQ(delay.request_activate(101ms, 0ms), RequestResult::InvalidDelay);
	begin_activation(delay, 50ms);
	CHECK(delay.push(packet({StreamKind::Audio, 1, 10}, 0, 0, 3), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Audio, 0, 11}, 0, 0, 4), 0ms).empty());
	CHECK(delay.push(packet({StreamKind::Video, 0, 10}, 0, 0, 2), 0ms).empty());
	const auto streams = delay.stream_metrics(10ms);
	CHECK_EQ(streams.size(), 3U);
	CHECK_EQ(streams[0].stream.kind, StreamKind::Video);
	CHECK_EQ(streams[1].stream.generation, 11U);
	CHECK_EQ(streams[2].stream.track, 1U);
	CHECK_EQ(delay.request_activate(50ms, 10ms), RequestResult::InvalidState);
}

void estimate_and_payload_lifetime_are_accounted()
{
	CHECK_EQ(estimate_payload_bytes(8'000'000, 30s), 30'000'000U);
	CHECK_EQ(estimate_payload_bytes(1, 1ms), 1U);
	CHECK_EQ(estimate_payload_bytes(0, 1s), 0U);
	CHECK_EQ(estimate_payload_bytes(std::numeric_limits<std::uint64_t>::max(),
		Duration{std::numeric_limits<Duration::rep>::max()}),
		std::numeric_limits<std::size_t>::max());

	struct Owner {
		std::array<std::byte, 2> bytes{};
	};
	PacketDelay delay;
	begin_activation(delay, 1s);
	auto owner = std::make_shared<Owner>();
	std::weak_ptr<Owner> weak = owner;
	Packet input{{{StreamKind::Video, 0, 0}, 0, 0, 1, {1, 1'000}, true, 0},
		SharedPayload::alias(owner, owner->bytes.data(), owner->bytes.size())};
	CHECK(delay.push(std::move(input), 0ms).empty());
	owner.reset();
	CHECK(!weak.expired());
	delay.report_error("test cleanup");
	CHECK(weak.expired());
}

void concurrent_bypass_producers_are_safe()
{
	PacketDelay delay;
	constexpr int thread_count = 4;
	constexpr int packets_per_thread = 500;
	std::atomic<int> outputs{0};
	std::vector<std::thread> threads;
	for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
		threads.emplace_back([&, thread_index] {
			for (int index = 0; index < packets_per_thread; ++index) {
				auto result = delay.push(packet({StreamKind::Audio,
					static_cast<std::uint32_t>(thread_index), 0}, index, index, 0), 0ms);
				if (result.size() == 1 && !result[0].delayed)
					++outputs;
			}
		});
	}
	for (auto &thread : threads)
		thread.join();
	CHECK_EQ(outputs.load(), thread_count * packets_per_thread);
	CHECK_EQ(delay.metrics(0ms).bypassed_packets,
		static_cast<std::uint64_t>(thread_count * packets_per_thread));
}

struct TestCase {
	const char *name;
	void (*function)();
};

} // namespace

int main()
{
	const std::vector<TestCase> tests{
		{"shared payload retained bytes", shared_payload_tracks_real_retained_memory},
		{"bypass", bypass_never_buffers_or_retimestamps},
		{"coverage and commit", filling_requires_real_coverage_and_explicit_commit},
		{"all stream coverage", every_observed_stream_must_cover_the_target},
		{"required roster and gaps", explicit_roster_blocks_missing_or_gapped_streams},
		{"keyframe delayed cut", delayed_cut_starts_each_video_lane_on_a_keyframe},
		{"persistent DTS watermark", persistent_watermark_orders_across_separate_push_calls},
		{"timestamp invariants", timestamps_shift_once_and_preserve_pts_minus_dts},
		{"dynamic target", delay_can_increase_and_decrease_while_active},
		{"timestamp resize epoch", timestamp_epoch_absorbs_gop_overshoot_on_delay_reduction},
		{"B-frame epoch horizons", b_frame_reorder_horizons_are_cleared_at_every_epoch},
		{"cancel dynamic request", an_uncommitted_dynamic_change_can_be_cancelled},
		{"keyframe live return", returning_waits_for_live_keyframe_and_commit},
		{"audio-aligned live return", return_aligns_audio_at_or_after_the_video_cut},
		{"multi-video live return", multi_video_return_protects_each_keyframe_until_commit},
		{"stalled and optional return lanes", stalled_return_lanes_and_new_optional_video_do_not_deadlock},
		{"return without keyframes", return_without_keyframe_requirement_preserves_the_live_cut},
		{"transactional cancel", cancel_uses_the_same_transactional_return_protocol},
		{"GOP-safe pruning", temporal_pruning_keeps_a_decodable_video_front},
		{"bounded memory", capacity_limits_include_payload_metadata_and_owner_memory},
		{"invalid timestamps", invalid_timebase_and_timestamp_overflow_fail_open},
		{"clock extremes", clock_arithmetic_is_checked_at_extremes},
		{"zero delay commit", zero_delay_still_requires_ready_then_commit},
		{"validation and streams", validation_and_stream_metrics_are_deterministic},
		{"estimate and lifetime", estimate_and_payload_lifetime_are_accounted},
		{"concurrent producers", concurrent_bypass_producers_are_safe},
	};

	std::size_t passed = 0;
	for (const auto &test : tests) {
		try {
			test.function();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
		} catch (...) {
			std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
		}
	}

	std::cout << passed << '/' << tests.size() << " tests passed\n";
	return passed == tests.size() ? 0 : 1;
}
