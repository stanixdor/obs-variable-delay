#pragma once

#include "delay-types.hpp"

#include <obs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dynamic_delay {

struct AudioPreflightResult {
	HoldAudioMode requestedMode = HoldAudioMode::Silence;
	HoldAudioMode effectiveMode = HoldAudioMode::Silence;
	bool degraded = false;
	// Always normalized to a zero-based OBS mixer index (0..5).
	uint32_t reservedMixerIndex = 0;
	std::string message;
	// Source/output display names which caused the requested mode to be rejected.
	std::vector<std::string> conflictNames;
};

/**
 * Resolves the requested hold-audio mode without changing any OBS source,
 * mixer, encoder, or output state.
 *
 * The caller owns the supplied references.  The function takes temporary
 * strong references while it walks source trees and active outputs.
 */
[[nodiscard]] AudioPreflightResult preflight_hold_audio(const DelaySettings &settings, obs_source_t *holdScene,
							obs_source_t *dedicatedSource,
							const std::vector<obs_output_t *> &primaryOutputs);

} // namespace dynamic_delay
