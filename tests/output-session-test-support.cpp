#include "output-session-test-support.hpp"

#include "hold-pipeline.hpp"
#include "output-session.hpp"

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
struct PayloadStorage {
	std::atomic_size_t references{1};
	uint64_t id = 0;
};
std::atomic<uint64_t> clockNs{1'000'000'000ULL};
std::atomic_size_t payloads{0};
output_test::PipelineBehavior pipelineBehavior;

PayloadStorage *storage(const encoder_packet &packet)
{
	return reinterpret_cast<PayloadStorage *>(packet.data);
}
} // namespace

namespace output_test {
void set_clock(const uint64_t nanoseconds)
{
	clockNs.store(nanoseconds);
}
encoder_packet make_packet(const obs_encoder_type type, const int64_t dtsUsec, const uint64_t payloadId,
			   const bool keyframe, const std::size_t track, const int64_t compositionUsec,
			   const std::size_t declaredBytes)
{
	auto *owner = new PayloadStorage;
	owner->id = payloadId;
	++payloads;
	encoder_packet packet;
	packet.data = reinterpret_cast<uint8_t *>(owner);
	// Large declared sizes exercise production safety limits without allocating
	// GiBs. No test boundary or production session dereferences packet bytes.
	packet.size = declaredBytes;
	packet.type = type;
	packet.timebase_num = type == OBS_ENCODER_VIDEO ? 50'000 : 1;
	packet.pts = dtsUsec + compositionUsec;
	packet.dts = dtsUsec;
	packet.dts_usec = dtsUsec;
	packet.sys_dts_usec = static_cast<int64_t>(clockNs.load() / 1'000);
	packet.keyframe = keyframe;
	packet.track_idx = track;
	return packet;
}
uint64_t payload_id(const encoder_packet &packet)
{
	return storage(packet)->id;
}
std::size_t live_payloads()
{
	return payloads.load();
}
std::shared_ptr<dynamic_delay::HoldMediaHub> media_hub()
{
	// HoldPipeline is the controlled external boundary. The real session only
	// forwards this opaque shared owner and never dereferences HoldMediaHub.
	auto owner = std::make_shared<int>(1);
	return {owner, reinterpret_cast<dynamic_delay::HoldMediaHub *>(owner.get())};
}
void reconnect(obs_output_t &output)
{
	output.reconnecting = true;
	if (output.signals.reconnect)
		output.signals.reconnect(output.signals.reconnectParam, nullptr);
}
void activate_output(obs_output_t &output)
{
	output.reconnecting = false;
	if (output.signals.activate)
		output.signals.activate(output.signals.activateParam, nullptr);
}
void pause_output(obs_output_t &output, const bool paused)
{
	output.paused = paused;
	if (paused && output.signals.pause)
		output.signals.pause(output.signals.pauseParam, nullptr);
	if (!paused && output.signals.unpause)
		output.signals.unpause(output.signals.unpauseParam, nullptr);
}
void emit(obs_output_t &output, encoder_packet &packet, encoder_packet_time *timing)
{
	if (!output.callback)
		throw std::runtime_error("OutputSession did not attach its production callback");
	packet.encoder = packet.type == OBS_ENCODER_VIDEO ? output.video[packet.track_idx]
							  : output.audio[packet.track_idx];
	output.callback(&output, &packet, timing, output.callbackParam);
}
PipelineBehavior &pipeline_behavior()
{
	return pipelineBehavior;
}
void reset_pipeline_behavior()
{
	pipelineBehavior = {};
}
} // namespace output_test

uint64_t os_gettime_ns()
{
	return clockNs.load();
}
extern "C" void obs_log(int, const char *, ...) {}
obs_output_t *obs_output_get_ref(obs_output_t *output)
{
	if (output)
		++output->refs;
	return output;
}
void obs_output_release(obs_output_t *output)
{
	if (output)
		--output->refs;
}
uint32_t obs_output_get_flags(const obs_output_t *output)
{
	return output->flags;
}
const char *obs_output_get_id(const obs_output_t *output)
{
	return output->id.c_str();
}
obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *output)
{
	return output->video[0];
}
obs_encoder_t *obs_output_get_video_encoder2(const obs_output_t *output, const std::size_t index)
{
	return output->video.at(index);
}
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *output, const std::size_t index)
{
	return output->audio.at(index);
}
uint32_t obs_output_get_active_delay(const obs_output_t *output)
{
	return output->nativeDelay;
}
bool obs_output_paused(const obs_output_t *output)
{
	return output->paused;
}
bool obs_encoder_paused(const obs_encoder_t *encoder)
{
	return encoder->paused;
}
bool obs_output_reconnecting(const obs_output_t *output)
{
	return output->reconnecting;
}
uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *encoder)
{
	return encoder->sampleRate;
}
std::size_t obs_encoder_get_frame_size(const obs_encoder_t *encoder)
{
	return encoder->frameSize;
}
void obs_output_add_packet_callback(obs_output_t *output, const packet_callback_t callback, void *param)
{
	output->callback = callback;
	output->callbackParam = param;
}
void obs_output_remove_packet_callback(obs_output_t *output, const packet_callback_t callback, void *param)
{
	if (output->callback == callback && output->callbackParam == param) {
		output->callback = nullptr;
		output->callbackParam = nullptr;
	}
}
signal_handler_t *obs_output_get_signal_handler(obs_output_t *output)
{
	return &output->signals;
}
void signal_handler_connect(signal_handler_t *handler, const char *name, const signal_callback_t callback, void *param)
{
	if (std::strcmp(name, "reconnect") == 0) {
		handler->reconnect = callback;
		handler->reconnectParam = param;
	} else if (std::strcmp(name, "activate") == 0) {
		handler->activate = callback;
		handler->activateParam = param;
	} else if (std::strcmp(name, "pause") == 0) {
		handler->pause = callback;
		handler->pauseParam = param;
	} else if (std::strcmp(name, "unpause") == 0) {
		handler->unpause = callback;
		handler->unpauseParam = param;
	}
}
void signal_handler_disconnect(signal_handler_t *handler, const char *name, const signal_callback_t callback,
			       void *param)
{
	if (std::strcmp(name, "reconnect") == 0 && handler->reconnect == callback && handler->reconnectParam == param) {
		handler->reconnect = nullptr;
		handler->reconnectParam = nullptr;
	} else if (std::strcmp(name, "activate") == 0 && handler->activate == callback &&
		   handler->activateParam == param) {
		handler->activate = nullptr;
		handler->activateParam = nullptr;
	} else if (std::strcmp(name, "pause") == 0 && handler->pause == callback && handler->pauseParam == param) {
		handler->pause = nullptr;
		handler->pauseParam = nullptr;
	} else if (std::strcmp(name, "unpause") == 0 && handler->unpause == callback &&
		   handler->unpauseParam == param) {
		handler->unpause = nullptr;
		handler->unpauseParam = nullptr;
	}
}
const char *obs_encoder_get_id(const obs_encoder_t *encoder)
{
	return encoder->id.c_str();
}
obs_data_t *obs_encoder_get_settings(obs_encoder_t *encoder)
{
	return &encoder->settings;
}
const char *obs_data_get_string(const obs_data_t *settings, const char *)
{
	return settings->rateControl.c_str();
}
int64_t obs_data_get_int(const obs_data_t *settings, const char *)
{
	return settings->bitrate;
}
bool obs_data_get_bool(const obs_data_t *settings, const char *)
{
	return settings->lossless;
}
void obs_data_release(obs_data_t *) {}
void obs_encoder_packet_ref(encoder_packet *destination, encoder_packet *source)
{
	*destination = *source;
	if (destination->data)
		++storage(*destination)->references;
}
void obs_encoder_packet_release(encoder_packet *packet)
{
	if (packet->data && --storage(*packet)->references == 0) {
		delete storage(*packet);
		--payloads;
	}
	*packet = {};
}

namespace dynamic_delay {
HoldPipeline::HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, std::shared_ptr<HoldMediaHub> mediaHub)
	: owner_(owner),
	  primaryOutput_(primaryOutput),
	  mediaHub_(std::move(mediaHub))
{
	++pipelineBehavior.constructed;
}
HoldPipeline::~HoldPipeline()
{
	if (pipelineBehavior.onDestroy)
		pipelineBehavior.onDestroy(owner_);
	++pipelineBehavior.destroyed;
}
bool HoldPipeline::start(std::string &error)
{
	if (!pipelineBehavior.startSucceeds)
		error = "Injected encoder startup failure";
	return pipelineBehavior.startSucceeds;
}
void HoldPipeline::stop()
{
	if (pipelineBehavior.onStop)
		pipelineBehavior.onStop(owner_);
	++pipelineBehavior.stopped;
}
bool HoldPipeline::set_paused(const bool paused)
{
	pipelineBehavior.paused = paused;
	++pipelineBehavior.pauseCalls;
	return true;
}
bool HoldPipeline::compatible_with_primary(std::string &error) const
{
	if (!pipelineBehavior.compatible)
		error = "Injected bitstream mismatch";
	return pipelineBehavior.compatible;
}
} // namespace dynamic_delay
