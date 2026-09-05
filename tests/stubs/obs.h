#pragma once

// Minimal libobs boundary for the real OutputSession translation unit. The
// packet queue, state machine and timestamp code remain production code.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

constexpr std::size_t MAX_OUTPUT_AUDIO_ENCODERS = 6;
constexpr std::size_t MAX_OUTPUT_VIDEO_ENCODERS = 6;
constexpr std::size_t MAX_AUDIO_MIXES = 6;
constexpr uint32_t OBS_OUTPUT_VIDEO = 1U;
constexpr uint32_t OBS_OUTPUT_AUDIO = 2U;
constexpr uint32_t OBS_OUTPUT_AV = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO;
constexpr uint32_t OBS_OUTPUT_ENCODED = 4U;
constexpr uint32_t OBS_OUTPUT_MULTI_TRACK = 8U;
constexpr uint32_t OBS_OUTPUT_MULTI_TRACK_VIDEO = 32U;
constexpr uint32_t OBS_OUTPUT_CAN_PAUSE = 64U;
constexpr int LOG_DEBUG = 100;
constexpr int LOG_INFO = 200;
constexpr int LOG_WARNING = 300;
constexpr int LOG_ERROR = 400;

enum obs_encoder_type { OBS_ENCODER_AUDIO, OBS_ENCODER_VIDEO };
enum gs_color_space { GS_CS_SRGB };
struct obs_source;
struct obs_view;
struct gs_effect;
struct audio_output;
struct video_output;
struct audio_output_data;
struct audio_data;
struct obs_source_audio_mix;
using obs_source_t = obs_source;
using obs_view_t = obs_view;
using gs_effect_t = gs_effect;
using audio_t = audio_output;
using video_t = video_output;
using obs_source_enum_proc_t = void (*)(obs_source_t *, obs_source_t *, void *);

struct obs_data {
	std::string rateControl = "CBR";
	int64_t bitrate = 6'000;
	bool lossless = false;
};
using obs_data_t = obs_data;
struct obs_encoder {
	obs_data_t settings;
	std::string id = "fake-h264";
	obs_encoder_type type = OBS_ENCODER_VIDEO;
	bool paused = false;
	uint32_t sampleRate = 48'000;
	std::size_t frameSize = 2'400;
};
using obs_encoder_t = obs_encoder;

struct encoder_packet {
	uint8_t *data = nullptr;
	std::size_t size = 0;
	int64_t pts = 0;
	int64_t dts = 0;
	int32_t timebase_num = 1;
	int32_t timebase_den = 1'000'000;
	obs_encoder_type type = OBS_ENCODER_VIDEO;
	bool keyframe = false;
	int priority = 0;
	int drop_priority = 0;
	int64_t dts_usec = 0;
	int64_t sys_dts_usec = 0;
	std::size_t track_idx = 0;
	obs_encoder_t *encoder = nullptr;
};
struct encoder_packet_time {
	int64_t pts = 0;
	uint64_t cts = 0;
	uint64_t fer = 0;
	uint64_t ferc = 0;
	uint64_t pir = 0;
};
struct calldata {};
using calldata_t = calldata;
struct obs_output;
using obs_output_t = obs_output;
using packet_callback_t = void (*)(obs_output_t *, encoder_packet *, encoder_packet_time *, void *);
using signal_callback_t = void (*)(void *, calldata_t *);
struct signal_handler {
	signal_callback_t reconnect = nullptr;
	void *reconnectParam = nullptr;
	signal_callback_t activate = nullptr;
	void *activateParam = nullptr;
	signal_callback_t pause = nullptr;
	void *pauseParam = nullptr;
	signal_callback_t unpause = nullptr;
	void *unpauseParam = nullptr;
};
using signal_handler_t = signal_handler;
struct obs_output {
	uint32_t flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_AV;
	uint32_t nativeDelay = 0;
	std::string id = "fake-output";
	std::array<obs_encoder_t *, MAX_OUTPUT_VIDEO_ENCODERS> video{};
	std::array<obs_encoder_t *, MAX_OUTPUT_AUDIO_ENCODERS> audio{};
	packet_callback_t callback = nullptr;
	void *callbackParam = nullptr;
	signal_handler_t signals;
	int refs = 1;
	bool paused = false;
	bool reconnecting = false;
};

obs_output_t *obs_output_get_ref(obs_output_t *output);
void obs_output_release(obs_output_t *output);
uint32_t obs_output_get_flags(const obs_output_t *output);
const char *obs_output_get_id(const obs_output_t *output);
obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *output);
obs_encoder_t *obs_output_get_video_encoder2(const obs_output_t *output, std::size_t index);
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *output, std::size_t index);
uint32_t obs_output_get_active_delay(const obs_output_t *output);
bool obs_output_paused(const obs_output_t *output);
bool obs_encoder_paused(const obs_encoder_t *encoder);
bool obs_output_reconnecting(const obs_output_t *output);
uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *encoder);
std::size_t obs_encoder_get_frame_size(const obs_encoder_t *encoder);
void obs_output_add_packet_callback(obs_output_t *output, packet_callback_t callback, void *param);
void obs_output_remove_packet_callback(obs_output_t *output, packet_callback_t callback, void *param);
signal_handler_t *obs_output_get_signal_handler(obs_output_t *output);
void signal_handler_connect(signal_handler_t *handler, const char *name, signal_callback_t callback, void *param);
void signal_handler_disconnect(signal_handler_t *handler, const char *name, signal_callback_t callback, void *param);
const char *obs_encoder_get_id(const obs_encoder_t *encoder);
obs_data_t *obs_encoder_get_settings(obs_encoder_t *encoder);
const char *obs_data_get_string(const obs_data_t *settings, const char *name);
int64_t obs_data_get_int(const obs_data_t *settings, const char *name);
bool obs_data_get_bool(const obs_data_t *settings, const char *name);
void obs_data_release(obs_data_t *settings);
void obs_encoder_packet_ref(encoder_packet *destination, encoder_packet *source);
void obs_encoder_packet_release(encoder_packet *packet);
