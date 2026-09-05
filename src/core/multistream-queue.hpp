#pragma once

#include "multistream-transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>

namespace dynamic_delay::core {

// Caller supplies synchronization. Queue operations hold references to the
// one immutable payload allocation shared by every destination, never copy it.
class MultistreamQueue final {
public:
	struct Limits {
		std::size_t bytes = 8U * 1024U * 1024U;
		std::size_t packets = 512;
		uint64_t durationUsec = 2'000'000;
	};
	enum class PushResult { Accepted, WaitingForKeyframe, BeforeEpoch, Overflow, Invalid };

	MultistreamQueue() = default;
	explicit MultistreamQueue(const Limits limits) : limits_(limits) {}

	void reset() noexcept
	{
		packets_.clear();
		bytes_ = 0;
		waitingForKeyframe_ = true;
		epochDtsUsec_ = 0;
	}

	PushResult push(const SharedEncodedPacket &packet, const uint64_t nowUsec)
	{
		if (!packet.data || packet.data->empty())
			return PushResult::Invalid;
		if (waitingForKeyframe_ && (!packet.video || !packet.keyframe))
			return PushResult::WaitingForKeyframe;
		if (!waitingForKeyframe_ && packet.dtsUsec < epochDtsUsec_)
			return PushResult::BeforeEpoch;
		const bool oversized = packet.data->size() > limits_.bytes - std::min(bytes_, limits_.bytes);
		const bool full = packets_.size() >= limits_.packets;
		const bool expired =
			!packets_.empty() &&
			(nowUsec < packets_.front().enqueuedUsec ||
			 nowUsec - packets_.front().enqueuedUsec > limits_.durationUsec ||
			 media_distance(packet.dtsUsec, packets_.front().packet.dtsUsec) > limits_.durationUsec);
		if (oversized || full || expired) {
			reset();
			return PushResult::Overflow;
		}
		if (waitingForKeyframe_) {
			epochDtsUsec_ = packet.dtsUsec;
			waitingForKeyframe_ = false;
		}
		packets_.push_back({packet, nowUsec});
		bytes_ += packet.data->size();
		return PushResult::Accepted;
	}

	bool pop(SharedEncodedPacket &packet, const uint64_t nowUsec)
	{
		if (packets_.empty())
			return false;
		// A packet can become stale even if no producer calls push again.
		if (nowUsec < packets_.front().enqueuedUsec ||
		    nowUsec - packets_.front().enqueuedUsec > limits_.durationUsec) {
			reset();
			return false;
		}
		packet = std::move(packets_.front().packet);
		bytes_ -= packet.data->size();
		packets_.pop_front();
		return true;
	}

	[[nodiscard]] bool empty() const noexcept { return packets_.empty(); }
	[[nodiscard]] bool waiting_for_keyframe() const noexcept { return waitingForKeyframe_; }
	[[nodiscard]] int64_t epoch_dts_usec() const noexcept { return epochDtsUsec_; }
	[[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
	[[nodiscard]] std::size_t size() const noexcept { return packets_.size(); }

private:
	static uint64_t media_distance(const int64_t newer, const int64_t older) noexcept
	{
		if (newer <= older)
			return 0;
		// Ordered subtraction through unsigned integers also covers epochs
		// with negative encoder priming timestamps without signed overflow.
		return static_cast<uint64_t>(newer) - static_cast<uint64_t>(older);
	}
	struct Entry {
		SharedEncodedPacket packet;
		uint64_t enqueuedUsec = 0;
	};
	Limits limits_;
	std::deque<Entry> packets_;
	std::size_t bytes_ = 0;
	bool waitingForKeyframe_ = true;
	int64_t epochDtsUsec_ = 0;
};

} // namespace dynamic_delay::core
