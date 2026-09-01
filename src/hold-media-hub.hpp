#pragma once

#include "hold-audio-tap.hpp"

#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dynamic_delay {

/**
 * Shared private media graph for one delay activation.  Streaming and
 * recording sessions clone their encoders against the same video_t/audio_t,
 * so the hold scene is present in libobs's render tree exactly once.
 */
class HoldMediaHub final {
public:
	static std::shared_ptr<HoldMediaHub> create(obs_source_t *holdScene, HoldAudioConfig config,
						    std::string &error);
	static void register_source_type();

	~HoldMediaHub();

	HoldMediaHub(const HoldMediaHub &) = delete;
	HoldMediaHub &operator=(const HoldMediaHub &) = delete;

	bool start(std::string &error);
	void stop();

	[[nodiscard]] video_t *video() const noexcept { return video_; }
	[[nodiscard]] audio_t *audio() const noexcept { return audio_; }
	[[nodiscard]] std::size_t encoder_mixer_index(const obs_encoder_t *original) const noexcept;
	[[nodiscard]] obs_source_t *hold_scene() const noexcept { return holdScene_; }
	[[nodiscard]] HoldAudioMode audio_mode() const noexcept
	{
		return audioSilenced_ ? HoldAudioMode::Silence : config_.mode;
	}
	[[nodiscard]] HoldAudioMode configured_audio_mode() const noexcept { return config_.mode; }
	[[nodiscard]] obs_source_t *dedicated_audio_source() const noexcept { return dedicatedAudioSource_; }
	[[nodiscard]] uint32_t reserved_mixer_index() const noexcept { return config_.reservedMixerIndex; }
	[[nodiscard]] bool active() const noexcept { return started_; }
	[[nodiscard]] bool matches(obs_source_t *holdScene, const HoldAudioConfig &config) const noexcept;
	void set_activation_signature(HoldAudioConfig config) noexcept;
	bool force_audio_silence();

private:
	HoldMediaHub(obs_source_t *holdScene, HoldAudioConfig config);

	static bool audio_input(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
				audio_output_data *mixes);
	bool create_view(std::string &error);
	bool create_audio(std::string &error);

	obs_source_t *holdScene_ = nullptr;
	obs_source_t *dedicatedAudioSource_ = nullptr;
	HoldAudioConfig config_;
	HoldAudioConfig activationConfig_;
	std::unique_ptr<HoldAudioTap> audioTap_;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	std::string audioName_;
	std::size_t audioChannels_ = 0;
	uint32_t audioSampleRate_ = 0;
	bool started_ = false;
	bool wrapperActive_ = false;
	bool sceneActive_ = false;
	bool ownsAudio_ = false;
	bool audioSilenced_ = false;
};

} // namespace dynamic_delay
