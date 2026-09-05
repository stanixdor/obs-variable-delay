#include "output-session.hpp"

#include "hold-pipeline.hpp"
#include "plugin-support.h"

#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace dynamic_delay {
namespace {

constexpr uint64_t NsPerSecond = 1'000'000'000ULL;
constexpr uint64_t ThroughputWindowNs = 2 * NsPerSecond;
constexpr uint64_t HoldPrerollNs = NsPerSecond / 2;
constexpr std::size_t MaxPrimaryBufferBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t MaxHoldBufferBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t MaxHoldLanePackets = 4096;

std::size_t video_encoder_count(const obs_output_t *output) noexcept
{
	std::size_t count = 0;
	for (std::size_t index = 0; index < MAX_OUTPUT_VIDEO_ENCODERS; ++index)
		count += obs_output_get_video_encoder2(output, index) != nullptr ? 1U : 0U;
	return count;
}

std::size_t audio_encoder_count(const obs_output_t *output) noexcept
{
	std::size_t count = 0;
	for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index)
		count += obs_output_get_audio_encoder(output, index) != nullptr ? 1U : 0U;
	return count;
}

int64_t ticks_to_usec(const int64_t ticks, const int32_t numerator, const int32_t denominator) noexcept
{
	(void)numerator;
	if (denominator <= 0)
		return 0;
	// OBS stores pts/dts in denominator ticks; timebase_num is the nominal
	// per-frame increment (for example 1001 at 60000/1001 fps), not an extra
	// multiplier.  This matches libobs packet_dts_usec().
	const long double value =
		static_cast<long double>(ticks) * 1'000'000.0L / static_cast<long double>(denominator);
	return static_cast<int64_t>(std::clamp(value, static_cast<long double>(std::numeric_limits<int64_t>::min()),
					       static_cast<long double>(std::numeric_limits<int64_t>::max())));
}

int64_t usec_to_ticks(const int64_t usec, const int32_t numerator, const int32_t denominator) noexcept
{
	(void)numerator;
	if (denominator <= 0)
		return 0;
	const long double value = static_cast<long double>(usec) * static_cast<long double>(denominator) / 1'000'000.0L;
	return static_cast<int64_t>(std::clamp(std::round(value),
					       static_cast<long double>(std::numeric_limits<int64_t>::min()),
					       static_cast<long double>(std::numeric_limits<int64_t>::max())));
}

int64_t saturating_add(const int64_t value, const int64_t increment) noexcept
{
	if (increment > 0 && value > std::numeric_limits<int64_t>::max() - increment)
		return std::numeric_limits<int64_t>::max();
	if (increment < 0 && value < std::numeric_limits<int64_t>::min() - increment)
		return std::numeric_limits<int64_t>::min();
	return value + increment;
}

} // namespace

OutputSession::OutputSession(obs_output_t *output, std::string label, Notify notify)
	: output_(obs_output_get_ref(output)),
	  label_(std::move(label)),
	  notify_(std::move(notify))
{
}

OutputSession::~OutputSession()
{
	if (lifecycleSignalsAttached_) {
		signal_handler_t *signals = obs_output_get_signal_handler(output_);
		signal_handler_disconnect(signals, "activate", &OutputSession::activate_signal, this);
		signal_handler_disconnect(signals, "pause", &OutputSession::pause_signal, this);
		signal_handler_disconnect(signals, "unpause", &OutputSession::pause_signal, this);
	}
	if (reconnectSignalAttached_) {
		signal_handler_disconnect(obs_output_get_signal_handler(output_), "reconnect",
					  &OutputSession::reconnect_signal, this);
		reconnectSignalAttached_ = false;
	}
	if (callbackAttached_) {
		obs_output_remove_packet_callback(output_, &OutputSession::packet_callback, this);
		callbackAttached_ = false;
	}
	holdPipeline_.reset();
	clear_primary_buffer();
	clear_hold_buffer();
	obs_output_release(output_);
}

bool OutputSession::supported(std::string &error) const
{
	const uint32_t flags = obs_output_get_flags(output_);
	if ((flags & OBS_OUTPUT_ENCODED) == 0 || (flags & OBS_OUTPUT_AV) != OBS_OUTPUT_AV) {
		error = "The output must be an encoded audio/video output.";
		return false;
	}
	const std::size_t videoEncoders = video_encoder_count(output_);
	if (videoEncoders == 0) {
		error = "The output has no video encoder.";
		return false;
	}
	// Hybrid MP4/MOV advertises OBS_OUTPUT_MULTI_TRACK_VIDEO as a capability
	// even for an ordinary single-video recording.  Reject the active layout,
	// not the output type's capability flag, so those default OBS 32 formats
	// remain usable while real multi-video/Enhanced Broadcasting stays blocked.
	if (videoEncoders != 1 || !obs_output_get_video_encoder2(output_, 0)) {
		error = "Multiple video tracks (including Enhanced Broadcasting) are not supported safely.";
		return false;
	}
	bool hasAudio = false;
	for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index)
		hasAudio = hasAudio || obs_output_get_audio_encoder(output_, index) != nullptr;
	if (!hasAudio) {
		error = "The packet callback requires at least one audio encoder.";
		return false;
	}
	if (obs_output_get_active_delay(output_) != 0) {
		error = "Disable OBS's native Stream Delay before using Dynamic Delay.";
		return false;
	}
	return true;
}

bool OutputSession::attach(std::string &error)
{
	const uint32_t flags = obs_output_get_flags(output_);
	const char *outputId = obs_output_get_id(output_);
	obs_encoder_t *videoEncoder = obs_output_get_video_encoder(output_);
	const char *videoEncoderId = videoEncoder ? obs_encoder_get_id(videoEncoder) : nullptr;
	obs_log(LOG_INFO,
		"%s: inspecting output id=%s flags=0x%08x video_encoders=%zu audio_encoders=%zu "
		"video_encoder=%s",
		label_.c_str(), outputId ? outputId : "<unknown>", flags, video_encoder_count(output_),
		audio_encoder_count(output_), videoEncoderId ? videoEncoderId : "<none>");
	if (!supported(error))
		return false;
	if (!callbackAttached_) {
		obs_output_add_packet_callback(output_, &OutputSession::packet_callback, this);
		callbackAttached_ = true;
	}
	if (!reconnectSignalAttached_) {
		signal_handler_connect(obs_output_get_signal_handler(output_), "reconnect",
				       &OutputSession::reconnect_signal, this);
		reconnectSignalAttached_ = true;
	}
	if (!lifecycleSignalsAttached_) {
		signal_handler_t *signals = obs_output_get_signal_handler(output_);
		signal_handler_connect(signals, "activate", &OutputSession::activate_signal, this);
		signal_handler_connect(signals, "pause", &OutputSession::pause_signal, this);
		signal_handler_connect(signals, "unpause", &OutputSession::pause_signal, this);
		lifecycleSignalsAttached_ = true;
	}
	return true;
}

bool OutputSession::request_delay(const uint32_t seconds, std::shared_ptr<HoldMediaHub> mediaHub, std::string &error)
{
	if (!mediaHub) {
		error = "The shared hold media graph is not available.";
		return false;
	}
	if (seconds == 0 || seconds > 300) {
		error = "Delay must be between 1 and 300 seconds.";
		return false;
	}
	if (!supported(error))
		return false;

	// A user can cancel and re-enable before the 100 ms maintenance tick.
	// Stop the old collector before publishing a new generation, and never
	// join encoder callbacks while holding the mutex those callbacks need.
	std::unique_ptr<HoldPipeline> retiredPipeline;
	{
		std::scoped_lock lock(mutex_);
		const DelayState current = state_.load(std::memory_order_relaxed);
		if (current != DelayState::Bypass && current != DelayState::Error) {
			error = "This output is already changing delay state.";
			return false;
		}
		retiredPipeline = std::move(holdPipeline_);
	}
	retiredPipeline.reset();

	{
		std::scoped_lock lock(mutex_);
		const DelayState current = state_.load(std::memory_order_relaxed);
		if (current != DelayState::Bypass && current != DelayState::Error) {
			error = "This output is already changing delay state.";
			return false;
		}
		targetSeconds_ = seconds;
		fillStartNs_ = 0;
		fillStartDtsUsec_ = 0;
		latestPrimaryDtsUsec_ = 0;
		holdReady_ = false;
		returnFromFilling_ = false;
		drainPrimaryOnReturn_ = false;
		cleanupHoldRequested_ = false;
		errorReturnPending_ = false;
		pendingErrorDetail_.clear();
		effectiveSeconds_ = 0.0;
		activeAudioMask_ = 0;
		holdGopStartDtsUsec_ = 0;
		holdPauseStateKnown_ = false;
		primaryFormats_ = {};
		holdFormats_ = {};
		havePrimaryDts_ = {};
		haveEmittedSourceDts_ = {};
		primaryStepUsec_ = {};
		realignmentPending_ = false;
		for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
			if (obs_output_get_audio_encoder(output_, index))
				activeAudioMask_ |= 1U << index;
		}
		detail_ = "Preparing the hold encoder";
		clear_primary_buffer();
		clear_hold_buffer();
		state_.store(DelayState::Preparing, std::memory_order_release);
	}

	std::unique_ptr<HoldPipeline> pipeline = std::make_unique<HoldPipeline>(*this, output_, std::move(mediaHub));
	{
		std::scoped_lock lock(mutex_);
		holdPipeline_ = std::move(pipeline);
	}
	if (!holdPipeline_->start(error)) {
		std::scoped_lock lock(mutex_);
		set_error_locked(error);
		cleanupHoldRequested_ = true;
		return false;
	}
	sync_pause_state();

	if (notify_)
		notify_();
	obs_log(LOG_INFO, "%s: preparing %u second delay", label_.c_str(), seconds);
	return true;
}

void OutputSession::request_bypass()
{
	std::scoped_lock lock(mutex_);
	const DelayState current = state_.load(std::memory_order_relaxed);
	if (current == DelayState::Bypass)
		return;
	if (current == DelayState::ReturningLive)
		return;
	if (current == DelayState::Preparing || current == DelayState::Error) {
		state_.store(DelayState::Bypass, std::memory_order_release);
		detail_ = "Live";
		errorReturnPending_ = false;
		pendingErrorDetail_.clear();
		clear_primary_buffer();
		clear_hold_buffer();
		cleanupHoldRequested_ = true;
	} else {
		returnFromFilling_ = current == DelayState::Filling;
		drainPrimaryOnReturn_ = false;
		state_.store(DelayState::ReturningLive, std::memory_order_release);
		detail_ = "Waiting for the next live keyframe";
	}
	obs_log(LOG_INFO, "%s: delay removal requested (%s)", label_.c_str(),
		state_.load(std::memory_order_relaxed) == DelayState::Bypass ? "live" : "awaiting keyframe");
	if (notify_)
		notify_();
}

void OutputSession::maintenance()
{
	sync_pause_state();
	std::unique_ptr<HoldPipeline> pipeline;
	{
		std::scoped_lock lock(mutex_);
		if (!cleanupHoldRequested_)
			return;
		cleanupHoldRequested_ = false;
		pipeline = std::move(holdPipeline_);
		holdPauseStateKnown_ = false;
	}
	if (pipeline)
		pipeline->stop();
	std::array<PacketQueue, LaneCount> retiredPrimary;
	std::array<PacketQueue, LaneCount> retiredHold;
	std::array<ObsPacket, LaneCount> retiredPrimaryFallback;
	std::array<ObsPacket, LaneCount> retiredHoldFallback;
	{
		std::scoped_lock lock(mutex_);
		const DelayState current = state_.load(std::memory_order_relaxed);
		const bool retirePrimary = current == DelayState::Bypass || current == DelayState::Error;
		const bool retireHold = retirePrimary || current == DelayState::Delayed;
		for (std::size_t lane = 0; lane < LaneCount; ++lane) {
			if (retirePrimary) {
				primaryQueues_[lane].swap(retiredPrimary[lane]);
				retiredPrimaryFallback[lane] = std::move(primaryFallbackPackets_[lane]);
			}
			if (retireHold) {
				holdQueues_[lane].swap(retiredHold[lane]);
				retiredHoldFallback[lane] = std::move(holdFallbackPackets_[lane]);
			}
		}
		if (retirePrimary)
			bufferedBytes_ = 0;
		if (retireHold) {
			holdBufferedBytes_ = 0;
			holdGopStartDtsUsec_ = 0;
		}
	}
}

std::size_t OutputSession::lane_for(const encoder_packet &packet) noexcept
{
	if (packet.type == OBS_ENCODER_VIDEO)
		return 0;
	return std::min<std::size_t>(packet.track_idx + 1, LaneCount - 1);
}

void OutputSession::packet_callback(obs_output_t *, encoder_packet *packet, encoder_packet_time *packetTime,
				    void *param)
{
	if (packet && param)
		static_cast<OutputSession *>(param)->process_primary_packet(packet, packetTime);
}

void OutputSession::reconnect_signal(void *param, calldata_t *)
{
	auto *self = static_cast<OutputSession *>(param);
	if (!self)
		return;
	self->reconnectResetPending_.store(true, std::memory_order_release);
	self->request_bypass();
}

void OutputSession::activate_signal(void *param, calldata_t *)
{
	auto *self = static_cast<OutputSession *>(param);
	if (!self || !self->reconnectResetPending_.exchange(false, std::memory_order_acq_rel))
		return;
	{
		// libobs emits activate before making packet capture active. Reset
		// here, before even AAC priming can reach the newly connected muxer.
		std::scoped_lock lock(self->mutex_);
		self->state_.store(DelayState::Bypass, std::memory_order_release);
		self->reset_timeline_locked();
		self->cleanupHoldRequested_ = true;
		self->errorReturnPending_ = false;
		self->pendingErrorDetail_.clear();
		self->detail_ = "Live after reconnection";
	}
	self->reconnectRearmRequested_.store(true, std::memory_order_release);
	if (self->notify_)
		self->notify_();
}

void OutputSession::pause_signal(void *param, calldata_t *)
{
	auto *self = static_cast<OutputSession *>(param);
	if (self)
		self->pauseSyncRequested_.store(true, std::memory_order_release);
}

void OutputSession::sync_pause_state()
{
	obs_encoder_t *video = obs_output_get_video_encoder(output_);
	const bool paused = obs_output_paused(output_) || (video && obs_encoder_paused(video));
	HoldPipeline *pipeline = nullptr;
	{
		std::scoped_lock lock(mutex_);
		const bool requested = pauseSyncRequested_.exchange(false, std::memory_order_relaxed);
		paused_ = paused;
		if (requested || !holdPauseStateKnown_ || paused != holdPaused_)
			pipeline = holdPipeline_.get();
	}
	// Session control and maintenance run on the frontend thread. Signals
	// only request work; pause may acquire libobs encoder locks here.
	if (pipeline && pipeline->set_paused(paused)) {
		std::scoped_lock lock(mutex_);
		holdPaused_ = paused;
		holdPauseStateKnown_ = true;
	}
}

void OutputSession::reset_timeline_locked()
{
	timelineOffsetUsec_.store(0, std::memory_order_relaxed);
	maxEmittedVideoPtsUsec_ = 0;
	haveEmittedVideoPts_ = false;
	latestPrimaryDtsUsec_ = 0;
	fillStartDtsUsec_ = 0;
	fillStartNs_ = 0;
	effectiveSeconds_ = 0.0;
	emittedVideoTimestampNs_.store(0, std::memory_order_relaxed);
	havePrimaryDts_ = {};
	haveEmittedSourceDts_ = {};
	primaryStepUsec_ = {};
	realignmentPending_ = false;
	primaryFormats_ = {};
	holdFormats_ = {};
}

void OutputSession::note_throughput(const std::size_t bytes, const uint64_t nowNs)
{
	observedBytes_.fetch_add(bytes, std::memory_order_relaxed);
	uint64_t windowStart = observedWindowStartNs_.load(std::memory_order_relaxed);
	if (windowStart == 0) {
		observedWindowStartNs_.compare_exchange_strong(windowStart, nowNs, std::memory_order_relaxed);
		return;
	}
	if (nowNs <= windowStart || nowNs - windowStart < ThroughputWindowNs)
		return;
	if (!observedWindowStartNs_.compare_exchange_strong(windowStart, nowNs, std::memory_order_relaxed))
		return;

	const uint64_t elapsed = nowNs - windowStart;
	const uint64_t observedBytes = observedBytes_.exchange(0, std::memory_order_relaxed);
	measuredBytesPerSecond_.store(static_cast<double>(observedBytes) * static_cast<double>(NsPerSecond) /
					      static_cast<double>(elapsed),
				      std::memory_order_relaxed);
	if (notify_)
		notify_();
}

bool OutputSession::buffer_primary(encoder_packet *packet, const uint64_t nowNs, const int64_t rawDtsUsec)
{
	const std::size_t lane = lane_for(*packet);
	if (packet->size > MaxPrimaryBufferBytes - std::min(bufferedBytes_, MaxPrimaryBufferBytes))
		return false;
	primaryQueues_[lane].emplace_back(packet, nowNs);
	// Retain the original media timestamp for age/alignment decisions. The
	// payload's PTS-DTS composition offset is unchanged by timeline bridges.
	primaryQueues_[lane].back().get()->dts_usec = rawDtsUsec;
	bufferedBytes_ += packet->size;
	return true;
}

void OutputSession::note_primary_cadence_locked(const encoder_packet &packet, const int64_t rawDtsUsec)
{
	const std::size_t lane = lane_for(packet);
	int64_t &step = primaryStepUsec_[lane];
	if (step == 0) {
		if (packet.type == OBS_ENCODER_VIDEO) {
			// libobs already multiplies packet.timebase_num by the encoder's
			// frame-rate divisor, in both CPU and GPU encoding paths.
			step = std::max<int64_t>(1, ticks_to_usec(packet.timebase_num, 1, packet.timebase_den));
		} else if (packet.encoder) {
			const uint32_t rate = obs_encoder_get_sample_rate(packet.encoder);
			const size_t frames = obs_encoder_get_frame_size(packet.encoder);
			if (rate && frames)
				step = static_cast<int64_t>(frames * 1'000'000ULL / rate);
		}
	}
	if (havePrimaryDts_[lane] && rawDtsUsec > lastPrimaryDts_[lane]) {
		const int64_t observedStep = rawDtsUsec - lastPrimaryDts_[lane];
		if (step == 0)
			step = observedStep;
		// Arrival jitter does not count: only a hole in the encoder's media
		// DTS does. AAC rounding and B-frame PTS reordering are harmless.
		if (step > 0 && observedStep > step + std::max<int64_t>(2'000, step / 2))
			realignmentPending_ = true;
	}
	lastPrimaryDts_[lane] = rawDtsUsec;
	havePrimaryDts_[lane] = true;
	if (packet.type == OBS_ENCODER_VIDEO && step > 0)
		videoFrameDurationUsec_ = step;
}

void OutputSession::realign_primary_locked(encoder_packet *carrier, const int64_t rawDtsUsec)
{
	if (carrier->type != OBS_ENCODER_VIDEO || primaryQueues_[0].empty())
		return;
	const int64_t videoSource = primaryQueues_[0].front().get()->dts_usec;
	if (haveEmittedSourceDts_[0] && primaryStepUsec_[0] > 0 &&
	    videoSource - lastEmittedSourceDts_[0] >
		    primaryStepUsec_[0] + std::max<int64_t>(2'000, primaryStepUsec_[0] / 2))
		realignmentPending_ = true;
	if (!realignmentPending_)
		return;

	const int64_t videoAge = rawDtsUsec - videoSource;
	int64_t lowerBound = videoSource;
	bool skewed = false;
	for (std::size_t lane = 1; lane < LaneCount; ++lane) {
		if ((activeAudioMask_ & (1U << (lane - 1))) == 0)
			continue;
		if (primaryQueues_[lane].empty() || !havePrimaryDts_[lane])
			return;
		const int64_t audioSource = primaryQueues_[lane].front().get()->dts_usec;
		lowerBound = std::max(lowerBound, audioSource);
		// The audio head is the NEXT packet; the last observed carrier has
		// already consumed its predecessor. Include one nominal audio step.
		const int64_t audioAge = lastPrimaryDts_[lane] + primaryStepUsec_[lane] - audioSource;
		const int64_t tolerance =
			std::max<int64_t>(4'000, 2 * std::max(primaryStepUsec_[0], primaryStepUsec_[lane]));
		skewed = skewed || std::abs(videoAge - audioAge) > tolerance;
	}
	if (!skewed) {
		realignmentPending_ = false;
		return;
	}

	// Move all tracks forward to one independently decodable GOP. Never
	// discard a reference frame and then emit its dependent P/B frames.
	// Prefer a keyframe at/before the configured age, without replaying any
	// audio already emitted. A lost carrier may make exact seconds
	// impossible; effectiveSeconds reports the resulting age honestly.
	const int64_t desired = rawDtsUsec - static_cast<int64_t>(targetSeconds_) * 1'000'000LL;
	std::size_t chosen = primaryQueues_[0].size();
	std::array<std::size_t, LaneCount> audioPositions{};
	for (std::size_t index = 0; index < primaryQueues_[0].size(); ++index) {
		const ObsPacket &packet = primaryQueues_[0][index];
		if (!packet.keyframe() || packet.get()->dts_usec < lowerBound)
			continue;
		const int64_t candidateDts = packet.get()->dts_usec;
		bool audioAligned = true;
		bool beyondAvailableAudio = false;
		for (std::size_t lane = 1; lane < LaneCount; ++lane) {
			if ((activeAudioMask_ & (1U << (lane - 1))) == 0)
				continue;
			const PacketQueue &audio = primaryQueues_[lane];
			auto &position = audioPositions[lane];
			while (position < audio.size() && audio[position].get()->dts_usec < candidateDts)
				++position;
			if (position == audio.size()) {
				audioAligned = false;
				beyondAvailableAudio = true;
				break;
			}
			if (audio[position].get()->dts_usec - candidateDts > primaryStepUsec_[lane] + 2'000) {
				audioAligned = false;
				break;
			}
		}
		// An audio hole can straddle this GOP even when its queue has newer
		// packets. Seek a later GOP that actually has matching audio on ALL
		// tracks, rather than silently jumping one track past the video.
		if (beyondAvailableAudio)
			break;
		if (!audioAligned)
			continue;
		if (chosen == primaryQueues_[0].size() || packet.get()->dts_usec <= desired)
			chosen = index;
		if (packet.get()->dts_usec >= desired)
			break;
	}
	if (chosen == primaryQueues_[0].size())
		return;
	const int64_t origin = primaryQueues_[0][chosen].get()->dts_usec;
	// Wait for each audio lane to actually contain the new origin before
	// committing, so a cut cannot expose current/live audio accidentally.
	for (std::size_t lane = 1; lane < LaneCount; ++lane) {
		if ((activeAudioMask_ & (1U << (lane - 1))) != 0 &&
		    primaryQueues_[lane].back().get()->dts_usec < origin)
			return;
	}
	for (std::size_t lane = 0; lane < LaneCount; ++lane) {
		auto &queue = primaryQueues_[lane];
		while (!queue.empty() && queue.front().get()->dts_usec < origin) {
			bufferedBytes_ -= queue.front().size();
			queue.pop_front();
		}
		primaryFallbackPackets_[lane].reset();
	}
	haveEmittedSourceDts_ = {};
	realignmentPending_ = false;
	begin_timeline_epoch_locked(*carrier, &primaryQueues_[0].front());
	obs_log(LOG_WARNING, "%s: recovered audio/video alignment at a complete buffered GOP (%.3f s delay)",
		label_.c_str(), static_cast<double>(rawDtsUsec - origin) / 1'000'000.0);
}

void OutputSession::note_lane_format(std::array<LaneFormat, LaneCount> &formats, const encoder_packet &packet)
{
	LaneFormat &format = formats[lane_for(packet)];
	format.seen = true;
	format.type = packet.type;
	format.track = packet.track_idx;
	format.timebaseNum = packet.timebase_num;
	format.timebaseDen = packet.timebase_den;
}

bool OutputSession::packets_compatible(const encoder_packet &carrier, const ObsPacket &replacement) const
{
	const encoder_packet *payload = replacement.get();
	return payload && payload->type == carrier.type && payload->track_idx == carrier.track_idx &&
	       payload->timebase_num == carrier.timebase_num && payload->timebase_den == carrier.timebase_den;
}

void OutputSession::apply_timeline_offset_locked(encoder_packet &packet, const int64_t offsetUsec)
{
	if (offsetUsec <= 0)
		return;
	const int64_t offsetTicks = usec_to_ticks(offsetUsec, packet.timebase_num, packet.timebase_den);
	packet.dts = saturating_add(packet.dts, offsetTicks);
	packet.pts = saturating_add(packet.pts, offsetTicks);
	packet.dts_usec = saturating_add(packet.dts_usec, offsetUsec);
	// sys_dts_usec remains the wall-clock capture timestamp used by output
	// pacing.  Only the media timeline receives the splice gap.
}

void OutputSession::begin_timeline_epoch_locked(encoder_packet &carrier, const ObsPacket *replacement)
{
	if (carrier.type != OBS_ENCODER_VIDEO || !haveEmittedVideoPts_)
		return;
	const encoder_packet *payload = replacement && replacement->valid() ? replacement->get() : &carrier;
	const int64_t compositionUsec =
		ticks_to_usec(payload->pts - payload->dts, carrier.timebase_num, carrier.timebase_den);
	const int64_t candidatePtsUsec = saturating_add(carrier.dts_usec, compositionUsec);
	const int64_t nextSafePtsUsec =
		saturating_add(maxEmittedVideoPtsUsec_, std::max<int64_t>(1, videoFrameDurationUsec_));
	if (candidatePtsUsec >= nextSafePtsUsec)
		return;

	const int64_t additionalUsec = nextSafePtsUsec - candidatePtsUsec;
	apply_timeline_offset_locked(carrier, additionalUsec);
	timelineOffsetUsec_.store(saturating_add(timelineOffsetUsec_.load(std::memory_order_relaxed), additionalUsec),
				  std::memory_order_relaxed);
	obs_log(LOG_DEBUG, "%s: inserted %.3f ms timestamp bridge at encoded GOP splice", label_.c_str(),
		static_cast<double>(additionalUsec) / 1000.0);
}

void OutputSession::note_emitted_video_locked(const encoder_packet &packet)
{
	if (packet.type != OBS_ENCODER_VIDEO)
		return;
	const int64_t compositionUsec =
		ticks_to_usec(packet.pts - packet.dts, packet.timebase_num, packet.timebase_den);
	const int64_t ptsUsec = saturating_add(packet.dts_usec, compositionUsec);
	if (!haveEmittedVideoPts_ || ptsUsec > maxEmittedVideoPtsUsec_)
		maxEmittedVideoPtsUsec_ = ptsUsec;
	haveEmittedVideoPts_ = true;
}

bool OutputSession::hold_formats_compatible_locked(bool &complete) const
{
	complete = true;
	for (std::size_t lane = 0; lane < LaneCount; ++lane) {
		const bool required = lane == 0 || (activeAudioMask_ & (1U << (lane - 1))) != 0;
		if (!required)
			continue;
		const LaneFormat &primary = primaryFormats_[lane];
		const LaneFormat &hold = holdFormats_[lane];
		if (!primary.seen || !hold.seen) {
			complete = false;
			continue;
		}
		if (primary.type != hold.type || primary.track != hold.track ||
		    primary.timebaseNum != hold.timebaseNum || primary.timebaseDen != hold.timebaseDen)
			return false;
	}
	return true;
}

bool OutputSession::hold_preroll_ready_locked() const
{
	if (!holdReady_ || holdQueues_[0].empty() || !holdQueues_[0].front().keyframe())
		return false;
	const int64_t readyDts = holdGopStartDtsUsec_ + static_cast<int64_t>(HoldPrerollNs / 1000ULL);
	if (holdQueues_[0].back().get()->dts_usec < readyDts)
		return false;
	for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
		if ((activeAudioMask_ & (1U << index)) != 0) {
			const PacketQueue &queue = holdQueues_[index + 1];
			if (queue.empty() || queue.back().get()->dts_usec < readyDts)
				return false;
		}
	}
	bool complete = false;
	return hold_formats_compatible_locked(complete) && complete;
}

bool OutputSession::replace_from(std::array<PacketQueue, LaneCount> &queues,
				 std::array<ObsPacket, LaneCount> *fallbackPackets, encoder_packet *carrier,
				 const bool holdQueue, bool *usedFallback)
{
	if (usedFallback)
		*usedFallback = false;
	const std::size_t lane = lane_for(*carrier);
	ObsPacket *chosen = nullptr;
	bool fromQueue = false;
	if (!queues[lane].empty() && queues[lane].front().valid() &&
	    packets_compatible(*carrier, queues[lane].front())) {
		chosen = &queues[lane].front();
		fromQueue = true;
	} else if (fallbackPackets && (*fallbackPackets)[lane].valid() &&
		   packets_compatible(*carrier, (*fallbackPackets)[lane])) {
		chosen = &(*fallbackPackets)[lane];
		if (usedFallback)
			*usedFallback = true;
	}
	if (!chosen || !chosen->valid())
		return false;

	if (!holdQueue && fromQueue && fallbackPackets &&
	    (chosen->get()->type == OBS_ENCODER_AUDIO || chosen->keyframe()))
		(*fallbackPackets)[lane] = ObsPacket(chosen->get(), chosen->captured_at_ns());

	const std::size_t bytes = chosen->size();
	if (!holdQueue) {
		const int64_t sourceDts = chosen->get()->dts_usec;
		if (haveEmittedSourceDts_[lane] && primaryStepUsec_[lane] > 0 &&
		    sourceDts - lastEmittedSourceDts_[lane] >
			    primaryStepUsec_[lane] + std::max<int64_t>(2'000, primaryStepUsec_[lane] / 2))
			realignmentPending_ = true;
		lastEmittedSourceDts_[lane] = sourceDts;
		haveEmittedSourceDts_[lane] = true;
		if (carrier->type == OBS_ENCODER_VIDEO) {
			const uint64_t captured = chosen->captured_at_ns();
			effectiveSeconds_ = currentVideoCaptureNs_ >= captured
						    ? static_cast<double>(currentVideoCaptureNs_ - captured) /
							      static_cast<double>(NsPerSecond)
						    : 0.0;
			emittedVideoTimestampNs_.store(captured, std::memory_order_relaxed);
		}
	} else if (carrier->type == OBS_ENCODER_VIDEO) {
		emittedVideoTimestampNs_.store(0, std::memory_order_relaxed);
	}
	replace_packet_payload(carrier, *chosen);
	if (fromQueue) {
		queues[lane].pop_front();
		if (holdQueue)
			holdBufferedBytes_ = holdBufferedBytes_ >= bytes ? holdBufferedBytes_ - bytes : 0;
		else
			bufferedBytes_ = bufferedBytes_ >= bytes ? bufferedBytes_ - bytes : 0;
	}
	return true;
}

bool OutputSession::replace_with_fallback(std::array<ObsPacket, LaneCount> &fallbackPackets, encoder_packet *carrier)
{
	const std::size_t lane = lane_for(*carrier);
	ObsPacket &fallback = fallbackPackets[lane];
	if (!fallback.valid() || !packets_compatible(*carrier, fallback))
		return false;
	if (carrier->type == OBS_ENCODER_VIDEO) {
		const bool primary = &fallbackPackets == &primaryFallbackPackets_;
		const uint64_t captured = fallback.captured_at_ns();
		emittedVideoTimestampNs_.store(primary ? captured : 0, std::memory_order_relaxed);
		if (primary)
			effectiveSeconds_ = currentVideoCaptureNs_ >= captured
						    ? static_cast<double>(currentVideoCaptureNs_ - captured) /
							      static_cast<double>(NsPerSecond)
						    : 0.0;
	}
	replace_packet_payload(carrier, fallback);
	return true;
}

void OutputSession::process_primary_packet(encoder_packet *packet, const encoder_packet_time *packetTime)
{
	if (!packet)
		return;
	const uint64_t nowNs = os_gettime_ns();
	const uint64_t captureNs =
		packetTime && packetTime->cts
			? packetTime->cts
			: (packet->sys_dts_usec > 0 ? static_cast<uint64_t>(packet->sys_dts_usec) * 1000ULL : nowNs);
	note_throughput(packet->size, nowNs);
	const DelayState observed = state_.load(std::memory_order_acquire);
	if ((observed == DelayState::Bypass || observed == DelayState::Error) &&
	    timelineOffsetUsec_.load(std::memory_order_relaxed) == 0) {
		if (packet->type == OBS_ENCODER_VIDEO)
			emittedVideoTimestampNs_.store(captureNs, std::memory_order_relaxed);
		return;
	}
	bool notify = false;
	std::scoped_lock lock(mutex_);
	const int64_t rawDtsUsec = packet->dts_usec;
	note_primary_cadence_locked(*packet, rawDtsUsec);
	note_lane_format(primaryFormats_, *packet);
	latestPrimaryDtsUsec_ = std::max(latestPrimaryDtsUsec_, rawDtsUsec);
	if (packet->type == OBS_ENCODER_VIDEO) {
		currentVideoCaptureNs_ = captureNs;
		emittedVideoTimestampNs_.store(captureNs, std::memory_order_relaxed);
	}
	apply_timeline_offset_locked(*packet, timelineOffsetUsec_.load(std::memory_order_relaxed));
	const auto finishPacket = [this, packet] {
		note_emitted_video_locked(*packet);
	};

	DelayState current = state_.load(std::memory_order_relaxed);
	if (current == DelayState::Bypass || current == DelayState::Error) {
		effectiveSeconds_ = 0.0;
		finishPacket();
		return;
	}

	bool startingHoldEpoch = false;
	if (current == DelayState::Preparing) {
		if (packet->type != OBS_ENCODER_VIDEO || !packet->keyframe || !hold_preroll_ready_locked()) {
			finishPacket();
			return;
		}
		fillStartNs_ = nowNs;
		fillStartDtsUsec_ = rawDtsUsec;
		state_.store(DelayState::Filling, std::memory_order_release);
		detail_ = "Building the delayed buffer";
		obs_log(LOG_INFO, "%s: hold stream active; filling delayed buffer", label_.c_str());
		current = DelayState::Filling;
		startingHoldEpoch = true;
		notify = true;
	}

	if (current == DelayState::ReturningLive && packet->type == OBS_ENCODER_VIDEO && packet->keyframe) {
		begin_timeline_epoch_locked(*packet, nullptr);
		const bool completedWithError = errorReturnPending_;
		state_.store(completedWithError ? DelayState::Error : DelayState::Bypass, std::memory_order_release);
		if (!completedWithError)
			detail_ = "Live";
		else
			detail_ = std::move(pendingErrorDetail_) + " Live fallback active.";
		errorReturnPending_ = false;
		pendingErrorDetail_.clear();
		obs_log(completedWithError ? LOG_WARNING : LOG_INFO, "%s: live bypass restored%s", label_.c_str(),
			completedWithError ? " after a safe error fallback" : "");
		cleanupHoldRequested_ = true;
		effectiveSeconds_ = 0.0;
		notify = true;
		finishPacket();
		if (notify_)
			notify_();
		return;
	}

	if (current == DelayState::Filling) {
		if (!buffer_primary(packet, captureNs, rawDtsUsec)) {
			returnFromFilling_ = true;
			state_.store(DelayState::ReturningLive, std::memory_order_release);
			detail_ = "RAM safety limit reached — waiting for a live keyframe";
			if (startingHoldEpoch && !holdQueues_[0].empty())
				begin_timeline_epoch_locked(*packet, &holdQueues_[0].front());
			if (!replace_from(holdQueues_, &holdFallbackPackets_, packet, true))
				return_live_with_error_locked("RAM safety limit reached and the hold stream underrun.",
							      true);
			finishPacket();
			if (notify_)
				notify_();
			return;
		}
		const int64_t targetUsec = static_cast<int64_t>(targetSeconds_) * 1'000'000LL;
		const bool durationReady = rawDtsUsec >= fillStartDtsUsec_ &&
					   rawDtsUsec - fillStartDtsUsec_ >= targetUsec;
		const bool firstBufferedVideoIsKeyframe = !primaryQueues_[0].empty() &&
							  primaryQueues_[0].front().keyframe();
		if (durationReady && packet->type == OBS_ENCODER_VIDEO && firstBufferedVideoIsKeyframe) {
			begin_timeline_epoch_locked(*packet, &primaryQueues_[0].front());
			state_.store(DelayState::Delayed, std::memory_order_release);
			detail_ = "Delay active";
			obs_log(LOG_INFO, "%s: %u second delay active", label_.c_str(), targetSeconds_);
			cleanupHoldRequested_ = true;
			current = DelayState::Delayed;
			haveEmittedSourceDts_ = {};
			notify = true;
			if (!replace_from(primaryQueues_, &primaryFallbackPackets_, packet, false)) {
				const bool keptHold = replace_with_fallback(holdFallbackPackets_, packet);
				if (keptHold && packet->type == OBS_ENCODER_VIDEO)
					begin_timeline_epoch_locked(*packet, nullptr);
				return_live_with_error_locked("The delayed keyframe could not be emitted safely.",
							      keptHold);
				notify = true;
			}
		} else {
			bool usedFallback = false;
			if (startingHoldEpoch && !holdQueues_[0].empty())
				begin_timeline_epoch_locked(*packet, &holdQueues_[0].front());
			if (!replace_from(holdQueues_, &holdFallbackPackets_, packet, true, &usedFallback)) {
				return_live_with_error_locked("Hold encoder underrun without a safe fallback packet.",
							      true);
				notify = true;
			} else if (usedFallback) {
				if (packet->type == OBS_ENCODER_VIDEO)
					begin_timeline_epoch_locked(*packet, nullptr);
				return_live_with_error_locked("Hold encoder underrun.", true);
				notify = true;
			}
		}
	} else if (current == DelayState::Delayed) {
		if (!buffer_primary(packet, captureNs, rawDtsUsec)) {
			returnFromFilling_ = false;
			drainPrimaryOnReturn_ = true;
			state_.store(DelayState::ReturningLive, std::memory_order_release);
			detail_ = "RAM safety limit reached — waiting for a live keyframe";
			if (packet->type == OBS_ENCODER_VIDEO && packet->keyframe) {
				begin_timeline_epoch_locked(*packet, nullptr);
				state_.store(DelayState::Bypass, std::memory_order_release);
				cleanupHoldRequested_ = true;
			} else {
				bool usedFallback = false;
				if (!replace_from(primaryQueues_, &primaryFallbackPackets_, packet, false,
						  &usedFallback) ||
				    usedFallback) {
					if (usedFallback && packet->type == OBS_ENCODER_VIDEO)
						begin_timeline_epoch_locked(*packet, nullptr);
					return_live_with_error_locked(
						"RAM limit reached and the delayed queue could not be drained safely.",
						false);
				}
			}
			finishPacket();
			if (notify_)
				notify_();
			return;
		}
		realign_primary_locked(packet, rawDtsUsec);
		bool usedFallback = false;
		if (!replace_from(primaryQueues_, &primaryFallbackPackets_, packet, false, &usedFallback)) {
			return_live_with_error_locked("Delay buffer underrun; returning to live safely.", false);
			notify = true;
		} else if (usedFallback) {
			if (packet->type == OBS_ENCODER_VIDEO)
				begin_timeline_epoch_locked(*packet, nullptr);
			return_live_with_error_locked("Delay buffer underrun; returning to live safely.", false);
			notify = true;
		}
	} else if (current == DelayState::ReturningLive) {
		if (errorReturnPending_) {
			auto &fallbackPackets = returnFromFilling_ ? holdFallbackPackets_ : primaryFallbackPackets_;
			if (!replace_with_fallback(fallbackPackets, packet))
				obs_log(LOG_ERROR,
					"%s: safe fallback packet unavailable while waiting for a live keyframe",
					label_.c_str());
		} else if (returnFromFilling_) {
			if (!replace_from(holdQueues_, &holdFallbackPackets_, packet, true))
				return_live_with_error_locked("Hold encoder underrun while cancelling.", true);
		} else {
			if (!drainPrimaryOnReturn_ && !buffer_primary(packet, captureNs, rawDtsUsec)) {
				drainPrimaryOnReturn_ = true;
				detail_ = "RAM safety limit reached — draining until a live keyframe";
				notify = true;
			}
			bool usedFallback = false;
			if (!replace_from(primaryQueues_, &primaryFallbackPackets_, packet, false, &usedFallback)) {
				return_live_with_error_locked("Delay buffer underrun while returning live.", false);
			} else if (usedFallback) {
				if (packet->type == OBS_ENCODER_VIDEO)
					begin_timeline_epoch_locked(*packet, nullptr);
				return_live_with_error_locked("Delay buffer underrun while returning live.", false);
			}
		}
	}

	finishPacket();
	if (notify && notify_)
		notify_();
}

void OutputSession::receive_hold_packet(encoder_packet *packet)
{
	if (!packet)
		return;
	const uint64_t nowNs = os_gettime_ns();
	bool notify = false;
	std::scoped_lock lock(mutex_);
	const DelayState current = state_.load(std::memory_order_relaxed);
	if (current != DelayState::Preparing && current != DelayState::Filling &&
	    !(current == DelayState::ReturningLive && returnFromFilling_))
		return;

	const std::size_t lane = lane_for(*packet);
	note_lane_format(holdFormats_, *packet);
	if (current == DelayState::Preparing && packet->type == OBS_ENCODER_VIDEO && packet->keyframe && !holdReady_) {
		while (!holdQueues_[0].empty()) {
			holdBufferedBytes_ = holdBufferedBytes_ >= holdQueues_[0].front().size()
						     ? holdBufferedBytes_ - holdQueues_[0].front().size()
						     : 0;
			holdQueues_[0].pop_front();
		}
		holdGopStartDtsUsec_ = packet->dts_usec;
		for (std::size_t audioLane = 1; audioLane < LaneCount; ++audioLane) {
			while (!holdQueues_[audioLane].empty() &&
			       holdQueues_[audioLane].front().get()->dts_usec < holdGopStartDtsUsec_) {
				holdBufferedBytes_ =
					holdBufferedBytes_ >= holdQueues_[audioLane].front().size()
						? holdBufferedBytes_ - holdQueues_[audioLane].front().size()
						: 0;
				holdQueues_[audioLane].pop_front();
			}
		}
	}
	if ((packet->type == OBS_ENCODER_VIDEO && packet->keyframe) || packet->type == OBS_ENCODER_AUDIO)
		holdFallbackPackets_[lane] = ObsPacket(packet, nowNs);
	if (packet->size > MaxHoldBufferBytes - std::min(holdBufferedBytes_, MaxHoldBufferBytes)) {
		if (current == DelayState::Preparing) {
			set_error_locked("The hold encoder exceeded its 128 MiB preroll safety limit.");
		} else {
			returnFromFilling_ = true;
			state_.store(DelayState::ReturningLive, std::memory_order_release);
			detail_ = "Hold encoder backlog limit reached — waiting for a live keyframe";
			cleanupHoldRequested_ = true;
		}
		if (notify_)
			notify_();
		return;
	}
	holdQueues_[lane].emplace_back(packet, nowNs);
	holdBufferedBytes_ += packet->size;
	if (current == DelayState::Preparing) {
		bool formatsComplete = false;
		if (!hold_formats_compatible_locked(formatsComplete) && formatsComplete) {
			set_error_locked("The hold encoder packet timebase or track layout is incompatible.");
			if (notify_)
				notify_();
			return;
		}
	}
	if (current == DelayState::Preparing && packet->type == OBS_ENCODER_VIDEO && packet->keyframe && !holdReady_) {
		std::string error;
		if (!holdPipeline_ || !holdPipeline_->compatible_with_primary(error)) {
			set_error_locked(error.empty() ? "Hold encoder is not bitstream-compatible." : error);
			cleanupHoldRequested_ = true;
		} else {
			holdReady_ = true;
			detail_ = "Prerolling hold audio";
		}
		notify = true;
	}
	if (current == DelayState::Preparing && hold_preroll_ready_locked())
		detail_ = "Waiting for a live keyframe";

	// Do not let preroll grow without bound while waiting for a main keyframe.
	while (current == DelayState::Preparing && lane != 0 && holdQueues_[lane].size() > MaxHoldLanePackets) {
		holdBufferedBytes_ = holdBufferedBytes_ >= holdQueues_[lane].front().size()
					     ? holdBufferedBytes_ - holdQueues_[lane].front().size()
					     : 0;
		holdQueues_[lane].pop_front();
	}

	if (notify && notify_)
		notify_();
}

void OutputSession::clear_primary_buffer()
{
	for (auto &queue : primaryQueues_)
		queue.clear();
	for (auto &packet : primaryFallbackPackets_)
		packet.reset();
	bufferedBytes_ = 0;
}

void OutputSession::clear_hold_buffer()
{
	for (auto &queue : holdQueues_)
		queue.clear();
	for (auto &packet : holdFallbackPackets_)
		packet.reset();
	holdBufferedBytes_ = 0;
	holdGopStartDtsUsec_ = 0;
}

void OutputSession::set_error_locked(std::string message)
{
	state_.store(DelayState::Error, std::memory_order_release);
	detail_ = std::move(message);
	cleanupHoldRequested_ = true;
	obs_log(LOG_ERROR, "%s: %s", label_.c_str(), detail_.c_str());
}

void OutputSession::return_live_with_error_locked(std::string message, const bool fromFilling)
{
	errorReturnPending_ = true;
	returnFromFilling_ = fromFilling;
	pendingErrorDetail_ = std::move(message);
	detail_ = pendingErrorDetail_ + " Waiting for the next live keyframe.";
	state_.store(DelayState::ReturningLive, std::memory_order_release);
	obs_log(LOG_ERROR, "%s: %s", label_.c_str(), detail_.c_str());
}

std::size_t OutputSession::configured_bitrate_bytes(const uint32_t seconds, bool &available) const
{
	available = false;
	uint64_t kilobitsPerSecond = 0;
	if (obs_encoder_t *video = obs_output_get_video_encoder(output_)) {
		obs_data_t *settings = obs_encoder_get_settings(video);
		std::string rateControl = obs_data_get_string(settings, "rate_control");
		std::transform(rateControl.begin(), rateControl.end(), rateControl.begin(),
			       [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
		const bool qualityBased =
			rateControl.find("cqp") != std::string::npos || rateControl.find("crf") != std::string::npos ||
			rateControl.find("icq") != std::string::npos ||
			rateControl.find("lossless") != std::string::npos || obs_data_get_bool(settings, "lossless");
		const int64_t videoKilobits = std::max<int64_t>(0, obs_data_get_int(settings, "bitrate"));
		obs_data_release(settings);
		if (qualityBased || videoKilobits == 0)
			return 0;
		kilobitsPerSecond = static_cast<uint64_t>(videoKilobits);
	} else {
		return 0;
	}
	for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
		if (obs_encoder_t *audio = obs_output_get_audio_encoder(output_, index)) {
			obs_data_t *settings = obs_encoder_get_settings(audio);
			const int64_t audioKilobits = std::max<int64_t>(0, obs_data_get_int(settings, "bitrate"));
			obs_data_release(settings);
			if (audioKilobits == 0)
				return 0;
			kilobitsPerSecond += static_cast<uint64_t>(audioKilobits);
		}
	}
	available = true;
	return static_cast<std::size_t>(kilobitsPerSecond * 1000ULL * seconds / 8ULL * 12ULL / 10ULL);
}

std::size_t OutputSession::estimated_bytes(const uint32_t seconds, bool *available) const
{
	const double measuredBytesPerSecond = measuredBytesPerSecond_.load(std::memory_order_relaxed);
	if (measuredBytesPerSecond > 0.0) {
		if (available)
			*available = true;
		return static_cast<std::size_t>(measuredBytesPerSecond * static_cast<double>(seconds) * 1.2);
	}
	bool configuredAvailable = false;
	const std::size_t configured = configured_bitrate_bytes(seconds, configuredAvailable);
	if (available)
		*available = configuredAvailable;
	return configured;
}

DelaySnapshot OutputSession::snapshot() const
{
	std::scoped_lock lock(mutex_);
	DelaySnapshot result;
	result.state = state_.load(std::memory_order_relaxed);
	result.configuredSeconds = targetSeconds_;
	result.bufferedBytes = bufferedBytes_ + holdBufferedBytes_;
	const double measuredBytesPerSecond = measuredBytesPerSecond_.load(std::memory_order_relaxed);
	result.measuredMegabitsPerSecond = measuredBytesPerSecond * 8.0 / 1'000'000.0;
	result.estimatedBytes = estimated_bytes(targetSeconds_, &result.estimateAvailable);
	result.activeOutputs = 1;
	result.emittedVideoTimestampNs = emittedVideoTimestampNs_.load(std::memory_order_relaxed);
	result.bufferStartTimestampNs = primaryQueues_[0].empty() ? 0 : primaryQueues_[0].front().captured_at_ns();
	result.paused = paused_;
	result.emittingHold = result.state == DelayState::Filling ||
			      (result.state == DelayState::ReturningLive && returnFromFilling_);
	result.emittingDelayed = result.state == DelayState::Delayed ||
				 (result.state == DelayState::ReturningLive && !returnFromFilling_);
	result.effectiveSeconds = result.emittingDelayed ? effectiveSeconds_ : 0.0;
	result.detail = detail_;
	if (result.state == DelayState::Filling && fillStartNs_ != 0 && targetSeconds_ != 0) {
		const double elapsed =
			latestPrimaryDtsUsec_ >= fillStartDtsUsec_
				? static_cast<double>(latestPrimaryDtsUsec_ - fillStartDtsUsec_) / 1'000'000.0
				: 0.0;
		result.progress = std::clamp(elapsed / targetSeconds_, 0.0, 1.0);
	} else if (result.state == DelayState::Delayed) {
		result.progress = 1.0;
	}
	return result;
}

} // namespace dynamic_delay
