// Production HoldPipeline/SharedHoldEncoding with a deterministic libobs
// boundary. Audio/video rendering itself is covered by the OBS smoke tests.
#include "hold-pipeline.hpp"
#include "hold-media-hub.hpp"
#include "output-session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using dynamic_delay::HoldAudioConfig;
using dynamic_delay::HoldAudioTap;
using dynamic_delay::HoldMediaHub;
using dynamic_delay::HoldPipeline;
using dynamic_delay::OutputSession;

namespace {

void check(const bool condition, const char *expression)
{
	if (!condition)
		throw std::runtime_error(expression);
}
#define CHECK(value) check((value), #value)

struct Collector {
	void *sink = nullptr;
	bool active = false;
	std::string error;
};
obs_output_info registeredOutput;
std::unordered_map<obs_output_t *, Collector> collectors;
std::unordered_map<obs_encoder_t *, int> encoderRefs;
std::unordered_set<obs_encoder_t *> allocatedEncoders;
std::unordered_map<OutputSession *, std::vector<int64_t>> deliveries;
std::size_t createdCollectors = 0;
std::size_t createdVideoEncoders = 0;
std::size_t createdAudioEncoders = 0;
bool failNextStart = false;

struct Primary {
	obs_encoder_t video;
	obs_encoder_t audio;
	obs_output_t output;
	Primary()
	{
		audio.id = "fake-aac";
		audio.type = OBS_ENCODER_AUDIO;
		output.video[0] = &video;
		output.audio[0] = &audio;
	}
};

void reset_counters()
{
	CHECK(collectors.empty());
	CHECK(allocatedEncoders.empty());
	CHECK(deliveries.empty());
	createdCollectors = createdVideoEncoders = createdAudioEncoders = 0;
	failNextStart = false;
	encoderRefs.clear();
}

std::shared_ptr<HoldMediaHub> hub()
{
	std::string error;
	auto result = HoldMediaHub::create(nullptr, {}, error);
	CHECK(result != nullptr);
	return result;
}

obs_output_t *only_collector()
{
	CHECK(collectors.size() == 1);
	return collectors.begin()->first;
}

void emit_packet(obs_output_t *output, const int64_t sequence, const bool keyframe = false)
{
	const auto &collector = collectors.at(output);
	if (!collector.active || output->paused)
		return;
	encoder_packet packet{};
	packet.dts = sequence;
	packet.keyframe = keyframe;
	registeredOutput.encoded_packet(collector.sink, &packet);
}

void same_layout_shares_and_late_subscriber_gets_only_future_packets()
{
	reset_counters();
	Primary primary;
	obs_output_t secondOutput;
	secondOutput.video = primary.output.video;
	secondOutput.audio = primary.output.audio;
	HoldPipeline *firstPipelineObserver = nullptr;
	OutputSession first(&primary.output, "first", [&] {
		// Real sessions validate headers from inside receive_hold_packet.
		// This must not try to reacquire the subscriber/dispatch mutex.
		std::string compatibilityError;
		CHECK(firstPipelineObserver->compatible_with_primary(compatibilityError));
	});
	OutputSession second(&secondOutput, "second", {});
	auto media = hub();
	HoldPipeline firstPipeline(first, &primary.output, media);
	HoldPipeline secondPipeline(second, &secondOutput, media);
	firstPipelineObserver = &firstPipeline;
	std::string error;
	CHECK(firstPipeline.start(error));
	obs_output_t *collector = only_collector();
	emit_packet(collector, 1, true);
	CHECK(secondPipeline.start(error));
	CHECK(createdCollectors == 1);
	CHECK(createdVideoEncoders == 1);
	CHECK(createdAudioEncoders == 1);
	CHECK(deliveries.at(&second).empty());
	emit_packet(collector, 2);
	emit_packet(collector, 3, true);
	CHECK(deliveries.at(&first) == std::vector<int64_t>({1, 2, 3}));
	CHECK(deliveries.at(&second) == std::vector<int64_t>({2, 3}));
	CHECK(firstPipeline.compatible_with_primary(error));
	CHECK(secondPipeline.compatible_with_primary(error));
	firstPipeline.stop();
	CHECK(collectors.size() == 1);
	emit_packet(collector, 4);
	CHECK(deliveries.at(&first).size() == 3);
	CHECK(deliveries.at(&second).back() == 4);
	secondPipeline.stop();
	CHECK(collectors.empty());
	CHECK(allocatedEncoders.empty());
	CHECK(primary.output.refs == 2); // The two pipeline objects retain outputs until destruction.
	CHECK(secondOutput.refs == 2);
}

void partial_layout_or_distinct_hub_stays_independent()
{
	reset_counters();
	Primary primary;
	obs_encoder_t separateAudio;
	separateAudio.type = OBS_ENCODER_AUDIO;
	separateAudio.id = "fake-aac";
	obs_output_t secondOutput;
	secondOutput.video = primary.output.video;
	secondOutput.audio[0] = &separateAudio;
	OutputSession first(&primary.output, "first", {});
	OutputSession second(&secondOutput, "second", {});
	auto media = hub();
	HoldPipeline firstPipeline(first, &primary.output, media);
	HoldPipeline secondPipeline(second, &secondOutput, media);
	std::string error;
	CHECK(firstPipeline.start(error));
	obs_output_t *firstCollector = only_collector();
	CHECK(secondPipeline.start(error));
	CHECK(createdCollectors == 2);
	CHECK(createdVideoEncoders == 2);
	obs_output_t *secondCollector = nullptr;
	for (const auto &[output, unused] : collectors) {
		(void)unused;
		if (output != firstCollector)
			secondCollector = output;
	}
	CHECK(secondCollector != nullptr);
	CHECK(firstPipeline.set_paused(true));
	CHECK(firstCollector->paused);
	CHECK(!secondCollector->paused);
	emit_packet(secondCollector, 10);
	CHECK(deliveries.at(&second).back() == 10);
	firstPipeline.stop();
	secondPipeline.stop();
	CHECK(collectors.empty());
	CHECK(allocatedEncoders.empty());

	// Even identical encoders cannot be shared across separate hold scenes.
	HoldPipeline separateHubA(first, &primary.output, hub());
	HoldPipeline separateHubB(first, &primary.output, hub());
	CHECK(separateHubA.start(error));
	CHECK(separateHubB.start(error));
	CHECK(collectors.size() == 2);
}

void shared_pause_is_idempotent_and_survives_first_subscriber_stop()
{
	reset_counters();
	Primary primary;
	OutputSession first(&primary.output, "first", {});
	OutputSession second(&primary.output, "second", {});
	auto media = hub();
	HoldPipeline firstPipeline(first, &primary.output, media);
	HoldPipeline secondPipeline(second, &primary.output, media);
	std::string error;
	CHECK(firstPipeline.start(error));
	CHECK(secondPipeline.start(error));
	obs_output_t *collector = only_collector();
	CHECK(firstPipeline.set_paused(true));
	CHECK(secondPipeline.set_paused(true));
	emit_packet(collector, 1);
	CHECK(deliveries.at(&first).empty());
	CHECK(deliveries.at(&second).empty());
	firstPipeline.stop();
	CHECK(collector->paused);
	CHECK(secondPipeline.set_paused(false));
	CHECK(secondPipeline.set_paused(false));
	emit_packet(collector, 2, true);
	CHECK(deliveries.at(&first).empty());
	CHECK(deliveries.at(&second) == std::vector<int64_t>({2}));
}

void last_unsubscribe_waits_for_inflight_delivery()
{
	reset_counters();
	Primary primary;
	std::mutex barrierMutex;
	std::condition_variable barrier;
	bool entered = false;
	bool release = false;
	OutputSession owner(&primary.output, "blocking", [&] {
		std::unique_lock lock(barrierMutex);
		entered = true;
		barrier.notify_all();
		barrier.wait(lock, [&] { return release; });
	});
	HoldPipeline pipeline(owner, &primary.output, hub());
	std::string error;
	CHECK(pipeline.start(error));
	obs_output_t *collector = only_collector();
	auto delivery = std::async(std::launch::async, [&] { emit_packet(collector, 1, true); });
	{
		std::unique_lock lock(barrierMutex);
		barrier.wait(lock, [&] { return entered; });
	}
	std::atomic_bool stopping{false};
	auto stop = std::async(std::launch::async, [&] {
		stopping.store(true, std::memory_order_release);
		pipeline.stop();
	});
	while (!stopping.load(std::memory_order_acquire))
		std::this_thread::yield();
	const bool waited = stop.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
	{
		std::scoped_lock lock(barrierMutex);
		release = true;
	}
	barrier.notify_all();
	delivery.get();
	stop.get();
	CHECK(waited);
	CHECK(collectors.empty());
	CHECK(allocatedEncoders.empty());
	CHECK(deliveries.at(&owner) == std::vector<int64_t>({1}));
}

void failed_start_releases_clones_and_does_not_poison_cache()
{
	reset_counters();
	Primary primary;
	OutputSession owner(&primary.output, "owner", {});
	auto media = hub();
	HoldPipeline failed(owner, &primary.output, media);
	std::string error;
	failNextStart = true;
	CHECK(!failed.start(error));
	CHECK(!error.empty());
	CHECK(collectors.empty());
	CHECK(allocatedEncoders.empty());
	HoldPipeline retry(owner, &primary.output, media);
	CHECK(retry.start(error));
	CHECK(collectors.size() == 1);
}

void inactive_encoding_is_not_reused()
{
	reset_counters();
	Primary primary;
	OutputSession first(&primary.output, "first", {});
	OutputSession second(&primary.output, "second", {});
	auto media = hub();
	HoldPipeline firstPipeline(first, &primary.output, media);
	std::string error;
	CHECK(firstPipeline.start(error));
	obs_output_t *retiredCollector = only_collector();
	obs_output_end_data_capture(retiredCollector);
	HoldPipeline secondPipeline(second, &primary.output, media);
	CHECK(secondPipeline.start(error));
	CHECK(createdCollectors == 2);
	CHECK(!obs_output_active(retiredCollector));
	for (const auto &[output, unused] : collectors) {
		(void)unused;
		if (output != retiredCollector)
			emit_packet(output, 1, true);
	}
	CHECK(deliveries.at(&first).empty());
	CHECK(deliveries.at(&second) == std::vector<int64_t>({1}));
}

} // namespace

// Test boundary implementations. The collector's callbacks, sharing, refs,
// subscription synchronization, and pause calls are production code.
obs_output_t *obs_output_get_ref(obs_output_t *output)
{
	if (output)
		++output->refs;
	return output;
}
void obs_output_release(obs_output_t *output)
{
	if (!output || --output->refs != 0)
		return;
	auto collector = collectors.find(output);
	CHECK(collector != collectors.end());
	registeredOutput.destroy(collector->second.sink);
	for (auto *encoder : output->video)
		obs_encoder_release(encoder);
	for (auto *encoder : output->audio)
		obs_encoder_release(encoder);
	collectors.erase(collector);
	delete output;
}
obs_source_t *obs_source_get_ref(obs_source_t *source)
{
	return source;
}
void obs_source_release(obs_source_t *) {}
obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *encoder)
{
	if (encoder) {
		auto [entry, inserted] = encoderRefs.try_emplace(encoder, 1);
		(void)inserted;
		++entry->second;
	}
	return encoder;
}
void obs_encoder_release(obs_encoder_t *encoder)
{
	if (!encoder)
		return;
	auto entry = encoderRefs.find(encoder);
	CHECK(entry != encoderRefs.end());
	if (--entry->second == 0) {
		CHECK(allocatedEncoders.erase(encoder) == 1);
		encoderRefs.erase(entry);
		delete encoder;
	}
}
obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *output)
{
	return output->video[0];
}
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *output, std::size_t index)
{
	return output->audio[index];
}
const char *obs_encoder_get_id(const obs_encoder_t *encoder)
{
	return encoder->id.c_str();
}
const char *obs_encoder_get_codec(obs_encoder_t *encoder)
{
	return encoder->type == OBS_ENCODER_VIDEO ? "h264" : "aac";
}
obs_data_t *obs_encoder_get_settings(obs_encoder_t *encoder)
{
	return new obs_data_t(encoder->settings);
}
obs_data_t *obs_data_create()
{
	return new obs_data_t;
}
void obs_data_release(obs_data_t *settings)
{
	delete settings;
}
int64_t obs_data_get_int(const obs_data_t *settings, const char *)
{
	return settings->bitrate;
}
void obs_data_set_int(obs_data_t *settings, const char *, int64_t value)
{
	settings->bitrate = value;
}
void obs_register_output(const obs_output_info *info)
{
	registeredOutput = *info;
}
obs_output_t *obs_output_create(const char *id, const char *, obs_data_t *settings, obs_data_t *)
{
	CHECK(registeredOutput.id != nullptr);
	CHECK(std::string(id) == registeredOutput.id);
	auto *output = new obs_output_t;
	output->flags = registeredOutput.flags;
	collectors[output].sink = registeredOutput.create(settings, output);
	++createdCollectors;
	return output;
}
bool obs_output_can_begin_data_capture(obs_output_t *, uint32_t)
{
	return true;
}
bool obs_output_initialize_encoders(obs_output_t *, uint32_t)
{
	return true;
}
bool obs_output_begin_data_capture(obs_output_t *output, uint32_t)
{
	collectors.at(output).active = true;
	return true;
}
void obs_output_end_data_capture(obs_output_t *output)
{
	collectors.at(output).active = false;
}
bool obs_output_start(obs_output_t *output)
{
	if (std::exchange(failNextStart, false))
		return false;
	return registeredOutput.start(collectors.at(output).sink);
}
void obs_output_stop(obs_output_t *output)
{
	registeredOutput.stop(collectors.at(output).sink, 0);
}
bool obs_output_active(obs_output_t *output)
{
	return collectors.at(output).active;
}
bool obs_output_pause(obs_output_t *output, bool paused)
{
	if (!(output->flags & OBS_OUTPUT_CAN_PAUSE) || !obs_output_active(output))
		return false;
	output->paused = paused;
	for (auto *encoder : output->video)
		if (encoder)
			encoder->paused = paused;
	for (auto *encoder : output->audio)
		if (encoder)
			encoder->paused = paused;
	return true;
}
const char *obs_output_get_last_error(obs_output_t *output)
{
	return collectors.at(output).error.c_str();
}
void obs_output_set_last_error(obs_output_t *output, const char *error)
{
	collectors.at(output).error = error;
}
void obs_output_set_video_encoder(obs_output_t *output, obs_encoder_t *encoder)
{
	output->video[0] = obs_encoder_get_ref(encoder);
}
void obs_output_set_audio_encoder(obs_output_t *output, obs_encoder_t *encoder, std::size_t index)
{
	output->audio[index] = obs_encoder_get_ref(encoder);
}
obs_encoder_t *obs_video_encoder_create(const char *id, const char *, obs_data_t *, obs_data_t *)
{
	auto *encoder = new obs_encoder_t;
	encoder->id = id;
	allocatedEncoders.insert(encoder);
	encoderRefs[encoder] = 1;
	++createdVideoEncoders;
	return encoder;
}
obs_encoder_t *obs_audio_encoder_create(const char *id, const char *name, obs_data_t *settings, std::size_t,
					obs_data_t *hotkeys)
{
	auto *encoder = obs_video_encoder_create(id, name, settings, hotkeys);
	encoder->type = OBS_ENCODER_AUDIO;
	--createdVideoEncoders;
	++createdAudioEncoders;
	return encoder;
}
bool obs_encoder_get_extra_data(obs_encoder_t *, uint8_t **data, std::size_t *size)
{
	*data = nullptr;
	*size = 0;
	return false;
}
bool obs_encoder_scaling_enabled(obs_encoder_t *)
{
	return false;
}
uint32_t obs_encoder_get_width(obs_encoder_t *)
{
	return 1920;
}
uint32_t obs_encoder_get_height(obs_encoder_t *)
{
	return 1080;
}
obs_scale_type obs_encoder_get_scale_type(obs_encoder_t *)
{
	return OBS_SCALE_DISABLE;
}
uint32_t obs_encoder_get_frame_rate_divisor(obs_encoder_t *)
{
	return 1;
}
video_format obs_encoder_get_preferred_video_format(obs_encoder_t *)
{
	return VIDEO_FORMAT_NONE;
}
video_colorspace obs_encoder_get_preferred_color_space(obs_encoder_t *)
{
	return VIDEO_CS_DEFAULT;
}
video_range_type obs_encoder_get_preferred_range(obs_encoder_t *)
{
	return VIDEO_RANGE_DEFAULT;
}
std::size_t obs_encoder_get_mixer_index(const obs_encoder_t *)
{
	return 0;
}
void obs_encoder_set_video(obs_encoder_t *, video_t *) {}
void obs_encoder_set_audio(obs_encoder_t *, audio_t *) {}
void obs_encoder_set_scaled_size(obs_encoder_t *, uint32_t, uint32_t) {}
void obs_encoder_set_gpu_scale_type(obs_encoder_t *, obs_scale_type) {}
void obs_encoder_set_frame_rate_divisor(obs_encoder_t *, uint32_t) {}
void obs_encoder_set_preferred_video_format(obs_encoder_t *, video_format) {}
void obs_encoder_set_preferred_color_space(obs_encoder_t *, video_colorspace) {}
void obs_encoder_set_preferred_range(obs_encoder_t *, video_range_type) {}
void obs_encoder_packet_ref(encoder_packet *destination, encoder_packet *source)
{
	*destination = *source;
}
void obs_encoder_packet_release(encoder_packet *packet)
{
	*packet = {};
}
extern "C" void obs_log(int, const char *, ...) {}

namespace dynamic_delay {

OutputSession::OutputSession(obs_output_t *output, std::string label, Notify notify)
	: output_(output),
	  label_(std::move(label)),
	  notify_(std::move(notify))
{
	deliveries.emplace(this, std::vector<int64_t>{});
}
OutputSession::~OutputSession()
{
	deliveries.erase(this);
}
void OutputSession::receive_hold_packet(encoder_packet *packet)
{
	if (notify_)
		notify_();
	deliveries.at(this).push_back(packet->dts);
}
HoldAudioTap::~HoldAudioTap() = default;
HoldMediaHub::HoldMediaHub(obs_source_t *, HoldAudioConfig) {}
HoldMediaHub::~HoldMediaHub() = default;
std::shared_ptr<HoldMediaHub> HoldMediaHub::create(obs_source_t *scene, HoldAudioConfig config, std::string &)
{
	auto value = std::shared_ptr<HoldMediaHub>(new HoldMediaHub(scene, config));
	value->started_ = true;
	return value;
}
void HoldMediaHub::register_source_type() {}
std::size_t HoldMediaHub::encoder_mixer_index(const obs_encoder_t *original) const noexcept
{
	return obs_encoder_get_mixer_index(original);
}

} // namespace dynamic_delay

int main()
{
	try {
		HoldPipeline::register_output_type();
		same_layout_shares_and_late_subscriber_gets_only_future_packets();
		partial_layout_or_distinct_hub_stays_independent();
		shared_pause_is_idempotent_and_survives_first_subscriber_stop();
		last_unsubscribe_waits_for_inflight_delivery();
		failed_start_releases_clones_and_does_not_poison_cache();
		inactive_encoding_is_not_reused();
		reset_counters();
		std::cout << "hold_pipeline_tests: 6 checks passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
