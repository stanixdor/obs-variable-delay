#include "hold-media-hub.hpp"

#include <algorithm>
#include <sstream>

namespace dynamic_delay {

HoldMediaHub::HoldMediaHub(obs_source_t *holdScene, HoldAudioConfig config)
	: holdScene_(obs_source_get_ref(holdScene)),
	  dedicatedAudioSource_(obs_source_get_ref(config.dedicatedSource)),
	  config_(config),
	  activationConfig_(config)
{
	config_.dedicatedSource = dedicatedAudioSource_;
	activationConfig_.dedicatedSource = dedicatedAudioSource_;
	audioSilenced_ = config_.mode == HoldAudioMode::Silence;
}

HoldMediaHub::~HoldMediaHub()
{
	stop();
	obs_source_release(dedicatedAudioSource_);
	obs_source_release(holdScene_);
}

std::shared_ptr<HoldMediaHub> HoldMediaHub::create(obs_source_t *holdScene, HoldAudioConfig config, std::string &error)
{
	if (!holdScene) {
		error = "Select a valid hold scene first.";
		return {};
	}
	if (config.mode == HoldAudioMode::DedicatedSource && !config.dedicatedSource) {
		error = "Select a valid dedicated hold audio source.";
		return {};
	}
	auto hub = std::shared_ptr<HoldMediaHub>(new HoldMediaHub(holdScene, config));
	if (!hub->start(error))
		return {};
	return hub;
}

void HoldMediaHub::register_source_type()
{
	HoldAudioTap::register_source_type();
}

std::size_t HoldMediaHub::encoder_mixer_index(const obs_encoder_t *original) const noexcept
{
	return config_.mode == HoldAudioMode::ReservedTrack ? config_.reservedMixerIndex
							    : obs_encoder_get_mixer_index(original);
}

bool HoldMediaHub::matches(obs_source_t *holdScene, const HoldAudioConfig &config) const noexcept
{
	if (holdScene_ != holdScene || activationConfig_.mode != config.mode)
		return false;
	if ((config.mode == HoldAudioMode::SceneMix || config.mode == HoldAudioMode::DedicatedSource) &&
	    activationConfig_.dedicatedSourceUuid != config.dedicatedSourceUuid)
		return false;
	return config.mode != HoldAudioMode::ReservedTrack ||
	       activationConfig_.reservedMixerIndex == config.reservedMixerIndex;
}

void HoldMediaHub::set_activation_signature(HoldAudioConfig config) noexcept
{
	config.dedicatedSource = dedicatedAudioSource_;
	activationConfig_ = config;
}

bool HoldMediaHub::create_view(std::string &error)
{
	obs_video_info videoInfo{};
	if (!obs_get_video_info(&videoInfo)) {
		error = "OBS video is not initialized.";
		return false;
	}

	audioTap_ = std::make_unique<HoldAudioTap>(holdScene_, config_);
	if (!audioTap_->create(error))
		return false;
	audio_t *mainAudio = obs_get_audio();
	const std::size_t clockMixer = config_.mode == HoldAudioMode::ReservedTrack ? config_.reservedMixerIndex : 0;
	if (config_.mode != HoldAudioMode::Silence && !audioTap_->connect_main_clock(mainAudio, clockMixer, error))
		return false;

	view_ = obs_view_create();
	if (!view_) {
		error = "Could not create the private hold-scene view.";
		return false;
	}
	video_ = obs_view_add2(view_, &videoInfo);
	if (!video_) {
		error = "Could not attach video output to the private hold-scene view.";
		return false;
	}

	obs_view_set_source(view_, 0, audioTap_->source());
	// AUX_VIEW marks a source as showing, but the audio renderer only walks
	// active view roots.  The explicit active ref also propagates to exactly
	// the one audio child enumerated by the wrapper.
	obs_source_inc_active(audioTap_->source());
	wrapperActive_ = true;

	// Dedicated/Silence modes intentionally do not enumerate the
	// scene into the audio tree.  It still needs activation for game/window
	// capture and other video children to keep producing frames.
	if (config_.mode == HoldAudioMode::DedicatedSource || config_.mode == HoldAudioMode::Silence) {
		obs_source_inc_active(holdScene_);
		sceneActive_ = true;
	}
	return true;
}

bool HoldMediaHub::force_audio_silence()
{
	if (audioSilenced_ || !audioTap_ || !audioTap_->source())
		return false;
	obs_source_t *audioChild = audioTap_->active_audio_child();
	if (!audioTap_->force_silence())
		return false;
	if (!sceneActive_) {
		// Preserve the hold video's active/showing refs before removing the
		// same scene from SceneMix/Reserved audio enumeration.
		obs_source_inc_active(holdScene_);
		sceneActive_ = true;
	}
	// The wrapper remains attached to its video view.  Remove just the audio
	// child refs which were propagated through the two wrapper activations;
	// future enum calls see the forced-silence flag and return an empty tree.
	if (audioChild)
		obs_source_remove_active_child(audioTap_->source(), audioChild);
	audioSilenced_ = true;
	return true;
}

bool HoldMediaHub::audio_input(void *param, const uint64_t startTs, uint64_t, uint64_t *newTs,
			       const uint32_t activeMixers, audio_output_data *mixes)
{
	auto *self = static_cast<HoldMediaHub *>(param);
	if (!self || !self->audioTap_)
		return false;
	return self->audioTap_->pull(startTs, newTs, activeMixers, mixes, self->audioChannels_, self->audioSampleRate_);
}

bool HoldMediaHub::create_audio(std::string &error)
{
	audio_t *mainAudio = obs_get_audio();
	if (!mainAudio) {
		error = "OBS audio is not initialized.";
		return false;
	}
	const audio_output_info *mainInfo = audio_output_get_info(mainAudio);
	if (!mainInfo) {
		error = "OBS audio is not initialized.";
		return false;
	}

	std::ostringstream name;
	name << "dynamic-delay-hold-audio-hub-" << this;
	audioName_ = name.str();
	audio_output_info info = *mainInfo;
	audioChannels_ = get_audio_channels(info.speakers);
	// audio_output_open starts its worker before publishing the returned
	// audio_t pointer.  The callback must therefore use values initialized
	// before the open call rather than dereferencing audio_ during startup.
	audioSampleRate_ = info.samples_per_sec;
	info.name = audioName_.c_str();
	info.input_callback = &HoldMediaHub::audio_input;
	info.input_param = this;
	if (audio_output_open(&audio_, &info) != AUDIO_OUTPUT_SUCCESS) {
		error = "Could not create the private hold-scene audio clock.";
		return false;
	}
	ownsAudio_ = true;
	return true;
}

bool HoldMediaHub::start(std::string &error)
{
	if (started_)
		return true;
	if (config_.mode == HoldAudioMode::ReservedTrack && config_.reservedMixerIndex >= MAX_AUDIO_MIXES) {
		error = "The reserved OBS audio track is outside the valid 1-6 range.";
		return false;
	}
	const bool ready = create_view(error) && create_audio(error);
	if (!ready) {
		stop();
		return false;
	}
	started_ = true;
	return true;
}

void HoldMediaHub::stop()
{
	if (view_)
		obs_view_set_source(view_, 0, nullptr);
	if (wrapperActive_ && audioTap_ && audioTap_->source()) {
		obs_source_dec_active(audioTap_->source());
		wrapperActive_ = false;
	}

	// Detaching the wrapper stops new producer callbacks.  Joining the
	// private audio thread then guarantees no FIFO consumer remains before
	// the tap/source state is released.
	if (audio_ && ownsAudio_) {
		audio_output_close(audio_);
	}
	audio_ = nullptr;
	ownsAudio_ = false;
	audioChannels_ = 0;
	audioSampleRate_ = 0;
	if (audioTap_) {
		audioTap_->stop();
		audioTap_.reset();
	}
	if (view_) {
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	if (sceneActive_) {
		obs_source_dec_active(holdScene_);
		sceneActive_ = false;
	}
	started_ = false;
	audioSilenced_ = config_.mode == HoldAudioMode::Silence;
}

} // namespace dynamic_delay
