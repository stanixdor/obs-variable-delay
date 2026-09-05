// Opt-in recording integration with real libobs, encoders, and Hybrid MP4.
// Configuration and synthetic sources are isolated from the user's OBS app.
#include "hold-media-hub.hpp"
#include "hold-pipeline.hpp"
#include "output-session.hpp"

#include <obs.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <QDir>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace dynamic_delay;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string &message)
{
	if (!condition)
		throw std::runtime_error(message);
}

struct Synth {
	obs_source_t *source = nullptr;
	vec4 color{};
	double frequency = 440.0;
	std::atomic_bool stopped{false};
	std::thread audio;
	~Synth()
	{
		stopped.store(true);
		if (audio.joinable())
			audio.join();
	}
};

const char *synth_name(void *)
{
	return "Dynamic Delay isolated A/V source";
}
uint32_t synth_width(void *)
{
	return 640;
}
uint32_t synth_height(void *)
{
	return 360;
}
void synth_destroy(void *data)
{
	delete static_cast<Synth *>(data);
}

void *synth_create(obs_data_t *settings, obs_source_t *source)
{
	auto *synth = new Synth;
	synth->source = source;
	synth->frequency = obs_data_get_double(settings, "frequency");
	vec4_set(&synth->color, static_cast<float>(obs_data_get_double(settings, "red")),
		 static_cast<float>(obs_data_get_double(settings, "green")),
		 static_cast<float>(obs_data_get_double(settings, "blue")), 1.0F);
	synth->audio = std::thread([synth] {
		std::array<float, 960> samples{};
		uint64_t sample = 0;
		const uint64_t epoch = os_gettime_ns();
		auto next = Clock::now();
		while (!synth->stopped.load()) {
			for (size_t frame = 0; frame < 480; ++frame) {
				const double phase = static_cast<double>(sample + frame) * synth->frequency / 48'000.0 *
						     6.283185307179586;
				samples[frame * 2] = samples[frame * 2 + 1] =
					static_cast<float>(0.12 * std::sin(phase));
			}
			obs_source_audio audio{};
			audio.data[0] = reinterpret_cast<const uint8_t *>(samples.data());
			audio.frames = 480;
			audio.speakers = SPEAKERS_STEREO;
			audio.samples_per_sec = 48'000;
			audio.timestamp = epoch + sample * 1'000'000'000ULL / 48'000ULL;
			audio.format = AUDIO_FORMAT_FLOAT;
			obs_source_output_audio(synth->source, &audio);
			sample += 480;
			next += std::chrono::milliseconds(10);
			std::this_thread::sleep_until(next);
		}
	});
	return synth;
}

void synth_render(void *data, gs_effect_t *)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &static_cast<Synth *>(data)->color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, 640, 360);
}

void register_synth()
{
	obs_source_info info{};
	info.id = "dynamic_delay_recording_integration_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = synth_name;
	info.create = synth_create;
	info.destroy = synth_destroy;
	info.get_width = synth_width;
	info.get_height = synth_height;
	info.video_render = synth_render;
	obs_register_source(&info);
}

struct ObsInstance {
	explicit ObsInstance(const char *config)
	{
		require(obs_startup("en-US", config, nullptr), "obs_startup failed");
	}
	~ObsInstance() { obs_shutdown(); }
};

class Scene {
public:
	Scene(const char *name, double red, double green, double blue, double frequency)
	{
		scene = obs_scene_create_private(name);
		require(scene != nullptr, "Cannot create private scene");
		obs_data_t *settings = obs_data_create();
		obs_data_set_double(settings, "red", red);
		obs_data_set_double(settings, "green", green);
		obs_data_set_double(settings, "blue", blue);
		obs_data_set_double(settings, "frequency", frequency);
		source = obs_source_create_private("dynamic_delay_recording_integration_source", name, settings);
		obs_data_release(settings);
		require(source && obs_scene_add(scene, source), "Cannot create synthetic scene content");
	}
	~Scene()
	{
		obs_scene_release(scene);
		obs_source_release(source);
	}
	obs_source_t *get() const { return obs_scene_get_source(scene); }

private:
	obs_scene_t *scene = nullptr;
	obs_source_t *source = nullptr;
};

void load_module(const std::filesystem::path &app, const std::string &name)
{
	const auto bundle = app / "Contents" / "PlugIns" / (name + ".plugin");
	const auto binary = bundle / "Contents" / "MacOS" / name;
	const auto resources = bundle / "Contents" / "Resources";
	obs_module_t *module = nullptr;
	const int result = obs_open_module(&module, binary.c_str(), resources.c_str());
	require(result == MODULE_SUCCESS && module && obs_init_module(module), "Cannot load real module " + name);
}

class Recording {
public:
	Recording(const std::filesystem::path &path, const std::string &encoderId, uint32_t frameDivisor,
		  bool attachSession = true)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "path", path.c_str());
		output = obs_output_create("mp4_output", "isolated-hybrid-mp4", settings, nullptr);
		obs_data_release(settings);
		require(output != nullptr, "Hybrid MP4 output unavailable");
		settings = obs_data_create();
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", 1500);
		obs_data_set_int(settings, "keyint_sec", 1);
		obs_data_set_string(settings, "preset", "veryfast");
		obs_data_set_string(settings, "profile", "high");
		obs_data_set_string(settings, "x264opts", "bframes=2 b-adapt=0 scenecut=0");
		obs_data_set_bool(settings, "bframes", true);
		video = obs_video_encoder_create(encoderId.c_str(), "integration-h264", settings, nullptr);
		obs_data_release(settings);
		require(video != nullptr, "Cannot create video encoder " + encoderId);
		obs_encoder_set_video(video, obs_get_video());
		require(obs_encoder_set_frame_rate_divisor(video, frameDivisor), "Cannot set encoder frame divisor");
		settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", 160);
		audio = obs_audio_encoder_create("ffmpeg_aac", "integration-aac", settings, 0, nullptr);
		obs_data_release(settings);
		require(audio != nullptr, "Cannot create AAC encoder");
		obs_encoder_set_audio(audio, obs_get_audio());
		obs_output_set_video_encoder(output, video);
		obs_output_set_audio_encoder(output, audio, 0);
		obs_output_add_packet_callback(output, &Recording::inspect_packet, this);
		require(obs_output_start(output), "Cannot start Hybrid MP4");
		if (attachSession) {
			session = std::make_unique<OutputSession>(output, "Integration recording", [] {});
			std::string error;
			require(session->attach(error), "Cannot attach production callback: " + error);
		}
	}
	~Recording()
	{
		stop();
		session.reset();
		obs_output_remove_packet_callback(output, &Recording::inspect_packet, this);
		obs_output_release(output);
		obs_encoder_release(audio);
		obs_encoder_release(video);
	}
	void tick()
	{
		QGuiApplication::processEvents();
		if (session)
			session->maintenance();
		QThread::msleep(10);
	}
	void wait(double seconds)
	{
		const auto end = Clock::now() + std::chrono::duration<double>(seconds);
		while (Clock::now() < end)
			tick();
	}
	void until(DelayState desired, double seconds)
	{
		const auto end = Clock::now() + std::chrono::duration<double>(seconds);
		auto nextDiagnostic = Clock::now() + std::chrono::seconds(1);
		while (Clock::now() < end) {
			tick();
			const auto snapshot = session->snapshot();
			if (snapshot.state == desired)
				return;
			require(snapshot.state != DelayState::Error, "Production error: " + snapshot.detail);
			if (Clock::now() >= nextDiagnostic) {
				std::cerr << "waiting state=" << static_cast<int>(snapshot.state)
					  << " progress=" << snapshot.progress
					  << " video_packets=" << videoPackets.load()
					  << " audio_packets=" << audioPackets.load()
					  << " last_video_dts=" << videoDts.load()
					  << " video_timebase_num=" << videoTimebaseNum.load() << '\n';
				nextDiagnostic += std::chrono::seconds(1);
			}
		}
		throw std::runtime_error("State timeout: " + session->snapshot().detail);
	}
	void stop()
	{
		if (!output || !obs_output_active(output))
			return;
		obs_output_stop(output);
		const auto end = Clock::now() + std::chrono::seconds(10);
		while (obs_output_active(output) && Clock::now() < end)
			tick();
		if (obs_output_active(output))
			obs_output_force_stop(output);
	}
	obs_output_t *output = nullptr;
	std::unique_ptr<OutputSession> session;
	uint64_t video_packet_count() const { return videoPackets.load(); }

private:
	static void inspect_packet(obs_output_t *, encoder_packet *packet, encoder_packet_time *, void *data)
	{
		auto *self = static_cast<Recording *>(data);
		if (packet->type == OBS_ENCODER_VIDEO) {
			++self->videoPackets;
			self->videoDts = packet->dts_usec;
			self->videoTimebaseNum = packet->timebase_num;
		} else {
			++self->audioPackets;
		}
	}
	std::atomic_uint64_t videoPackets{0};
	std::atomic_uint64_t audioPackets{0};
	std::atomic_int64_t videoDts{0};
	std::atomic_int videoTimebaseNum{0};
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
};
} // namespace

int main(int argc, char **argv)
{
	QGuiApplication application(argc, argv);
	if (argc < 4) {
		std::cerr << "Usage: output_session_integration <OBS.app> <graphics-module> <artifact-directory> "
			     "[video-encoder-id] [frame-rate-divisor] [native-pause|no-pause]\n";
		return 2;
	}
	try {
		// A native encoder deadlock must fail this isolated diagnostic, not
		// keep a CI/manual integration run alive indefinitely.
		std::jthread deadline([](std::stop_token stopped) {
			for (int tick = 0; tick < 600 && !stopped.stop_requested(); ++tick)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (!stopped.stop_requested()) {
				std::cerr << "output_session_integration: FAIL: 60-second process deadline\n";
				std::_Exit(4);
			}
		});
		const std::filesystem::path app = argv[1];
		const std::string graphics = argv[2];
		const std::filesystem::path directory = argv[3];
		const std::string encoder = argc > 4 ? argv[4] : "obs_x264";
		const uint32_t frameDivisor = argc > 5 ? static_cast<uint32_t>(std::stoul(argv[5])) : 1;
		const bool nativePauseOnly = argc > 6 && std::string(argv[6]) == "native-pause";
		const bool exercisePause = !(argc > 6 && std::string(argv[6]) == "no-pause");
		require(frameDivisor > 0 && frameDivisor <= 4, "Expected frame-rate divisor from 1 to 4");
		std::filesystem::create_directories(directory);
		QTemporaryDir config(QDir::tempPath() + "/obs-delay-recording-integration-XXXXXX");
		require(config.isValid(), "Cannot create isolated configuration");
		ObsInstance obs(config.path().toUtf8().constData());
		const auto resources = app / "Contents" / "Frameworks" / "libobs.framework" / "Resources";
		obs_add_data_path(resources.c_str());
		obs_audio_info audio{48'000, SPEAKERS_STEREO};
		require(obs_reset_audio(&audio), "Cannot initialize audio");
		obs_video_info video{};
		video.graphics_module = graphics.c_str();
		video.fps_num = 30 * frameDivisor;
		video.fps_den = 1;
		video.base_width = video.output_width = 640;
		video.base_height = video.output_height = 360;
		video.output_format = VIDEO_FORMAT_NV12;
		video.gpu_conversion = true;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_PARTIAL;
		video.scale_type = OBS_SCALE_BILINEAR;
		require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Cannot initialize graphics");
		load_module(app, "obs-outputs");
		load_module(app, "obs-x264");
		load_module(app, "obs-ffmpeg");
		if (encoder != "obs_x264")
			load_module(app, "mac-videotoolbox");
		obs_post_load_modules();
		register_synth();
		HoldPipeline::register_output_type();
		Scene red("program-red-440Hz", 1, 0, 0, 440);
		Scene blue("program-blue-880Hz", 0, 0, 1, 880);
		Scene green("hold-green-220Hz", 0, 1, 0, 220);
		obs_set_output_source(0, red.get());
		const auto movie = directory / "hybrid-mp4-integration.mp4";
		Recording recording(movie, encoder, frameDivisor, !nativePauseOnly);
		if (nativePauseOnly) {
			// Diagnostic control: no OutputSession, HoldPipeline, or private
			// media hub exists. This isolates native libobs pause semantics.
			recording.wait(1.0);
			for (int index = 0; index < 12; ++index) {
				require(obs_output_pause(recording.output, true), "Native pause failed");
				recording.wait(0.2);
				const uint64_t before = recording.video_packet_count();
				require(obs_output_pause(recording.output, false), "Native resume failed");
				recording.wait(0.7);
				const uint64_t after = recording.video_packet_count();
				std::cerr << "native_pause cycle=" << index << " packets_before=" << before
					  << " packets_after=" << after << '\n';
				if (after < before + 5) {
					// A native encoder stuck in pause also prevents libobs
					// output destruction. Bound the diagnostic process; its
					// incomplete movie is disposable test output only.
					std::cerr << "NATIVE LIBOBS FAILURE: no video after resume, without plugin\n";
					std::_Exit(3);
				}
			}
			recording.stop();
			obs_set_output_source(0, nullptr);
			std::cout << "native_pause_integration: PASS\n";
			return 0;
		}
		std::ofstream events(directory / "recording-events.tsv");
		events << "wall_seconds\tevent\tstate\tprogress\teffective_seconds\tbuffered_bytes\n";
		const auto started = Clock::now();
		auto event = [&](const char *name) {
			const auto snapshot = recording.session->snapshot();
			events << std::chrono::duration<double>(Clock::now() - started).count() << '\t' << name << '\t'
			       << static_cast<int>(snapshot.state) << '\t' << snapshot.progress << '\t'
			       << snapshot.effectiveSeconds << '\t' << snapshot.bufferedBytes << '\n';
			events.flush();
		};
		event("start_red_440Hz");
		recording.wait(1.0);
		std::string error;
		HoldAudioConfig holdConfig{};
		holdConfig.mode = HoldAudioMode::SceneMix;
		auto hub = HoldMediaHub::create(green.get(), holdConfig, error);
		require(hub != nullptr, "Cannot create hold graph: " + error);
		for (int index = 0; index < 3; ++index) {
			require(recording.session->request_delay(3, hub, error), "Rapid activation failed: " + error);
			require(recording.session->state() == DelayState::Preparing,
				"Expected Preparing before cancel");
			recording.session->request_bypass();
		}
		event("three_rapid_cancels_complete");
		require(recording.session->request_delay(3, hub, error), "Activation failed: " + error);
		recording.until(DelayState::Filling, 10.0);
		event("filling_green_220Hz");
		recording.wait(0.35);
		DelaySnapshot pausedBefore{};
		DelaySnapshot pausedAfter{};
		if (exercisePause) {
			require(obs_output_pause(recording.output, true), "Real recording pause failed");
			recording.session->sync_pause_state();
			event("paused");
			recording.wait(0.35);
			pausedBefore = recording.session->snapshot();
			recording.wait(2.5);
			pausedAfter = recording.session->snapshot();
			require(pausedAfter.state == DelayState::Filling && pausedAfter.paused, "Pause left Filling");
			require(pausedBefore.bufferedBytes == pausedAfter.bufferedBytes,
				"Hold buffer grew during pause");
			require(pausedBefore.progress == pausedAfter.progress, "Media clock advanced during pause");
			require(obs_output_pause(recording.output, false), "Real recording resume failed");
			recording.session->sync_pause_state();
			event("resumed");
		}
		recording.until(DelayState::Delayed, 10.0);
		event("delayed");
		const auto delayed = recording.session->snapshot();
		require(delayed.effectiveSeconds >= 2.9 && delayed.effectiveSeconds < 7.0,
			"Unexpected effective age after paused fill");
		obs_set_output_source(0, blue.get());
		event("program_switch_blue_880Hz");
		recording.wait(4.5);
		require(recording.session->state() == DelayState::Delayed, "Scene switch disturbed delay");
		recording.session->request_bypass();
		event("remove_delay_requested");
		recording.until(DelayState::Bypass, 5.0);
		event("live_blue_880Hz");
		recording.wait(1.0);
		recording.stop();
		event("stopped");
		require(!obs_output_active(recording.output), "Recording did not stop cleanly");
		require(std::filesystem::file_size(movie) > 10'000, "Hybrid MP4 unexpectedly small");
		std::ofstream metrics(directory / "recording-metrics.txt");
		metrics << "encoder=" << encoder << "\nencoder_frame_rate_divisor=" << frameDivisor
			<< "\nhold_audio=SceneMix (220Hz)\n"
			<< "program_audio=440Hz then 880Hz\nrapid_cancel_rearm_cycles=3\n"
			<< "pause_exercised=" << exercisePause << '\n'
			<< "paused_hold_buffer_bytes=" << pausedAfter.bufferedBytes << '\n'
			<< "paused_hold_buffer_growth=0\npaused_progress=" << pausedAfter.progress << '\n'
			<< "initial_effective_delay_seconds=" << delayed.effectiveSeconds << '\n'
			<< "configured_media_delay_seconds=3\nresult=PASS\n";
		obs_set_output_source(0, nullptr);
		std::cout << "output_session_integration: PASS; encoder=" << encoder << "; movie=" << movie << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "output_session_integration: FAIL: " << error.what() << '\n';
		return 1;
	}
}
