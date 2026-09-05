#include "core/preview-timing.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace dynamic_delay::core;

namespace {
void check(const bool value, const char *description)
{
	if (!value)
		throw std::runtime_error(description);
}

void duplicate_views_do_not_capture_multiple_times()
{
	PreviewCadence cadence;
	check(!cadence.begin_frame(0), "zero timestamps must not capture");
	for (uint64_t time = 1'000'000'000ULL; time <= 3'000'000'000ULL; time += PreviewIntervalNs) {
		check(cadence.begin_frame(time), "first video mix should visit the frame");
		check(cadence.sample_due(time), "frame should be due after half a second");
		cadence.sampled(time);
		for (int auxiliaryView = 0; auxiliaryView < 8; ++auxiliaryView)
			check(!cadence.begin_frame(time), "auxiliary views must not duplicate readback");
	}
}

void frame_rates_keep_gpu_readback_bounded()
{
	for (const auto &[numerator, denominator] :
	     {std::pair{60ULL, 1ULL}, std::pair{60'000ULL, 1'001ULL}, std::pair{1ULL, 1ULL}}) {
		PreviewCadence cadence;
		uint64_t lastSample = 0;
		std::size_t samples = 0;
		for (uint64_t frame = 0; frame < 600; ++frame) {
			const uint64_t time = 1'000'000'000ULL + frame * 1'000'000'000ULL * denominator / numerator;
			check(cadence.begin_frame(time), "a new frame must be observable");
			if (!cadence.sample_due(time))
				continue;
			check(lastSample == 0 || time - lastSample >= PreviewIntervalNs,
			      "readback must not exceed one sample per 500 ms");
			cadence.sampled(time);
			lastSample = time;
			++samples;
		}
		check(samples >= 19, "2 fps should continue sampling at 60/59.94 fps");
		if (numerator == 1)
			check(samples == 600, "low-fps sources must not skip useful available frames");
		else
			check(samples <= 21, "600 frames at 60/59.94 fps must not trigger excess readbacks");
	}
}

void clock_regression_restarts_sampling_without_underflow()
{
	PreviewCadence cadence;
	check(cadence.begin_frame(10'000'000'000ULL), "initial frame");
	cadence.sampled(10'000'000'000ULL);
	check(cadence.begin_frame(100'000'000ULL), "a new OBS clock epoch must be accepted");
	check(cadence.sample_due(100'000'000ULL), "sampling should resume immediately in a new clock epoch");
	cadence.sampled(100'000'000ULL);
	check(!cadence.sample_due(99'000'000ULL), "backward times cannot pass by unsigned underflow");
	check(!cadence.sample_due(599'999'999ULL), "half-second interval remains enforced");
	check(cadence.sample_due(600'000'000ULL), "interval boundary is inclusive");
}

void active_timing_is_independent_of_slider_and_keeps_history()
{
	constexpr uint64_t now = 100'000'000'000ULL;
	const uint32_t editedSlider = 60;
	const double currentlyEmittedDelay = 30.0;
	check(preview_target_ns(now, currentlyEmittedDelay) == 70'000'000'000ULL,
	      "editing slider must not retime the audience image before output changes");
	check(preview_history_seconds(editedSlider, currentlyEmittedDelay) == 63,
	      "increasing slider must reserve future history");
	check(preview_history_seconds(5, currentlyEmittedDelay) == 33,
	      "decreasing slider must retain still-active output history");
	check(preview_history_seconds(5, 30.1) == 34, "fractional effective delays need a complete extra second");
	check(preview_target_ns(100, currentlyEmittedDelay) == 0, "early startup cannot underflow the clock");
	check(preview_target_ns(now, 0.0) == now, "live output previews current content");
}

void telemetry_sanitization_has_bounded_integer_conversions()
{
	for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
				     -std::numeric_limits<double>::infinity(), -1.0, 0.0}) {
		check(preview_delay_ns(invalid) == 0, "invalid telemetry must not become an unsigned delay");
		check(preview_target_ns(42, invalid) == 42, "invalid delay falls back to current image");
		check(preview_history_seconds(30, invalid) == 33, "invalid delay cannot expand history");
	}
	check(preview_delay_ns(std::numeric_limits<double>::max()) == 3'600'000'000'000ULL,
	      "finite extreme telemetry is bounded before conversion");
	check(preview_history_seconds(300, std::numeric_limits<double>::max()) == 3'603,
	      "history remains bounded for extreme telemetry");
	check(preview_delay_ns(0.25) == 250'000'000ULL, "fractional delay must retain subsecond precision");
}
} // namespace

int main()
{
	try {
		duplicate_views_do_not_capture_multiple_times();
		frame_rates_keep_gpu_readback_bounded();
		clock_regression_restarts_sampling_without_underflow();
		active_timing_is_independent_of_slider_and_keeps_history();
		telemetry_sanitization_has_bounded_integer_conversions();
		std::cout << "5/5 production preview timing tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
