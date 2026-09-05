#pragma once

// Reuse the basic libobs boundary types; additional functions below are the
// encoder/collector boundary exercised by the real hold-pipeline.cpp.
#include "../stubs/obs.h"

constexpr uint32_t OBS_OUTPUT_MULTI_TRACK_AUDIO = OBS_OUTPUT_MULTI_TRACK;
enum obs_scale_type { OBS_SCALE_DISABLE };
enum video_format { VIDEO_FORMAT_NONE };
enum video_colorspace { VIDEO_CS_DEFAULT };
enum video_range_type { VIDEO_RANGE_DEFAULT };

struct obs_output_info {
	const char *id = nullptr;
	uint32_t flags = 0;
	const char *(*get_name)(void *) = nullptr;
	void *(*create)(obs_data_t *, obs_output_t *) = nullptr;
	void (*destroy)(void *) = nullptr;
	bool (*start)(void *) = nullptr;
	void (*stop)(void *, uint64_t) = nullptr;
	void (*encoded_packet)(void *, encoder_packet *) = nullptr;
	const char *encoded_video_codecs = nullptr;
	const char *encoded_audio_codecs = nullptr;
};

obs_source_t *obs_source_get_ref(obs_source_t *source);
void obs_source_release(obs_source_t *source);
obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *encoder);
void obs_encoder_release(obs_encoder_t *encoder);
obs_data_t *obs_data_create();
void obs_data_set_int(obs_data_t *settings, const char *name, int64_t value);
void obs_register_output(const obs_output_info *info);
obs_output_t *obs_output_create(const char *id, const char *name, obs_data_t *settings, obs_data_t *hotkeys);
bool obs_output_can_begin_data_capture(obs_output_t *output, uint32_t flags);
bool obs_output_initialize_encoders(obs_output_t *output, uint32_t flags);
bool obs_output_begin_data_capture(obs_output_t *output, uint32_t flags);
void obs_output_end_data_capture(obs_output_t *output);
bool obs_output_start(obs_output_t *output);
void obs_output_stop(obs_output_t *output);
bool obs_output_active(obs_output_t *output);
bool obs_output_pause(obs_output_t *output, bool paused);
const char *obs_output_get_last_error(obs_output_t *output);
void obs_output_set_last_error(obs_output_t *output, const char *error);
void obs_output_set_video_encoder(obs_output_t *output, obs_encoder_t *encoder);
void obs_output_set_audio_encoder(obs_output_t *output, obs_encoder_t *encoder, std::size_t index);
obs_encoder_t *obs_video_encoder_create(const char *id, const char *name, obs_data_t *settings, obs_data_t *hotkeys);
obs_encoder_t *obs_audio_encoder_create(const char *id, const char *name, obs_data_t *settings, std::size_t mixer,
					obs_data_t *hotkeys);
const char *obs_encoder_get_codec(obs_encoder_t *encoder);
bool obs_encoder_get_extra_data(obs_encoder_t *encoder, uint8_t **data, std::size_t *size);
bool obs_encoder_scaling_enabled(obs_encoder_t *encoder);
uint32_t obs_encoder_get_width(obs_encoder_t *encoder);
uint32_t obs_encoder_get_height(obs_encoder_t *encoder);
obs_scale_type obs_encoder_get_scale_type(obs_encoder_t *encoder);
uint32_t obs_encoder_get_frame_rate_divisor(obs_encoder_t *encoder);
video_format obs_encoder_get_preferred_video_format(obs_encoder_t *encoder);
video_colorspace obs_encoder_get_preferred_color_space(obs_encoder_t *encoder);
video_range_type obs_encoder_get_preferred_range(obs_encoder_t *encoder);
std::size_t obs_encoder_get_mixer_index(const obs_encoder_t *encoder);
void obs_encoder_set_video(obs_encoder_t *encoder, video_t *video);
void obs_encoder_set_audio(obs_encoder_t *encoder, audio_t *audio);
void obs_encoder_set_scaled_size(obs_encoder_t *encoder, uint32_t width, uint32_t height);
void obs_encoder_set_gpu_scale_type(obs_encoder_t *encoder, obs_scale_type scale);
void obs_encoder_set_frame_rate_divisor(obs_encoder_t *encoder, uint32_t divisor);
void obs_encoder_set_preferred_video_format(obs_encoder_t *encoder, video_format format);
void obs_encoder_set_preferred_color_space(obs_encoder_t *encoder, video_colorspace colorspace);
void obs_encoder_set_preferred_range(obs_encoder_t *encoder, video_range_type range);
