#include "hold-pipeline.hpp"

#include "hold-media-hub.hpp"
#include "output-session.hpp"
#include "plugin-support.h"

#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

namespace dynamic_delay {
namespace {

constexpr const char *CaptureOutputId = "obs_dynamic_delay_capture_output";

struct CaptureSink {
	OutputSession *owner = nullptr;
	obs_output_t *output = nullptr;
};

const char *capture_name(void *)
{
	return "Dynamic Delay internal packet collector";
}

void *capture_create(obs_data_t *settings, obs_output_t *output)
{
	auto *sink = new CaptureSink;
	sink->owner =
		reinterpret_cast<OutputSession *>(static_cast<uintptr_t>(obs_data_get_int(settings, "owner_pointer")));
	sink->output = output;
	return sink;
}

void capture_destroy(void *data)
{
	delete static_cast<CaptureSink *>(data);
}

bool capture_start(void *data)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (!sink || !sink->output)
		return false;
	if (!obs_output_can_begin_data_capture(sink->output, 0))
		return false;
	if (!obs_output_initialize_encoders(sink->output, 0))
		return false;
	return obs_output_begin_data_capture(sink->output, 0);
}

void capture_stop(void *data, uint64_t)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (sink && sink->output)
		obs_output_end_data_capture(sink->output);
}

void capture_packet(void *data, encoder_packet *packet)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (sink && sink->owner)
		sink->owner->receive_hold_packet(packet);
}

} // namespace

HoldPipeline::HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, obs_source_t *scene)
	: owner_(owner),
	  primaryOutput_(obs_output_get_ref(primaryOutput)),
	  scene_(obs_source_get_ref(scene))
{
}

HoldPipeline::HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, std::shared_ptr<HoldMediaHub> mediaHub)
	: owner_(owner),
	  primaryOutput_(obs_output_get_ref(primaryOutput)),
	  mediaHub_(std::move(mediaHub))
{
}

HoldPipeline::~HoldPipeline()
{
	stop();
	obs_output_release(primaryOutput_);
	obs_source_release(scene_);
}

void HoldPipeline::register_output_type()
{
	static std::once_flag once;
	std::call_once(once, [] {
		HoldMediaHub::register_source_type();
		obs_output_info info{};
		info.id = CaptureOutputId;
		info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK_AUDIO;
		info.get_name = capture_name;
		info.create = capture_create;
		info.destroy = capture_destroy;
		info.start = capture_start;
		info.stop = capture_stop;
		info.encoded_packet = capture_packet;
		info.encoded_video_codecs = "h264;hevc;av1;vp8;vp9";
		info.encoded_audio_codecs = "aac;opus;pcm_s16le;pcm_s24le;pcm_s32le;flac";
		obs_register_output(&info);
	});
}

obs_encoder_t *HoldPipeline::clone_video_encoder(obs_encoder_t *original, std::string &error)
{
	if (!original) {
		error = "The primary output has no video encoder.";
		return nullptr;
	}
	obs_data_t *settings = obs_encoder_get_settings(original);
	std::ostringstream name;
	name << "dynamic-delay-hold-video-" << this;
	obs_encoder_t *clone =
		obs_video_encoder_create(obs_encoder_get_id(original), name.str().c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!clone) {
		error = std::string("Could not clone video encoder: ") + obs_encoder_get_id(original);
		return nullptr;
	}

	obs_encoder_set_video(clone, mediaHub_->video());
	if (obs_encoder_scaling_enabled(original))
		obs_encoder_set_scaled_size(clone, obs_encoder_get_width(original), obs_encoder_get_height(original));
	obs_encoder_set_gpu_scale_type(clone, obs_encoder_get_scale_type(original));
	obs_encoder_set_frame_rate_divisor(clone, obs_encoder_get_frame_rate_divisor(original));
	obs_encoder_set_preferred_video_format(clone, obs_encoder_get_preferred_video_format(original));
	obs_encoder_set_preferred_color_space(clone, obs_encoder_get_preferred_color_space(original));
	obs_encoder_set_preferred_range(clone, obs_encoder_get_preferred_range(original));
	return clone;
}

obs_encoder_t *HoldPipeline::clone_audio_encoder(obs_encoder_t *original, const std::size_t index, std::string &error)
{
	obs_data_t *settings = obs_encoder_get_settings(original);
	std::ostringstream name;
	name << "dynamic-delay-hold-audio-" << index << '-' << this;
	obs_encoder_t *clone = obs_audio_encoder_create(obs_encoder_get_id(original), name.str().c_str(), settings,
							mediaHub_->encoder_mixer_index(original), nullptr);
	obs_data_release(settings);
	if (!clone) {
		error = std::string("Could not clone audio encoder: ") + obs_encoder_get_id(original);
		return nullptr;
	}
	obs_encoder_set_audio(clone, mediaHub_->audio());
	return clone;
}

bool HoldPipeline::clone_encoders(std::string &error)
{
	videoEncoder_ = clone_video_encoder(obs_output_get_video_encoder(primaryOutput_), error);
	if (!videoEncoder_)
		return false;
	for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
		obs_encoder_t *original = obs_output_get_audio_encoder(primaryOutput_, index);
		if (!original)
			continue;
		audioEncoders_[index] = clone_audio_encoder(original, index, error);
		if (!audioEncoders_[index])
			return false;
	}
	return true;
}

bool HoldPipeline::create_capture_output(std::string &error)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "owner_pointer", static_cast<long long>(reinterpret_cast<uintptr_t>(&owner_)));
	std::ostringstream name;
	name << "dynamic-delay-collector-" << this;
	captureOutput_ = obs_output_create(CaptureOutputId, name.str().c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!captureOutput_) {
		error = "Could not create the internal packet collector.";
		return false;
	}
	obs_output_set_video_encoder(captureOutput_, videoEncoder_);
	for (std::size_t index = 0; index < audioEncoders_.size(); ++index) {
		if (audioEncoders_[index])
			obs_output_set_audio_encoder(captureOutput_, audioEncoders_[index], index);
	}
	return true;
}

bool HoldPipeline::start(std::string &error)
{
	if (started_)
		return true;
	if (!mediaHub_) {
		mediaHub_ = HoldMediaHub::create(scene_, {}, error);
		if (!mediaHub_) {
			stop();
			return false;
		}
	}
	if (!mediaHub_->active()) {
		error = "The shared hold media hub is not active.";
		stop();
		return false;
	}
	if (!clone_encoders(error) || !create_capture_output(error)) {
		stop();
		return false;
	}
	if (!obs_output_start(captureOutput_)) {
		const char *lastError = obs_output_get_last_error(captureOutput_);
		error = lastError && *lastError ? lastError : "The hold-scene encoder could not start.";
		stop();
		return false;
	}
	started_ = true;
	return true;
}

void HoldPipeline::stop()
{
	if (captureOutput_) {
		if (obs_output_active(captureOutput_))
			obs_output_stop(captureOutput_);
		obs_output_release(captureOutput_);
		captureOutput_ = nullptr;
	}
	for (obs_encoder_t *&encoder : audioEncoders_) {
		obs_encoder_release(encoder);
		encoder = nullptr;
	}
	obs_encoder_release(videoEncoder_);
	videoEncoder_ = nullptr;
	mediaHub_.reset();
	started_ = false;
}

bool HoldPipeline::same_extradata(obs_encoder_t *first, obs_encoder_t *second)
{
	uint8_t *firstData = nullptr;
	uint8_t *secondData = nullptr;
	size_t firstSize = 0;
	size_t secondSize = 0;
	const bool firstOk = obs_encoder_get_extra_data(first, &firstData, &firstSize);
	const bool secondOk = obs_encoder_get_extra_data(second, &secondData, &secondSize);
	if (firstOk != secondOk || firstSize != secondSize)
		return false;
	return !firstOk || firstSize == 0 || std::memcmp(firstData, secondData, firstSize) == 0;
}

bool HoldPipeline::compatible_with_primary(std::string &error) const
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primaryOutput_);
	if (!primaryVideo || !videoEncoder_ ||
	    std::strcmp(obs_encoder_get_codec(primaryVideo), obs_encoder_get_codec(videoEncoder_)) != 0 ||
	    !same_extradata(primaryVideo, videoEncoder_)) {
		error = "The hold video encoder produced incompatible codec headers.";
		return false;
	}
	for (std::size_t index = 0; index < audioEncoders_.size(); ++index) {
		obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primaryOutput_, index);
		if (!!primaryAudio != !!audioEncoders_[index]) {
			error = "The hold audio track layout does not match the output.";
			return false;
		}
		if (primaryAudio && (std::strcmp(obs_encoder_get_codec(primaryAudio),
						 obs_encoder_get_codec(audioEncoders_[index])) != 0 ||
				     !same_extradata(primaryAudio, audioEncoders_[index]))) {
			error = "A hold-scene audio encoder produced incompatible codec headers.";
			return false;
		}
	}
	return true;
}

void HoldPipeline::receive(encoder_packet *packet)
{
	owner_.receive_hold_packet(packet);
}

} // namespace dynamic_delay
