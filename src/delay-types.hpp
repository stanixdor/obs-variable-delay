#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

struct DelaySettings {
	uint32_t delaySeconds = 30;
	TransitionStyle transition = TransitionStyle::Cut;
	std::string holdSceneUuid;
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
	std::string detail;
};

const char *state_name(DelayState state) noexcept;

} // namespace dynamic_delay
