#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace dynamic_delay::core {

inline constexpr uint64_t PreviewIntervalNs = 500'000'000ULL;

inline constexpr int preview_output_priority(const std::string_view label) noexcept
{
	if (label == "Streaming")
		return 3;
	if (label == "Multistream")
		return 2;
	if (label == "Recording")
		return 1;
	return 0;
}

// Output timing vectors originate in an unordered session map. Select one
// audience explicitly so an unrelated recording pause cannot suspend a live
// multistream preview. The controller and widget must use the same policy.
template<class Iterator> Iterator preview_output(Iterator first, Iterator last)
{
	return std::max_element(first, last, [](const auto &left, const auto &right) {
		return preview_output_priority(left.label) < preview_output_priority(right.label);
	});
}

// The render callback runs once per OBS video mix, not just once per frame.
// Sampling before readback bounds GPU transfers even with auxiliary views.
class PreviewCadence {
public:
	bool begin_frame(uint64_t timestampNs) noexcept
	{
		if (timestampNs == 0 || timestampNs == lastFrameNs_)
			return false;
		if (timestampNs < lastFrameNs_)
			lastSampleNs_ = 0;
		lastFrameNs_ = timestampNs;
		return true;
	}

	bool sample_due(uint64_t timestampNs) const noexcept
	{
		return lastSampleNs_ == 0 ||
		       (timestampNs >= lastSampleNs_ && timestampNs - lastSampleNs_ >= PreviewIntervalNs);
	}

	void sampled(uint64_t timestampNs) noexcept { lastSampleNs_ = timestampNs; }

private:
	uint64_t lastFrameNs_ = 0;
	uint64_t lastSampleNs_ = 0;
};

inline uint64_t preview_delay_ns(double seconds) noexcept
{
	// Sanitize telemetry before conversion to an unsigned timestamp.
	if (!std::isfinite(seconds) || seconds <= 0.0)
		return 0;
	return static_cast<uint64_t>(std::min(seconds, 3600.0) * 1'000'000'000.0);
}

inline uint64_t preview_target_ns(uint64_t nowNs, double effectiveSeconds) noexcept
{
	return nowNs - std::min(nowNs, preview_delay_ns(effectiveSeconds));
}

inline uint32_t preview_history_seconds(uint32_t configuredSeconds, double effectiveSeconds) noexcept
{
	const uint64_t effectiveNs = preview_delay_ns(effectiveSeconds);
	const uint64_t roundedSeconds = (effectiveNs + 999'999'999ULL) / 1'000'000'000ULL;
	return std::max(configuredSeconds, static_cast<uint32_t>(roundedSeconds)) + 3U;
}

} // namespace dynamic_delay::core
