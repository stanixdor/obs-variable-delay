// Opt-in real-libobs integration. Every RTMP endpoint is hard-coded loopback;
// the OBS application, user profiles, scenes, and real services are untouched.
#include "hold-media-hub.hpp"
#include "hold-pipeline.hpp"
#include "delay-controller.hpp"
#include "multistream-controller.hpp"
#include "multistream-encoders.hpp"
#include "multistream-transport.hpp"
#include "output-session.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <signal.h>
#endif

namespace {
using namespace dynamic_delay;
using Clock = std::chrono::steady_clock;
config_t *testProfile = nullptr;
std::string testProfilePath;
std::atomic_uint nativeOutputQueries{0};
obs_output_t *testNativeOutput = nullptr;
std::vector<std::pair<obs_frontend_event_cb, void *>> frontendCallbacks;
std::vector<obs_source_t *> frontendScenes;

void frontend_event(obs_frontend_event event)
{
	const auto callbacks = frontendCallbacks;
	for (const auto &[callback, data] : callbacks)
		callback(event, data);
}

void require(bool ok, const std::string &message)
{
	if (!ok)
		throw std::runtime_error(message);
}

struct ObsInstance {
	explicit ObsInstance(const char *directory)
	{
		require(obs_startup("en-US", directory, nullptr), "obs_startup failed");
	}
	~ObsInstance() { obs_shutdown(); }
};

struct Profile {
	explicit Profile(const QString &directory)
	{
		testProfilePath = directory.toStdString();
		require(config_open(&testProfile, (testProfilePath + "/basic.ini").c_str(), CONFIG_OPEN_ALWAYS) ==
				CONFIG_SUCCESS,
			"Cannot create isolated profile");
		config_set_string(testProfile, "Output", "Mode", "Simple");
		config_set_string(testProfile, "SimpleOutput", "StreamEncoder", "x264");
		config_set_string(testProfile, "SimpleOutput", "StreamAudioEncoder", "aac");
		config_set_uint(testProfile, "SimpleOutput", "VBitrate", 2500);
		config_set_uint(testProfile, "SimpleOutput", "ABitrate", 160);
		config_set_string(testProfile, "SimpleOutput", "Preset", "veryfast");
		config_set_bool(testProfile, "SimpleOutput", "UseAdvanced", true);
		config_set_string(testProfile, "SimpleOutput", "x264Settings",
				  "bframes=2 b-adapt=0 scenecut=0 keyint=30");
	}
	~Profile()
	{
		config_close(testProfile);
		testProfile = nullptr;
	}
};

struct Synth {
	obs_source_t *source = nullptr;
	vec4 color{};
	double frequency = 440;
	std::jthread thread;
};

void *create_synth(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new Synth;
	data->source = source;
	data->frequency = obs_data_get_double(settings, "frequency");
	vec4_set(&data->color, static_cast<float>(obs_data_get_double(settings, "r")),
		 static_cast<float>(obs_data_get_double(settings, "g")),
		 static_cast<float>(obs_data_get_double(settings, "b")), 1);
	data->thread = std::jthread([data](std::stop_token stopped) {
		std::array<float, 960> samples{};
		uint64_t sample = 0;
		const uint64_t epoch = os_gettime_ns();
		auto next = Clock::now();
		while (!stopped.stop_requested()) {
			for (size_t frame = 0; frame < 480; ++frame) {
				const double phase = static_cast<double>(sample + frame) * data->frequency / 48'000 *
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
			obs_source_output_audio(data->source, &audio);
			sample += 480;
			next += std::chrono::milliseconds(10);
			std::this_thread::sleep_until(next);
		}
	});
	return data;
}

void render_synth(void *data, gs_effect_t *)
{
	auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &static_cast<Synth *>(data)->color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, 640, 360);
}

void register_synth()
{
	obs_source_info info{};
	info.id = "dynamic_delay_multistream_test_synth";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Isolated multistream A/V";
	};
	info.create = create_synth;
	info.destroy = [](void *data) {
		delete static_cast<Synth *>(data);
	};
	info.get_width = [](void *) -> uint32_t {
		return 640;
	};
	info.get_height = [](void *) -> uint32_t {
		return 360;
	};
	info.video_render = render_synth;
	obs_register_source(&info);
}

class Scene {
public:
	Scene(const char *name, double r, double g, double b, double frequency)
	{
		scene_ = obs_scene_create_private(name);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_double(settings, "r", r);
		obs_data_set_double(settings, "g", g);
		obs_data_set_double(settings, "b", b);
		obs_data_set_double(settings, "frequency", frequency);
		OBSSourceAutoRelease source =
			obs_source_create_private("dynamic_delay_multistream_test_synth", name, settings);
		require(scene_ && source && obs_scene_add(scene_, source), "Cannot create test scene");
	}
	~Scene() { obs_scene_release(scene_); }
	obs_source_t *get() const { return obs_scene_get_source(scene_); }

private:
	obs_scene_t *scene_ = nullptr;
};

void load_module(const std::filesystem::path &app, const char *name)
{
	const auto bundle = app / "Contents" / "PlugIns" / (std::string(name) + ".plugin");
	obs_module_t *module = nullptr;
	require(obs_open_module(&module, (bundle / "Contents" / "MacOS" / name).c_str(),
				(bundle / "Contents" / "Resources").c_str()) == MODULE_SUCCESS &&
			obs_init_module(module),
		"Cannot load module " + std::string(name));
}

class Master {
public:
	explicit Master(MultistreamTransport &transport) : transport_(transport)
	{
		obs_output_info info{};
		info.id = "dynamic_delay_multistream_test_master";
		info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
		info.get_name = [](void *) {
			return "Isolated network-independent delay master";
		};
		info.create = [](obs_data_t *settings, obs_output_t *) -> void * {
			return reinterpret_cast<void *>(static_cast<uintptr_t>(obs_data_get_int(settings, "owner")));
		};
		info.destroy = [](void *) {
		};
		info.start = [](void *data) {
			auto *self = static_cast<Master *>(data);
			return obs_output_initialize_encoders(self->output_, 0) &&
			       obs_output_begin_data_capture(self->output_, 0);
		};
		info.stop = [](void *data, uint64_t) {
			auto *self = static_cast<Master *>(data);
			obs_output_end_data_capture(self->output_);
			obs_output_signal_stop(self->output_, OBS_OUTPUT_SUCCESS);
		};
		info.encoded_packet = [](void *data, encoder_packet *packet) {
			static_cast<Master *>(data)->receive(packet);
		};
		obs_register_output(&info);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "owner", static_cast<int64_t>(reinterpret_cast<uintptr_t>(this)));
		output_ = obs_output_create(info.id, "isolated-multistream-master", settings, nullptr);
		std::string error;
		require(configure_multistream_encoders(output_, error), "Encoder configuration: " + error);
		session = std::make_unique<OutputSession>(output_, "Multistream integration", [] {});
		require(session->attach(error), "Cannot attach production delay: " + error);
		require(obs_output_start(output_), "Cannot start isolated collector");
	}
	~Master()
	{
		transport_.stop_all();
		obs_output_force_stop(output_);
		session.reset();
		obs_output_release(output_);
	}
	void tick()
	{
		QGuiApplication::processEvents();
		session->maintenance();
		require(!failed_.load(), "Master encoder callback failed");
		QThread::msleep(10);
	}
	void wait(double seconds)
	{
		const auto end = Clock::now() + std::chrono::duration<double>(seconds);
		while (Clock::now() < end)
			tick();
	}
	void until(const std::function<bool()> &predicate, double seconds, const char *failure)
	{
		const auto end = Clock::now() + std::chrono::duration<double>(seconds);
		while (Clock::now() < end) {
			tick();
			if (predicate())
				return;
			require(session->state() != DelayState::Error, "Delay error: " + session->snapshot().detail);
		}
		throw std::runtime_error(failure);
	}
	StreamDescription description()
	{
		StreamDescription result;
		obs_encoder_t *video = obs_output_get_video_encoder(output_);
		obs_encoder_t *audio = obs_output_get_audio_encoder(output_, 0);
		until(
			[&] {
				uint8_t *bytes = nullptr;
				size_t size = 0;
				if (!obs_encoder_get_extra_data(video, &bytes, &size) || !size)
					return false;
				result.videoExtraData.assign(bytes, bytes + size);
				if (!obs_encoder_get_extra_data(audio, &bytes, &size) || !size)
					return false;
				result.audioExtraData.assign(bytes, bytes + size);
				return true;
			},
			10, "Encoder metadata unavailable");
		result.width = obs_encoder_get_width(video);
		result.height = obs_encoder_get_height(video);
		result.sampleRate = obs_encoder_get_sample_rate(audio);
		result.channels = 2;
		return result;
	}
	uint64_t video_packets() const { return videoPackets_.load(); }
	obs_output_t *output() const { return output_; }
	std::unique_ptr<OutputSession> session;

private:
	void receive(encoder_packet *packet) noexcept
	{
		if (!packet) {
			failed_ = true;
			return;
		}
		try {
			if (packet->type == OBS_ENCODER_VIDEO)
				++videoPackets_;
			SharedEncodedPacket shared;
			shared.data =
				std::make_shared<const std::vector<uint8_t>>(packet->data, packet->data + packet->size);
			shared.video = packet->type == OBS_ENCODER_VIDEO;
			shared.keyframe = packet->keyframe;
			// OBS packet timestamps are denominator ticks; the numerator is
			// nominal frame duration, NOT a second scale factor.
			shared.ptsUsec = packet->pts * 1'000'000LL / packet->timebase_den;
			shared.dtsUsec = packet->dts * 1'000'000LL / packet->timebase_den;
			transport_.submit(shared);
		} catch (...) {
			failed_ = true;
		}
	}
	MultistreamTransport &transport_;
	obs_output_t *output_ = nullptr;
	std::atomic_bool failed_{false};
	std::atomic_uint64_t videoPackets_{0};
};

void encoder_profile_checks(Master &master)
{
	auto emptyOutput = [&]() -> obs_output_t * {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "owner", static_cast<int64_t>(reinterpret_cast<uintptr_t>(&master)));
		return obs_output_create("dynamic_delay_multistream_test_master", "encoder-profile-probe", settings,
					 nullptr);
	};
	std::string error;
	{
		OBSOutputAutoRelease probe = emptyOutput();
		require(probe != nullptr, "Cannot create encoder profile probe");
		testNativeOutput = master.output();
		const bool configured = configure_multistream_encoders(probe, error);
		testNativeOutput = nullptr;
		require(configured, "Active encoder sharing failed: " + error);
		require(obs_output_get_video_encoder(probe) == obs_output_get_video_encoder(master.output()) &&
				obs_output_get_audio_encoder(probe, 0) ==
					obs_output_get_audio_encoder(master.output(), 0),
			"Compatible active main encoders were not shared");
	}
	{
		OBSOutputAutoRelease probe = emptyOutput();
		config_set_string(testProfile, "SimpleOutput", "StreamAudioEncoder", "opus");
		const bool configured = configure_multistream_encoders(probe, error);
		config_set_string(testProfile, "SimpleOutput", "StreamAudioEncoder", "aac");
		require(!configured && error.find("AAC") != std::string::npos && !obs_output_get_video_encoder(probe) &&
				!obs_output_get_audio_encoder(probe, 0),
			"Unsupported configured audio was silently replaced");
	}
	config_set_string(testProfile, "Output", "Mode", "Advanced");
	config_set_string(testProfile, "AdvOut", "Encoder", "obs_x264");
	config_set_string(testProfile, "AdvOut", "AudioEncoder", "ffmpeg_aac");
	config_set_uint(testProfile, "AdvOut", "TrackIndex", 2);
	config_set_uint(testProfile, "AdvOut", "Track2Bitrate", 192);
	config_set_uint(testProfile, "AdvOut", "RescaleFilter", OBS_SCALE_BICUBIC);
	config_set_string(testProfile, "AdvOut", "RescaleRes", "320x180");
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", 1234);
	obs_data_set_int(settings, "keyint_sec", 3);
	obs_data_set_string(settings, "preset", "superfast");
	require(obs_data_save_json(settings, (testProfilePath + "/streamEncoder.json").c_str()),
		"Cannot create temporary Advanced encoder fixture");
	{
		OBSOutputAutoRelease probe = emptyOutput();
		require(configure_multistream_encoders(probe, error),
			"Advanced profile configuration failed: " + error);
		obs_encoder_t *video = obs_output_get_video_encoder(probe);
		obs_encoder_t *audio = obs_output_get_audio_encoder(probe, 0);
		OBSDataAutoRelease actual = obs_encoder_get_settings(video);
		OBSDataAutoRelease audioSettings = obs_encoder_get_settings(audio);
		require(obs_data_get_int(actual, "bitrate") == 1234 && obs_data_get_int(actual, "keyint_sec") == 3 &&
				std::string(obs_data_get_string(actual, "preset")) == "superfast" &&
				obs_encoder_get_width(video) == 320 && obs_encoder_get_height(video) == 180 &&
				obs_encoder_get_mixer_index(audio) == 1 &&
				obs_data_get_int(audioSettings, "bitrate") == 192,
			"Advanced encoder settings, rescale, or audio track were not preserved");
	}
	{
		OBSOutputAutoRelease probe = emptyOutput();
		config_set_string(testProfile, "AdvOut", "Encoder", "ffmpeg_av1");
		require(!configure_multistream_encoders(probe, error) && !obs_output_get_video_encoder(probe),
			"Unsupported Advanced video was silently replaced");
	}
	config_set_string(testProfile, "Output", "Mode", "Simple");
}

class Receiver {
public:
	Receiver(const QString &ffmpeg, int port, const std::filesystem::path &file)
	{
		process_.setProcessChannelMode(QProcess::MergedChannels);
		const QString url = QString("rtmp://127.0.0.1:%1/live/integration").arg(port);
		process_.start(ffmpeg, {"-hide_banner", "-loglevel", "warning", "-y", "-listen", "1", "-timeout", "15",
					"-i", url, "-c", "copy", "-f", "flv", QString::fromStdString(file.string())});
		require(process_.waitForStarted(3000), "Cannot launch local ffmpeg receiver");
		QThread::msleep(200);
		require(process_.state() == QProcess::Running,
			"Local receiver failed: " + process_.readAll().toStdString());
	}
	~Receiver() { stop(); }
	void stop()
	{
#ifndef _WIN32
		if (suspended_ && process_.state() != QProcess::NotRunning)
			::kill(static_cast<pid_t>(process_.processId()), SIGCONT);
#endif
		suspended_ = false;
		if (process_.state() == QProcess::NotRunning)
			return;
		process_.terminate();
		if (!process_.waitForFinished(3000)) {
			process_.kill();
			process_.waitForFinished(1000);
		}
	}
	void wait_closed()
	{
		if (!process_.waitForFinished(5000))
			stop();
	}
	void suspend_reads()
	{
#ifndef _WIN32
		require(::kill(static_cast<pid_t>(process_.processId()), SIGSTOP) == 0,
			"Cannot suspend local slow receiver");
		suspended_ = true;
#endif
	}

private:
	QProcess process_;
	bool suspended_ = false;
};

MultistreamStatus status(MultistreamTransport &transport, const char *id)
{
	for (const auto &item : transport.snapshot()) {
		if (item.id == id)
			return item;
	}
	throw std::runtime_error("Target status missing");
}

void wait_until(const std::function<bool()> &predicate, double seconds, const std::string &failure)
{
	const auto end = Clock::now() + std::chrono::duration<double>(seconds);
	while (Clock::now() < end) {
		QGuiApplication::processEvents();
		if (predicate())
			return;
		QThread::msleep(10);
	}
	throw std::runtime_error(failure);
}

void pump_events(double seconds)
{
	const auto end = Clock::now() + std::chrono::duration<double>(seconds);
	wait_until([&] { return Clock::now() >= end; }, seconds + 1, "Event pump stalled");
}

void controller_checks(const QString &ffmpeg, const std::filesystem::path &directory, obs_source_t *red,
		       obs_source_t *blue, obs_source_t *green)
{
	config_set_string(testProfile, "Output", "Mode", "Advanced");
	config_set_string(testProfile, "AdvOut", "Encoder", "com.apple.videotoolbox.videoencoder.ave.avc");
	config_set_string(testProfile, "AdvOut", "AudioEncoder", "ffmpeg_aac");
	config_set_uint(testProfile, "AdvOut", "TrackIndex", 1);
	config_set_uint(testProfile, "AdvOut", "Track1Bitrate", 160);
	config_set_uint(testProfile, "AdvOut", "RescaleFilter", OBS_SCALE_DISABLE);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", 2500);
	obs_data_set_int(settings, "keyint_sec", 1);
	obs_data_set_bool(settings, "bframes", true);
	require(obs_data_save_json(settings, (testProfilePath + "/streamEncoder.json").c_str()),
		"Cannot create isolated hardware encoder profile");
	frontendScenes = {red, blue, green};
	obs_set_output_source(0, red);
	DelayController delay;
	delay.set_hold_scene(QString::fromUtf8(obs_source_get_uuid(green)));
	delay.set_delay_seconds(2);
	MultistreamController::register_output_type();
	const std::string id = "controller-primary";
	const MultistreamTarget target{id, "Controller / Prueba", "rtmp://127.0.0.1:19364/live", "integration"};
	QString error;
	{
		MultistreamController controller(delay);
		require(controller.targets().empty(), "Isolated controller unexpectedly loaded targets");
		require(!controller.save_target({"bad", "Invalid", "https://127.0.0.1/live", "integration"}, error),
			"Non-RTMP controller destination accepted");
		require(!controller.save_target({"bad", "Invalid", "rtmp://user:pass@127.0.0.1/live", "integration"},
						error),
			"URL-embedded credentials accepted");
		require(!controller.save_target({"bad", "Invalid", "rtmp://127.0.0.1:0/live", "integration"}, error),
			"Invalid port zero accepted before asynchronous startup");
		require(controller.save_target(target, error),
			"Cannot save isolated controller destination: " + error.toStdString());
		for (int index = 1; index < 8; ++index)
			require(controller.save_target({"unused-" + std::to_string(index), "Unused loopback",
							"rtmp://127.0.0.1:19364/live", "integration"},
						       error),
				"Cannot save up to eight destinations");
		require(!controller.save_target({"ninth", "Overflow", "rtmp://127.0.0.1:19364/live", "integration"},
						error),
			"More than eight destinations accepted");
		for (int index = 1; index < 8; ++index)
			require(controller.remove_target("unused-" + std::to_string(index), error),
				"Cannot remove unused target");
		char *configPath = obs_module_config_path("multistream.json");
		const std::string path = configPath ? configPath : "";
		bfree(configPath);
		require(path.starts_with(testProfilePath + "/"),
			"Controller attempted to leave isolated configuration");
		QFile file(QString::fromStdString(path));
		require(file.open(QIODevice::ReadOnly) && file.readAll().contains("Controller / Prueba"),
			"Controller did not persist its isolated settings");
		const auto permissions = QFileInfo(file).permissions();
		require((permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther |
					QFileDevice::WriteOther)) == 0,
			"Credential file is readable or writable by other users");
	}
	bool metadataPending = false;
	{
		MultistreamController controller(delay);
		require(controller.targets().size() == 1 && controller.targets().front().id == id &&
				controller.targets().front().key == target.key && !controller.running(id),
			"Reload lost settings or started publishing automatically");
		pump_events(0.3);
		require(delay.snapshot().activeOutputs == 0 && !controller.running(id),
			"Controller auto-started on reload");
		require(controller.start_target(id, error), "Immediate-cancel startup failed: " + error.toStdString());
		controller.stop_target(id);
		pump_events(0.1);
		require(!controller.running(id) && delay.snapshot().activeOutputs == 0,
			"Cancelling while encoder metadata is pending retained the master");
		Receiver receiver(ffmpeg, 19364, directory / "controller-hardware.flv");
		Receiver secondReceiver(ffmpeg, 19365, directory / "controller-second.flv");
		require(controller.save_target({"controller-secondary", "Controller second",
						"rtmp://127.0.0.1:19365/live", "integration"},
					       error),
			"Cannot save second controller destination");
		require(controller.start_target(id, error), "Real controller startup failed: " + error.toStdString());
		metadataPending = controller.statuses().front().state == MultistreamState::Connecting;
		require(controller.start_target("controller-secondary", error), "Second pending startup failed");
		require(!controller.remove_target(id, error) && !controller.save_target(target, error),
			"An active destination was editable or removable");
		wait_until(
			[&] {
				const auto states = controller.statuses();
				return states.size() == 2 &&
				       std::all_of(states.begin(), states.end(), [](const auto &s) {
					       return s.state == MultistreamState::Streaming;
				       });
			},
			15, "Production controller did not publish both pending hardware destinations");
		require(delay.snapshot().activeOutputs == 1,
			"Production controller did not register one shared delay session");
		pump_events(1.5);
		delay.toggle_delay();
		wait_until([&] { return delay.snapshot().state == DelayState::Delayed; }, 15,
			   "Controller delay did not activate");
		obs_set_output_source(0, blue);
		pump_events(3.5);
		delay.toggle_delay();
		wait_until([&] { return delay.snapshot().state == DelayState::Bypass; }, 8,
			   "Controller return-to-live did not complete");
		pump_events(1);
		controller.stop_target("controller-secondary");
		secondReceiver.wait_closed();
		require(controller.running(id) && delay.snapshot().activeOutputs == 1,
			"Stopping one destination discarded the shared master used by another");
		require(controller.remove_target("controller-secondary", error),
			"Cannot remove stopped controller target");
		controller.stop_target(id);
		wait_until([&] { return !controller.running(id) && delay.snapshot().activeOutputs == 0; }, 5,
			   "Stopping the last destination retained the capture encoder");
		receiver.wait_closed();
		for (const auto event :
		     {OBS_FRONTEND_EVENT_PROFILE_CHANGING, OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING}) {
			const auto filename = event == OBS_FRONTEND_EVENT_PROFILE_CHANGING ? "profile-stop.flv"
											   : "collection-stop.flv";
			Receiver eventReceiver(ffmpeg, 19364, directory / filename);
			require(controller.start_target(id, error),
				"Controller restart failed: " + error.toStdString());
			wait_until([&] { return controller.statuses().front().state == MultistreamState::Streaming; },
				   15, "Controller did not restart for frontend event test");
			pump_events(0.5);
			frontend_event(event);
			wait_until([&] { return !controller.running(id) && delay.snapshot().activeOutputs == 0; }, 5,
				   "Profile or collection change left a destination publishing");
			eventReceiver.wait_closed();
		}
	}
	{
		MultistreamController reloaded(delay);
		pump_events(0.2);
		require(!reloaded.running(id) && delay.snapshot().activeOutputs == 0,
			"Stopped output restarted after reload");
	}
	std::ofstream metrics(directory / "controller-metrics.txt");
	metrics << "result=PASS\nreal_multistream_controller_and_delay_controller=1\n"
		<< "save_reload_limit_validation_owner_permissions=PASS\nno_autostart=PASS\n"
		<< "cancel_pending_metadata=PASS\n"
		<< "two_pending_targets_one_shared_session=PASS\nstop_one_preserves_master=PASS\n"
		<< "native_streaming_output=null\nencoder=VideoToolbox_H264\nmetadata_async_pending_observed="
		<< metadataPending << "\nprofile_and_collection_stop=PASS\nlast_target_releases_master=PASS\n";
	frontendScenes.clear();
}

} // namespace

// Only the frontend boundary is controlled. libobs, configured encoder creation,
// delay engine, hold A/V pipeline, and network transport are the real production
// implementations. In particular there is no native streaming output to borrow.
config_t *obs_frontend_get_profile_config()
{
	return testProfile;
}
char *obs_frontend_get_current_profile_path()
{
	return bstrdup(testProfilePath.c_str());
}
obs_output_t *obs_frontend_get_streaming_output()
{
	++nativeOutputQueries;
	return obs_output_get_ref(testNativeOutput);
}
obs_service_t *obs_frontend_get_streaming_service()
{
	return nullptr;
}
obs_output_t *obs_frontend_get_recording_output()
{
	return nullptr;
}
bool obs_frontend_streaming_active()
{
	return false;
}
bool obs_frontend_recording_active()
{
	return false;
}
void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *data)
{
	frontendCallbacks.emplace_back(callback, data);
}
void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *data)
{
	std::erase(frontendCallbacks, std::pair(callback, data));
}
void obs_frontend_get_scenes(obs_frontend_source_list *list)
{
	for (obs_source_t *source : frontendScenes) {
		source = obs_source_get_ref(source);
		da_push_back(list->sources, &source);
	}
}
obs_module_t *obs_current_module()
{
	return obs_get_module("obs-x264");
}
const char *obs_module_text(const char *text)
{
	return text;
}
namespace dynamic_delay {
std::size_t audience_preview_estimated_bytes(uint32_t) noexcept
{
	return 0;
}
} // namespace dynamic_delay

int main(int argc, char **argv)
{
	QGuiApplication application(argc, argv);
	if (argc < 4) {
		std::cerr
			<< "Usage: multistream_integration <OBS.app> <graphics-module> <artifact-directory> [ffmpeg-path]\n";
		return 2;
	}
	try {
		std::jthread deadline([](std::stop_token stopped) {
			for (int index = 0; index < 1500 && !stopped.stop_requested(); ++index)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (!stopped.stop_requested()) {
				std::cerr << "FAIL: isolated integration exceeded 150 seconds\n";
				std::_Exit(4);
			}
		});
		const std::filesystem::path app = argv[1];
		const std::string graphics = argv[2];
		const std::filesystem::path directory = argv[3];
		const QString ffmpeg = argc > 4 ? QString::fromUtf8(argv[4])
						: QStringLiteral("/opt/homebrew/bin/ffmpeg");
		std::filesystem::create_directories(directory);
		QTemporaryDir temporary(QDir::tempPath() + "/obs-multistream-integration-XXXXXX");
		require(temporary.isValid(), "Cannot create isolated configuration");
		Profile profile(temporary.path());
		ObsInstance obs(temporary.path().toUtf8().constData());
		obs_add_data_path((app / "Contents" / "Frameworks" / "libobs.framework" / "Resources").c_str());
		obs_audio_info audio{48'000, SPEAKERS_STEREO};
		require(obs_reset_audio(&audio), "Cannot initialize real audio");
		obs_video_info video{};
		video.graphics_module = graphics.c_str();
		// Fractional cadence catches accidental multiplication of OBS PTS/DTS
		// by timebase_num (already included in the packet timestamp ticks).
		video.fps_num = 30'000;
		video.fps_den = 1001;
		video.base_width = video.output_width = 640;
		video.base_height = video.output_height = 360;
		video.output_format = VIDEO_FORMAT_NV12;
		video.gpu_conversion = true;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_PARTIAL;
		video.scale_type = OBS_SCALE_BILINEAR;
		require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Cannot initialize real graphics");
		load_module(app, "obs-x264");
		load_module(app, "obs-ffmpeg");
		load_module(app, "mac-videotoolbox");
		obs_post_load_modules();
		register_synth();
		HoldPipeline::register_output_type();
		Scene red("red-program-440Hz", 1, 0, 0, 440);
		Scene blue("blue-program-880Hz", 0, 0, 1, 880);
		Scene green("green-hold-220Hz", 0, 1, 0, 220);
		obs_set_output_source(0, red.get());
		{
			MultistreamTransport transport;
			Master master(transport);
			const auto description = master.description();
			require(nativeOutputQueries.load() > 0, "Profile encoder fallback was not exercised");
			encoder_profile_checks(master);
			std::string error;
			HoldAudioConfig holdAudio;
			holdAudio.mode = HoldAudioMode::SceneMix;
			auto hub = HoldMediaHub::create(green.get(), holdAudio, error);
			require(hub != nullptr, "Cannot create real hold graph: " + error);
			for (int cycle = 0; cycle < 3; ++cycle) {
				require(master.session->request_delay(2, hub, error),
					"Rapid delay activation: " + error);
				master.session->request_bypass();
			}
			master.wait(0.2);
			Receiver fast(ffmpeg, 19361, directory / "fast.flv");
			auto secondary =
				std::make_unique<Receiver>(ffmpeg, 19362, directory / "secondary-before-reconnect.flv");
			require(transport.start({"fast", "Fast loopback", "rtmp://127.0.0.1:19361/live", "integration"},
						description, error),
				error);
			// The reference connection encloses every secondary's time window,
			// allowing a bit-for-bit check of all remuxed payloads afterward.
			master.until([&] { return status(transport, "fast").state == MultistreamState::Streaming; }, 15,
				     "Reference loopback target did not start");
			require(transport.start({"secondary", "Second loopback", "rtmp://127.0.0.1:19362/live",
						 "integration"},
						description, error),
				error);
			master.until(
				[&] {
					return status(transport, "fast").state == MultistreamState::Streaming &&
					       status(transport, "secondary").state == MultistreamState::Streaming;
				},
				15, "Loopback targets did not start");
			master.wait(1.5);
			require(master.session->request_delay(2, hub, error), "Delay activation failed: " + error);
			master.until([&] { return master.session->state() == DelayState::Delayed; }, 10,
				     "Delay never activated");
			obs_set_output_source(0, blue.get());
			master.wait(3.5);
			master.session->request_bypass();
			master.until([&] { return master.session->state() == DelayState::Bypass; }, 5,
				     "Return-to-live failed");
			master.wait(1.0);
			const uint64_t beforeReconnect = status(transport, "fast").bytesSent;
			secondary.reset();
			master.wait(0.5);
			secondary =
				std::make_unique<Receiver>(ffmpeg, 19362, directory / "secondary-after-reconnect.flv");
			master.until(
				[&] {
					const auto s = status(transport, "secondary");
					return s.reconnectAttempts > 0 && s.state == MultistreamState::Streaming;
				},
				20, "Secondary did not reconnect safely");
			require(status(transport, "fast").bytesSent > beforeReconnect,
				"One reconnect blocked the healthy destination");
			Receiver slow(ffmpeg, 19363, directory / "slow.flv");
			require(transport.start({"slow", "Slow loopback", "rtmp://127.0.0.1:19363/live", "integration"},
						description, error),
				error);
			master.until([&] { return status(transport, "slow").state == MultistreamState::Streaming; }, 10,
				     "Slow target never started");
			master.wait(1.0);
			slow.suspend_reads();
			const uint64_t beforeSlow = status(transport, "fast").bytesSent;
			const uint64_t framesBeforeSlow = master.video_packets();
			size_t maxQueuedBytes = 0;
			for (int interval = 0; interval < 150; ++interval) {
				master.wait(0.1);
				const auto snapshot = status(transport, "slow");
				maxQueuedBytes = std::max(maxQueuedBytes, snapshot.queuedBytes);
				require(snapshot.queuedBytes <= 8 * 1024 * 1024,
					"Slow destination queue exceeded safety bound");
			}
			require(master.video_packets() >= framesBeforeSlow + 350,
				"Slow destination blocked the master encoder");
			require(status(transport, "fast").bytesSent > beforeSlow + 100'000,
				"Slow destination blocked the healthy target");
			require(status(transport, "slow").reconnectAttempts > 0,
				"Suspended receiver did not exercise backpressure/disconnection recovery");
			transport.stop("slow");
			slow.stop();
			transport.stop("secondary");
			secondary->wait_closed();
			master.wait(0.1);
			transport.stop_all();
			fast.wait_closed();
			std::ofstream metrics(directory / "multistream-metrics.txt");
			metrics << "result=PASS\nreal_libobs_encoders_and_delay=1\nnative_streaming_output=null\n"
				<< "fps=30000/1001\n"
				<< "profile_fallback_advanced_rescale_audio_track_and_rejections=PASS\nactive_encoder_sharing=PASS\n"
				<< "configured_delay_seconds=2\nrapid_cancel_rearm_cycles=3\nindependent_reconnect=PASS\n"
				<< "slow_receiver_master_continued=PASS\nslow_receiver_max_queued_bytes="
				<< maxQueuedBytes << '\n';
		}
		controller_checks(ffmpeg, directory, red.get(), blue.get(), green.get());
		require(frontendCallbacks.empty(), "Controller frontend callbacks outlived their owners");
		obs_set_output_source(0, nullptr);
		std::cout << "multistream_integration: PASS; artifacts=" << directory << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "multistream_integration: FAIL: " << error.what() << '\n';
		return 1;
	}
}
