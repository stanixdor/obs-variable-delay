#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dynamic_delay {

enum class DelayState {
	Bypass,
	Preparing,
	Filling,
	Delayed,
	ReturningLive,
	Error,
};

enum class TransitionStyle {
	Cut,
	FadeThroughBlack,
};

enum class HoldAudioMode {
	SceneMix,
	DedicatedSource,
	ReservedTrack,
	Silence,
};

struct DelaySettings {
	uint32_t delaySeconds = 30;
	TransitionStyle transition = TransitionStyle::Cut;
	std::string holdSceneUuid;
	HoldAudioMode holdAudioMode = HoldAudioMode::SceneMix;
	std::string holdAudioSourceUuid;
	// Stored as the user-facing OBS track number (1..MAX_AUDIO_MIXES).
	uint32_t reservedAudioTrack = 6;
	bool previewExpanded = false;
};

struct DelaySnapshot {
	DelayState state = DelayState::Bypass;
	uint32_t configuredSeconds = 30;
	double progress = 0.0;
	std::size_t bufferedBytes = 0;
	double measuredMegabitsPerSecond = 0.0;
	std::size_t estimatedBytes = 0;
	bool estimateAvailable = false;
	std::size_t activeOutputs = 0;
	bool emittingHold = false;
	bool emittingDelayed = false;
	// Actual age of the emitted video, independent of a slider change or
	// keyframe-aligned recovery after an encoder drops frames.
	double effectiveSeconds = 0.0;
	uint64_t emittedVideoTimestampNs = 0;
	uint64_t bufferStartTimestampNs = 0;
	bool paused = false;
	struct OutputTiming {
		std::string label;
		DelayState state = DelayState::Bypass;
		double effectiveSeconds = 0.0;
		uint64_t emittedVideoTimestampNs = 0;
		uint64_t bufferStartTimestampNs = 0;
		bool emittingHold = false;
		bool emittingDelayed = false;
		bool paused = false;
	};
	std::vector<OutputTiming> outputTimings;
	std::string detail;
};

const char *state_name(DelayState state) noexcept;

} // namespace dynamic_delay
