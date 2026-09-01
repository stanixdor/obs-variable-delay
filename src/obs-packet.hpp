#pragma once

#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dynamic_delay {

class ObsPacket {
public:
	ObsPacket() = default;

	ObsPacket(encoder_packet *source, const uint64_t capturedAtNs)
		: capturedAtNs_(capturedAtNs),
		  valid_(source != nullptr)
	{
		if (source)
			obs_encoder_packet_ref(&packet_, source);
	}

	~ObsPacket() { reset(); }

	ObsPacket(const ObsPacket &) = delete;
	ObsPacket &operator=(const ObsPacket &) = delete;

	ObsPacket(ObsPacket &&other) noexcept { *this = std::move(other); }

	ObsPacket &operator=(ObsPacket &&other) noexcept
	{
		if (this == &other)
			return *this;
		reset();
		packet_ = other.packet_;
		capturedAtNs_ = other.capturedAtNs_;
		valid_ = other.valid_;
		other.packet_ = {};
		other.capturedAtNs_ = 0;
		other.valid_ = false;
		return *this;
	}

	void reset() noexcept
	{
		if (valid_)
			obs_encoder_packet_release(&packet_);
		packet_ = {};
		capturedAtNs_ = 0;
		valid_ = false;
	}

	[[nodiscard]] bool valid() const noexcept { return valid_; }
	[[nodiscard]] std::size_t size() const noexcept { return valid_ ? packet_.size : 0; }
	[[nodiscard]] uint64_t captured_at_ns() const noexcept { return capturedAtNs_; }
	[[nodiscard]] bool keyframe() const noexcept { return valid_ && packet_.keyframe; }
	[[nodiscard]] encoder_packet *get() noexcept { return valid_ ? &packet_ : nullptr; }
	[[nodiscard]] const encoder_packet *get() const noexcept { return valid_ ? &packet_ : nullptr; }

private:
	encoder_packet packet_{};
	uint64_t capturedAtNs_ = 0;
	bool valid_ = false;
};

inline void replace_packet_payload(encoder_packet *carrier, ObsPacket &replacement)
{
	if (!carrier || !replacement.valid())
		return;

	const int64_t carrierDts = carrier->dts;
	const int64_t carrierDtsUsec = carrier->dts_usec;
	const int64_t carrierSystemDtsUsec = carrier->sys_dts_usec;
	const int32_t carrierTimebaseNum = carrier->timebase_num;
	const int32_t carrierTimebaseDen = carrier->timebase_den;
	const enum obs_encoder_type carrierType = carrier->type;
	const size_t carrierTrack = carrier->track_idx;
	obs_encoder_t *const carrierEncoder = carrier->encoder;

	const encoder_packet *payload = replacement.get();
	const int64_t compositionOffset = payload->pts - payload->dts;
	encoder_packet referenced{};
	obs_encoder_packet_ref(&referenced, const_cast<encoder_packet *>(payload));
	obs_encoder_packet_release(carrier);
	*carrier = referenced;

	carrier->dts = carrierDts;
	carrier->pts = carrierDts + compositionOffset;
	carrier->dts_usec = carrierDtsUsec;
	carrier->sys_dts_usec = carrierSystemDtsUsec;
	carrier->timebase_num = carrierTimebaseNum;
	carrier->timebase_den = carrierTimebaseDen;
	carrier->type = carrierType;
	carrier->track_idx = carrierTrack;
	carrier->encoder = carrierEncoder;
}

} // namespace dynamic_delay
