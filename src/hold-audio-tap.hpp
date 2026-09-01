#pragma once

#include "core/audio_spsc_fifo.hpp"
#include "delay-types.hpp"

#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dynamic_delay {

struct HoldAudioConfig {
	HoldAudioMode mode = HoldAudioMode::SceneMix;
	obs_source_t *dedicatedSource = nullptr;
	// Stable requested identity.  The live source pointer may become null when
	// OBS removes a source while an activation still retains it.
	std::string dedicatedSourceUuid;
	// Zero-based OBS mixer index.  Consumed by the ReservedTrack adapter.
	uint32_t reservedMixerIndex = 5;
};

/**
 * A private composite source which keeps the hold scene in libobs's normal
 * audio render tree.  Its audio_render callback runs after the child source,
 * copies the already-mixed PCM into a bounded SPSC FIFO, and returns
 * immediately.  The private hold encoder consumes that FIFO on its own audio
 * clock.
 */
class HoldAudioTap {
public:
	using Fifo = core::PcmSpscFifo<16>;

	HoldAudioTap(obs_source_t *holdScene, HoldAudioConfig config = {});
	~HoldAudioTap();

	HoldAudioTap(const HoldAudioTap &) = delete;
	HoldAudioTap &operator=(const HoldAudioTap &) = delete;

	static void register_source_type();
	bool create(std::string &error);
	bool connect_main_clock(audio_t *audio, std::size_t mixerIndex, std::string &error);
	bool force_silence() noexcept;
	[[nodiscard]] bool silence_forced() const noexcept;
	void stop();

	[[nodiscard]] obs_source_t *source() const noexcept { return wrapperSource_; }
	[[nodiscard]] obs_source_t *active_audio_child() const noexcept;
	[[nodiscard]] HoldAudioMode mode() const noexcept;

	bool pull(uint64_t callbackStartNs, uint64_t *newTimestampNs, uint32_t activeMixers, audio_output_data *mixes,
		  std::size_t channels, uint32_t sampleRate) noexcept;

private:
	struct SharedState;
	struct SourceContext;

	static const char *source_name(void *unused);
	static void *source_create(obs_data_t *settings, obs_source_t *source);
	static void source_destroy(void *data);
	static uint32_t source_width(void *data);
	static uint32_t source_height(void *data);
	static void source_video_render(void *data, gs_effect_t *effect);
	static enum gs_color_space source_video_color_space(void *data, std::size_t count,
							    const enum gs_color_space *preferredSpaces);
	static void source_enum_active(void *data, obs_source_enum_proc_t callback, void *param);
	static bool source_audio_render(void *data, uint64_t *timestampNs, obs_source_audio_mix *output,
					uint32_t mixers, std::size_t channels, std::size_t sampleRate);
	static void main_clock_output(void *param, std::size_t mixerIndex, audio_data *data);

	static void push(SharedState &state, const obs_source_audio_mix &mix, uint64_t timestampNs, uint32_t mixers,
			 std::size_t channels, uint32_t sampleRate) noexcept;

	std::shared_ptr<SharedState> state_;
	obs_source_t *wrapperSource_ = nullptr;
	audio_t *mainAudioClock_ = nullptr;
	std::size_t mainClockMixer_ = 0;
};

} // namespace dynamic_delay
