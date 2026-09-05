// Opt-in GPU integration, not a unit-test fake. This executable starts its
// own libobs instance and synthetic source; no OBS profiles are loaded.
#include "preview-capture.hpp"

#include <obs.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dynamic_delay::PreviewCapture;
std::atomic_bool alternatePattern{false};

void require(const bool condition, const std::string &message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void pump_events()
{
	QGuiApplication::processEvents();
	QThread::msleep(5);
}

void pump_for(const std::chrono::milliseconds duration)
{
	const auto deadline = std::chrono::steady_clock::now() + duration;
	while (std::chrono::steady_clock::now() < deadline)
		pump_events();
}

const char *pattern_name(void *)
{
	return "Dynamic Delay GPU integration pattern";
}

void *pattern_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

void pattern_destroy(void *) {}

uint32_t pattern_width(void *)
{
	obs_video_info info{};
	return obs_get_video_info(&info) ? info.base_width : 0;
}

uint32_t pattern_height(void *)
{
	obs_video_info info{};
	return obs_get_video_info(&info) ? info.base_height : 0;
}

void render_band(const uint32_t width, const uint32_t height, const float red, const float green, const float blue)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 color;
	vec4_set(&color, red, green, blue, 1.0F);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, width, height);
}

void pattern_render(void *data, gs_effect_t *)
{
	const uint32_t width = pattern_width(data);
	const uint32_t height = pattern_height(data);
	const bool alternate = alternatePattern.load(std::memory_order_relaxed);
	gs_matrix_push();
	render_band(width, height / 2, alternate ? 0.0F : 1.0F, alternate ? 1.0F : 0.0F, 0.0F);
	gs_matrix_translate3f(0.0F, static_cast<float>(height / 2), 0.0F);
	render_band(width, height - height / 2, alternate ? 1.0F : 0.0F, alternate ? 1.0F : 0.0F,
		    alternate ? 0.0F : 1.0F);
	gs_matrix_pop();
}

void register_pattern()
{
	obs_source_info info{};
	info.id = "dynamic_delay_gpu_integration_pattern";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = &pattern_name;
	info.create = &pattern_create;
	info.destroy = &pattern_destroy;
	info.get_width = &pattern_width;
	info.get_height = &pattern_height;
	info.video_render = &pattern_render;
	obs_register_source(&info);
}

class ObsInstance {
public:
	explicit ObsInstance(const char *configPath)
	{
		require(obs_startup("en-US", configPath, nullptr), "obs_startup failed");
	}
	~ObsInstance() { obs_shutdown(); }
};

class PatternSource {
public:
	PatternSource()
	{
		source_ = obs_source_create_private("dynamic_delay_gpu_integration_pattern", "GPU integration pattern",
						    nullptr);
		require(source_ != nullptr, "Could not create integration pattern");
		obs_set_output_source(0, source_);
	}
	~PatternSource()
	{
		obs_set_output_source(0, nullptr);
		obs_source_release(source_);
	}

private:
	obs_source_t *source_ = nullptr;
};

bool correct_pattern(const QImage &image, const bool alternate)
{
	if (image.width() != static_cast<int>(PreviewCapture::Width) ||
	    image.height() != static_cast<int>(PreviewCapture::Height))
		return false;
	const QColor upper = image.pixelColor(image.width() / 2, image.height() / 4);
	const QColor lower = image.pixelColor(image.width() / 2, 3 * image.height() / 4);
	const auto high = [](const int value) {
		return value > 220;
	};
	const auto low = [](const int value) {
		return value < 35;
	};
	if (alternate)
		return low(upper.red()) && high(upper.green()) && low(upper.blue()) && high(lower.red()) &&
		       high(lower.green()) && low(lower.blue());
	return high(upper.red()) && low(upper.green()) && low(upper.blue()) && low(lower.red()) && low(lower.green()) &&
	       high(lower.blue());
}

PreviewCapture::Frame wait_for_frame(PreviewCapture &capture, const uint64_t after, const bool alternate,
				     const QString &failureImage)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	PreviewCapture::Frame latest;
	while (std::chrono::steady_clock::now() < deadline) {
		pump_events();
		latest = capture.frame_at(std::numeric_limits<uint64_t>::max());
		if (latest.capturedAtNs > after && correct_pattern(latest.image, alternate))
			return latest;
	}
	if (!latest.image.isNull())
		latest.image.save(failureImage);
	throw std::runtime_error("No correctly oriented/color-correct GPU thumbnail arrived; inspect " +
				 failureImage.toStdString());
}

} // namespace

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	if (argc < 3) {
		std::cerr
			<< "Usage: preview_gpu_integration <graphics-module> <artifact-directory> [libobs-data-directory]\n";
		return 2;
	}
	try {
		const std::string graphicsModule = argv[1];
		const QString artifacts = QString::fromLocal8Bit(argv[2]);
		require(QDir().mkpath(artifacts), "Could not create artifact directory");
		QTemporaryDir config(QDir::tempPath() + "/obs-delay-preview-integration-XXXXXX");
		require(config.isValid(), "Could not create isolated libobs configuration directory");
		const QByteArray configPath = config.path().toUtf8();
		ObsInstance obs(configPath.constData());
		if (argc > 3)
			obs_add_data_path(argv[3]);
		obs_audio_info audio{48'000, SPEAKERS_STEREO};
		require(obs_reset_audio(&audio), "obs_reset_audio failed");
		obs_video_info video{};
		video.graphics_module = graphicsModule.c_str();
		video.fps_num = 60;
		video.fps_den = 1;
		video.base_width = video.output_width = 640;
		video.base_height = video.output_height = 360;
		video.output_format = VIDEO_FORMAT_NV12;
		video.gpu_conversion = true;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_PARTIAL;
		video.scale_type = OBS_SCALE_BILINEAR;
		const int reset = obs_reset_video(&video);
		require(reset == OBS_VIDEO_SUCCESS,
			"Initial obs_reset_video failed with code " + std::to_string(reset));
		register_pattern();
		PatternSource source;
		PreviewCapture capture;
		capture.set_history_seconds(3);
		require(capture.start(), "Preview capture did not start");
		require(!obs_video_active(), "Preview incorrectly made OBS video active");
		const auto first = wait_for_frame(capture, 0, false, artifacts + "/failure-initial.png");
		require(first.image.save(artifacts + "/red-blue.png"), "Could not save initial GPU thumbnail");
		std::vector<uint64_t> timestamps{first.capturedAtNs};
		for (int index = 0; index < 3; ++index) {
			const auto next =
				wait_for_frame(capture, timestamps.back(), false, artifacts + "/failure-cadence.png");
			timestamps.push_back(next.capturedAtNs);
		}
		for (std::size_t index = 1; index < timestamps.size(); ++index) {
			const uint64_t interval = timestamps[index] - timestamps[index - 1];
			require(interval >= 500'000'000ULL && interval < 1'100'000'000ULL,
				"Unexpected GPU sampling cadence: " + std::to_string(interval) + " ns");
		}
		require(!obs_video_active(), "GPU sampling registered an output consumer");
		video.base_width = video.output_width = 1280;
		video.base_height = video.output_height = 720;
		const uint64_t beforeReset = timestamps.back();
		const int resized = obs_reset_video(&video);
		require(resized == OBS_VIDEO_SUCCESS,
			"obs_reset_video while preview active failed with code " + std::to_string(resized));
		const auto resizedFrame =
			wait_for_frame(capture, beforeReset, false, artifacts + "/failure-resized.png");
		require(resizedFrame.image.save(artifacts + "/resized.png"), "Could not save resized GPU thumbnail");

		// A delayed recording pause can last longer than the preview window.
		// Pin the frame currently emitted, then ensure that both sides of the
		// wall-clock gap remain available while playback is still behind it.
		capture.set_playback_state(true, false, true, first.capturedAtNs, 0);
		pump_for(std::chrono::milliseconds(75)); // Finish a possible staged readback.
		const auto pausedLatest = capture.frame_at(std::numeric_limits<uint64_t>::max());
		pump_for(std::chrono::milliseconds(4100));
		require(capture.frame_at(std::numeric_limits<uint64_t>::max()).capturedAtNs ==
				pausedLatest.capturedAtNs,
			"Paused preview continued GPU sampling");
		require(capture.frame_at(first.capturedAtNs).capturedAtNs == first.capturedAtNs,
			"Long delayed pause discarded the emitted frame");
		alternatePattern.store(true, std::memory_order_relaxed);
		capture.set_playback_state(false, false, true, first.capturedAtNs, 0);
		const auto resumed =
			wait_for_frame(capture, pausedLatest.capturedAtNs, true, artifacts + "/failure-resumed.png");
		const auto resumedNext =
			wait_for_frame(capture, resumed.capturedAtNs, true, artifacts + "/failure-resumed-next.png");
		require(correct_pattern(capture.frame_at(first.capturedAtNs).image, false),
			"Resuming discarded the still-required pre-pause frame");
		require(correct_pattern(capture.frame_at(resumedNext.capturedAtNs).image, true),
			"Pre-pause protection prevented new GPU thumbnails");
		capture.set_playback_state(false, false, true, resumed.capturedAtNs, 0);
		wait_for_frame(capture, resumedNext.capturedAtNs, true, artifacts + "/failure-advanced-pin.png");
		require(capture.frame_at(first.capturedAtNs).image.isNull(),
			"Advancing playback retained already-consumed pre-pause history");

		// Stop while suspended: a later start must not inherit pause or pins.
		capture.set_playback_state(true, false, true, resumed.capturedAtNs, 0);
		capture.stop();
		require(capture.frame_at(std::numeric_limits<uint64_t>::max()).image.isNull(),
			"Stopped preview retained old history");
		require(capture.started_at_ns() == 0, "Stopped preview retained its capture epoch");
		require(!obs_video_active(), "Stopping preview left OBS video active");
		alternatePattern.store(true, std::memory_order_relaxed);
		require(capture.start(), "Preview capture did not restart");
		require(capture.frame_at(resizedFrame.capturedAtNs).image.isNull(), "Restart exposed an old frame");
		const auto restarted =
			wait_for_frame(capture, resizedFrame.capturedAtNs, true, artifacts + "/failure-restarted.png");
		require(restarted.image.save(artifacts + "/green-yellow.png"),
			"Could not save restarted GPU thumbnail");

		// Reproduce Filling with the ordinary delay+3s history already full.
		// Only the buffer's start is required after pausing; pinning the oldest
		// unrelated live thumbnail instead would hit the hard cap after resume.
		capture.set_history_seconds(6);
		pump_for(std::chrono::milliseconds(6400));
		const auto bufferStart = capture.frame_at(std::numeric_limits<uint64_t>::max());
		require(bufferStart.capturedAtNs >= restarted.capturedAtNs + 6'000'000'000ULL,
			"Filling regression did not prefill the history window");
		capture.set_playback_state(false, true, false, 0, bufferStart.capturedAtNs);
		const auto fillingFrame =
			wait_for_frame(capture, bufferStart.capturedAtNs, true, artifacts + "/failure-filling.png");
		capture.set_playback_state(true, true, false, 0, bufferStart.capturedAtNs);
		pump_for(std::chrono::milliseconds(4100));
		require(capture.frame_at(bufferStart.capturedAtNs).capturedAtNs == bufferStart.capturedAtNs,
			"Paused Filling discarded the buffered content start");
		alternatePattern.store(false, std::memory_order_relaxed);
		capture.set_playback_state(false, true, false, 0, bufferStart.capturedAtNs);
		uint64_t fillingResumeTimestamp = fillingFrame.capturedAtNs;
		for (int index = 0; index < 6; ++index)
			fillingResumeTimestamp = wait_for_frame(capture, fillingResumeTimestamp, false,
								artifacts + "/failure-filling-resume.png")
							 .capturedAtNs;
		capture.set_playback_state(false, false, true, bufferStart.capturedAtNs, 0);
		require(correct_pattern(capture.frame_at(bufferStart.capturedAtNs).image, true),
			"Filling-to-delayed lost the pre-pause buffered frame");
		require(correct_pattern(capture.frame_at(fillingResumeTimestamp).image, false),
			"Full live history blocked post-resume Filling thumbnails");
		capture.stop();
		std::cout
			<< "preview_gpu_integration: PASS; backend=" << graphicsModule
			<< "; capture=320x180@2fps; orientation/colors/reset/restart/delayed-pause/filling-pause verified; "
			   "obs_video_active=false\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "preview_gpu_integration: FAIL: " << error.what() << '\n';
		return 1;
	}
}
