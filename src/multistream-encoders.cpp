#include "multistream-encoders.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace dynamic_delay {
namespace {

std::string config_string(config_t *config, const char *section, const char *key)
{
	const char *value = config_get_string(config, section, key);
	return value ? value : "";
}

bool codec_is(const char *id, const char *codec)
{
	const char *actual = id && *id ? obs_get_encoder_codec(id) : nullptr;
	return actual && std::strcmp(actual, codec) == 0;
}

bool encoder_codec_is(obs_encoder_t *encoder, const char *codec)
{
	const char *actual = encoder ? obs_encoder_get_codec(encoder) : nullptr;
	return actual && std::strcmp(actual, codec) == 0;
}

bool share_active_stream(obs_output_t *master)
{
	OBSOutputAutoRelease native = obs_frontend_get_streaming_output();
	if (!native || !obs_output_active(native))
		return false;
	obs_encoder_t *video = obs_output_get_video_encoder(native);
	obs_encoder_t *audio = obs_output_get_audio_encoder(native, 0);
	if (!video || !audio || !obs_encoder_active(video) || !obs_encoder_active(audio) ||
	    obs_encoder_video(video) != obs_get_video() || obs_encoder_audio(audio) != obs_get_audio() ||
	    !encoder_codec_is(video, "h264") || !encoder_codec_is(audio, "aac"))
		return false;
	// An Enhanced Broadcasting/simulcast layout is not one master bitstream.
	for (size_t index = 1; index < MAX_OUTPUT_VIDEO_ENCODERS; ++index) {
		if (obs_output_get_video_encoder2(native, index))
			return false;
	}
	const auto colorSpace = obs_encoder_get_preferred_color_space(video);
	if (colorSpace == VIDEO_CS_2100_PQ || colorSpace == VIDEO_CS_2100_HLG)
		return false;
	obs_output_set_video_encoder(master, video);
	obs_output_set_audio_encoder(master, audio, 0);
	return true;
}

std::vector<int> audio_bitrates(const char *id, uint32_t sampleRate)
{
	OBSProperties properties = obs_get_encoder_properties(id);
	if (!properties)
		return {};
	if (obs_property_t *sampleRateProperty = obs_properties_get(properties, "samplerate")) {
		OBSDataAutoRelease settings = obs_encoder_defaults(id);
		obs_data_set_int(settings, "samplerate", sampleRate);
		obs_property_modified(sampleRateProperty, settings);
	}
	obs_property_t *bitrate = obs_properties_get(properties, "bitrate");
	if (!bitrate)
		return {};
	std::vector<int> result;
	if (obs_property_get_type(bitrate) == OBS_PROPERTY_LIST &&
	    obs_property_list_format(bitrate) == OBS_COMBO_FORMAT_INT) {
		for (size_t index = 0; index < obs_property_list_item_count(bitrate); ++index) {
			if (!obs_property_list_item_disabled(bitrate, index))
				result.push_back(static_cast<int>(obs_property_list_item_int(bitrate, index)));
		}
	} else if (obs_property_get_type(bitrate) == OBS_PROPERTY_INT) {
		const int low = obs_property_int_min(bitrate);
		const int high = obs_property_int_max(bitrate);
		const int step = std::max(1, obs_property_int_step(bitrate));
		for (int value = low; value <= high && result.size() < 10'000; value += step)
			result.push_back(value);
	}
	std::sort(result.begin(), result.end());
	return result;
}

int supported_audio_bitrate(const char *id, int requested, uint32_t sampleRate)
{
	const auto rates = audio_bitrates(id, sampleRate);
	if (rates.empty())
		return requested;
	const auto atOrAbove = std::lower_bound(rates.begin(), rates.end(), requested);
	return atOrAbove != rates.end() ? *atOrAbove : rates.back();
}

std::string simple_audio_encoder(int bitrate, uint32_t sampleRate)
{
	// Same preferred AAC families as the OBS simple-output implementation.
	for (const char *id : {"CoreAudio_AAC", "libfdk_aac", "ffmpeg_aac"}) {
		if (!codec_is(id, "aac"))
			continue;
		const auto rates = audio_bitrates(id, sampleRate);
		if (rates.empty() || std::binary_search(rates.begin(), rates.end(), bitrate))
			return id;
	}
	return codec_is("ffmpeg_aac", "aac") ? "ffmpeg_aac" : "";
}

std::string simple_video_encoder(const std::string &selected)
{
	if (selected == "x264" || selected == "x264_lowcpu")
		return "obs_x264";
	if (selected == "qsv")
		return "obs_qsv11_v2";
	if (selected == "amd")
		return "h264_texture_amf";
	if (selected == "nvenc")
		return codec_is("obs_nvenc_h264_tex", "h264") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	if (selected == "apple_h264")
		return "com.apple.videotoolbox.videoencoder.ave.avc";
	return {};
}

void apply_service_settings(config_t *config, bool advanced, obs_data_t *video, obs_data_t *audio)
{
	obs_service_t *service = obs_frontend_get_streaming_service(); // borrowed
	if (!service || (advanced && !config_get_bool(config, "AdvOut", "ApplyServiceSettings")))
		return;
	const int64_t videoBitrate = obs_data_get_int(video, "bitrate");
	const int64_t audioBitrate = obs_data_get_int(audio, "bitrate");
	const int64_t keyframeInterval = obs_data_get_int(video, "keyint_sec");
	// This mutates only our private settings objects, never the service or
	// a running encoder. Keep the current profile's recommendation policy.
	obs_service_apply_encoder_settings(service, video, audio);
	if (config_get_bool(config, "Stream1", "IgnoreRecommended")) {
		obs_data_set_int(video, "bitrate", videoBitrate);
		obs_data_set_int(audio, "bitrate", audioBitrate);
	}
	if (advanced && keyframeInterval > 0 && keyframeInterval < obs_data_get_int(video, "keyint_sec"))
		obs_data_set_int(video, "keyint_sec", keyframeInterval);
}

} // namespace

bool configure_multistream_encoders(obs_output_t *master, std::string &error)
{
	error.clear();
	if (!master || obs_output_active(master) || obs_output_get_video_encoder(master) ||
	    obs_output_get_audio_encoder(master, 0)) {
		error = "The multistream capture output must be empty and inactive.";
		return false;
	}
	obs_video_info videoInfo{};
	obs_audio_info audioInfo{};
	if (!obs_get_video_info(&videoInfo) || !obs_get_audio_info(&audioInfo) || !obs_get_video() || !obs_get_audio()) {
		error = "OBS video and audio are not initialized.";
		return false;
	}
	if (videoInfo.colorspace == VIDEO_CS_2100_PQ || videoInfo.colorspace == VIDEO_CS_2100_HLG ||
	    (videoInfo.output_format != VIDEO_FORMAT_I420 && videoInfo.output_format != VIDEO_FORMAT_NV12)) {
		error = "Multistream 1.2 requires SDR NV12/I420 video. HDR/10-bit and other color formats are not supported.";
		return false;
	}
	if (audioInfo.speakers != SPEAKERS_MONO && audioInfo.speakers != SPEAKERS_STEREO) {
		error = "Multistream 1.2 requires mono or stereo AAC audio.";
		return false;
	}
	if (share_active_stream(master))
		return true;
	config_t *config = obs_frontend_get_profile_config(); // borrowed; never saved or changed
	if (!config) {
		error = "The current OBS output profile is unavailable.";
		return false;
	}
	const bool advanced = config_string(config, "Output", "Mode") == "Advanced";
	std::string videoId;
	std::string audioId;
	OBSDataAutoRelease videoSettings = obs_data_create();
	OBSDataAutoRelease audioSettings = obs_data_create();
	size_t audioMixer = 0;
	uint32_t scaledWidth = 0;
	uint32_t scaledHeight = 0;
	obs_scale_type scaleType = OBS_SCALE_DISABLE;
	if (advanced) {
		videoId = config_string(config, "AdvOut", "Encoder");
		audioId = config_string(config, "AdvOut", "AudioEncoder");
		if (videoId == "vt_h264_hw")
			videoId = "com.apple.videotoolbox.videoencoder.h264.gva";
		else if (videoId == "vt_h264_sw")
			videoId = "com.apple.videotoolbox.videoencoder.h264";
		char *profilePath = obs_frontend_get_current_profile_path();
		const std::string encoderPath = profilePath ? std::string(profilePath) + "/streamEncoder.json" : "";
		bfree(profilePath);
		if (encoderPath.empty()) {
			error = "The current OBS profile directory is unavailable.";
			return false;
		}
		if (os_file_exists(encoderPath.c_str())) {
			videoSettings = obs_data_create_from_json_file(encoderPath.c_str());
			if (!videoSettings) {
				error = "The current profile's streamEncoder.json cannot be read.";
				return false;
			}
		}
		const int64_t track = config_get_int(config, "AdvOut", "TrackIndex");
		if (track < 1 || track > MAX_AUDIO_MIXES) {
			error = "Select an OBS streaming audio track between 1 and 6.";
			return false;
		}
		audioMixer = static_cast<size_t>(track - 1);
		const std::string bitrateKey = "Track" + std::to_string(track) + "Bitrate";
		obs_data_set_int(audioSettings, "bitrate", config_get_int(config, "AdvOut", bitrateKey.c_str()));
		const int64_t configuredScale = config_get_int(config, "AdvOut", "RescaleFilter");
		if (configuredScale < OBS_SCALE_DISABLE || configuredScale > OBS_SCALE_AREA) {
			error = "The OBS streaming rescale filter is not supported.";
			return false;
		}
		scaleType = static_cast<obs_scale_type>(configuredScale);
		if (scaleType != OBS_SCALE_DISABLE) {
			const std::string resolution = config_string(config, "AdvOut", "RescaleRes");
			char trailing = 0;
			unsigned width = 0, height = 0;
			if (std::sscanf(resolution.c_str(), "%ux%u%c", &width, &height, &trailing) != 2 || width < 16 ||
			    height < 16 || width > 16'384 || height > 16'384 || (width & 1U) || (height & 1U)) {
				error = "The OBS streaming rescale resolution must contain valid even dimensions.";
				return false;
			}
			scaledWidth = width;
			scaledHeight = height;
		}
	} else {
		const std::string selected = config_string(config, "SimpleOutput", "StreamEncoder");
		videoId = simple_video_encoder(selected);
		const std::string audioCodec = config_string(config, "SimpleOutput", "StreamAudioEncoder");
		if (!audioCodec.empty() && audioCodec != "aac") {
			error = "Multistream 1.2 requires AAC audio; the selected OBS audio codec is not AAC.";
			return false;
		}
		const int bitrate = static_cast<int>(config_get_int(config, "SimpleOutput", "ABitrate"));
		audioId = simple_audio_encoder(bitrate, audioInfo.samples_per_sec);
		obs_data_set_int(audioSettings, "bitrate", bitrate);
		obs_data_set_string(videoSettings, "rate_control", "CBR");
		obs_data_set_int(videoSettings, "bitrate", config_get_int(config, "SimpleOutput", "VBitrate"));
		const char *presetKey = selected == "qsv" ? "QSVPreset" : selected == "amd" ? "AMDPreset" :
					selected == "nvenc" ? "NVENCPreset2" : "Preset";
		const std::string preset = config_string(config, "SimpleOutput", presetKey);
		if (!preset.empty())
			obs_data_set_string(videoSettings, videoId.starts_with("ffmpeg_") && selected == "nvenc" ? "preset2" : "preset",
					    preset.c_str());
		if (config_get_bool(config, "SimpleOutput", "UseAdvanced")) {
			const std::string options = config_string(config, "SimpleOutput", "x264Settings");
			obs_data_set_string(videoSettings, "x264opts", options.c_str());
		}
	}
	if (!codec_is(videoId.c_str(), "h264") || !codec_is(audioId.c_str(), "aac")) {
		error = "Multistream 1.2 requires an available H.264 video encoder and AAC audio encoder. HEVC, AV1 and Opus are not supported.";
		return false;
	}
	OBSDataAutoRelease defaults = obs_encoder_defaults(videoId.c_str());
	obs_data_apply(defaults, videoSettings);
	videoSettings = std::move(defaults);
	const int requestedAudioBitrate = static_cast<int>(obs_data_get_int(audioSettings, "bitrate"));
	if (requestedAudioBitrate <= 0) {
		error = "The OBS streaming audio bitrate must be positive.";
		return false;
	}
	obs_data_set_int(audioSettings, "bitrate", supported_audio_bitrate(audioId.c_str(), requestedAudioBitrate,
								 audioInfo.samples_per_sec));
	obs_data_set_string(audioSettings, "rate_control", "CBR");
	apply_service_settings(config, advanced, videoSettings, audioSettings);
	const std::string suffix = std::to_string(reinterpret_cast<uintptr_t>(master));
	OBSEncoderAutoRelease video = obs_video_encoder_create(videoId.c_str(), ("dynamic-delay-master-video-" + suffix).c_str(),
							      videoSettings, nullptr);
	OBSEncoderAutoRelease audio = obs_audio_encoder_create(audioId.c_str(), ("dynamic-delay-master-audio-" + suffix).c_str(),
							      audioSettings, audioMixer, nullptr);
	if (!video || !audio) {
		error = "OBS could not create the configured multistream video/audio encoders.";
		return false;
	}
	obs_encoder_set_video(video, obs_get_video());
	obs_encoder_set_audio(audio, obs_get_audio());
	obs_encoder_set_scaled_size(video, scaledWidth, scaledHeight);
	obs_encoder_set_gpu_scale_type(video, scaleType);
	obs_output_set_video_encoder(master, video);
	obs_output_set_audio_encoder(master, audio, 0);
	return true;
}

} // namespace dynamic_delay
