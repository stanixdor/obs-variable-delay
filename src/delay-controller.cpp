#include "delay-controller.hpp"

#include "audience-preview.hpp"
#include "audio-preflight.hpp"
#include "hold-media-hub.hpp"
#include "output-session.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QMetaType>
#include <QMetaObject>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dynamic_delay {
namespace {

constexpr const char *InternalCaptureOutputId = "obs_dynamic_delay_capture_output";
constexpr const char *InternalAudioTapSourceId = "obs_dynamic_delay_hold_audio_tap";

uint64_t mix_fingerprint(uint64_t value) noexcept
{
	value ^= value >> 30U;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27U;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31U);
}

uint32_t global_output_mixer_mask(obs_output_t *output)
{
	if (!output || !obs_output_active(output) || (obs_output_get_flags(output) & OBS_OUTPUT_AUDIO) == 0)
		return 0;
	const uint32_t flags = obs_output_get_flags(output);
	uint32_t mask = 0;
	if ((flags & OBS_OUTPUT_ENCODED) != 0) {
		for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
			obs_encoder_t *encoder = obs_output_get_audio_encoder(output, index);
			if (!encoder || obs_encoder_get_type(encoder) != OBS_ENCODER_AUDIO)
				continue;
			audio_t *audio = obs_encoder_audio(encoder);
			if (audio && audio != obs_get_audio())
				continue;
			const std::size_t mixer = obs_encoder_get_mixer_index(encoder);
			if (mixer < MAX_AUDIO_MIXES)
				mask |= 1U << mixer;
		}
		return mask;
	}

	audio_t *audio = obs_output_audio(output);
	if (audio && audio != obs_get_audio())
		return 0;
	if ((flags & OBS_OUTPUT_MULTI_TRACK) != 0)
		return static_cast<uint32_t>(obs_output_get_mixers(output)) & ((1U << MAX_AUDIO_MIXES) - 1U);
	const std::size_t mixer = obs_output_get_mixer(output);
	return mixer < MAX_AUDIO_MIXES ? 1U << mixer : 0;
}

uint64_t reserved_audio_fingerprint()
{
	std::vector<uint64_t> tokens;
	obs_enum_outputs(
		[](void *param, obs_output_t *output) {
			auto &tokens = *static_cast<std::vector<uint64_t> *>(param);
			const char *id = output ? obs_output_get_id(output) : nullptr;
			if (!output || !obs_output_active(output) ||
			    (id && std::strcmp(id, InternalCaptureOutputId) == 0))
				return true;
			const uint64_t identity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(output));
			const uint64_t state = (static_cast<uint64_t>(obs_output_get_flags(output)) << 32U) |
					       global_output_mixer_mask(output);
			tokens.push_back(mix_fingerprint(identity ^ 0x4f55545055540000ULL) ^ mix_fingerprint(state));
			return true;
		},
		&tokens);
	obs_enum_all_sources(
		[](void *param, obs_source_t *source) {
			auto &tokens = *static_cast<std::vector<uint64_t> *>(param);
			const char *id = source ? obs_source_get_id(source) : nullptr;
			if (!source || obs_source_removed(source) ||
			    (id && std::strcmp(id, InternalAudioTapSourceId) == 0) ||
			    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			const uint64_t identity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source));
			const uint64_t mixers = obs_source_get_audio_mixers(source);
			tokens.push_back(mix_fingerprint(identity ^ 0x534f555243450000ULL) ^ mix_fingerprint(mixers));
			return true;
		},
		&tokens);
	std::sort(tokens.begin(), tokens.end());
	uint64_t fingerprint = 1469598103934665603ULL;
	for (const uint64_t token : tokens) {
		fingerprint ^= token;
		fingerprint *= 1099511628211ULL;
	}
	return fingerprint ^ mix_fingerprint(tokens.size());
}

int state_rank(const DelayState state)
{
	switch (state) {
	case DelayState::Error:
		return 6;
	case DelayState::ReturningLive:
		return 5;
	case DelayState::Filling:
		return 4;
	case DelayState::Preparing:
		return 3;
	case DelayState::Delayed:
		return 2;
	case DelayState::Bypass:
		return 1;
	}
	return 0;
}

void ensure_config_directory()
{
	char *directory = obs_module_config_path("");
	if (!directory)
		return;
	os_mkdirs(directory);
	bfree(directory);
}

} // namespace

DelayController::DelayController(QObject *parent) : QObject(parent)
{
	qRegisterMetaType<DelaySnapshot>();
	qRegisterMetaType<DelaySettings>();
	qRegisterMetaType<std::vector<SceneChoice>>();
	qRegisterMetaType<std::vector<AudioSourceChoice>>();
	load_settings();
	refresh_scenes();
	refresh_audio_sources();
	obs_frontend_add_event_callback(&DelayController::frontend_event, this);
	signal_handler_t *coreSignals = obs_get_signal_handler();
	signal_handler_connect(coreSignals, "source_create", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "source_create_canvas", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "source_remove", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "source_destroy", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "source_rename", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "canvas_create", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "canvas_remove", &DelayController::source_event, this);
	signal_handler_connect(coreSignals, "canvas_destroy", &DelayController::source_event, this);
	connect(&pollTimer_, &QTimer::timeout, this, &DelayController::poll);
	pollTimer_.start(100);
	attach_current_outputs();
}

DelayController::~DelayController()
{
	shutdown();
}

void DelayController::shutdown()
{
	if (shutdownComplete_.exchange(true, std::memory_order_acq_rel))
		return;
	shuttingDown_.store(true, std::memory_order_release);
	pollTimer_.stop();
	obs_frontend_remove_event_callback(&DelayController::frontend_event, this);
	signal_handler_t *coreSignals = obs_get_signal_handler();
	signal_handler_disconnect(coreSignals, "source_create", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "source_create_canvas", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "source_remove", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "source_destroy", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "source_rename", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "canvas_create", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "canvas_remove", &DelayController::source_event, this);
	signal_handler_disconnect(coreSignals, "canvas_destroy", &DelayController::source_event, this);
	disconnect_observers();
	std::unordered_map<obs_output_t *, std::unique_ptr<OutputSession>> retiredSessions;
	std::shared_ptr<HoldMediaHub> retiredHub;
	{
		std::scoped_lock lock(mutex_);
		retiredSessions.swap(sessions_);
		retiredHub = std::move(activeMediaHub_);
	}
	retiredSessions.clear();
	retiredHub.reset();
}

DelaySettings DelayController::settings() const
{
	std::scoped_lock lock(mutex_);
	return settings_;
}

std::vector<SceneChoice> DelayController::scenes() const
{
	std::scoped_lock lock(mutex_);
	return scenes_;
}

std::vector<AudioSourceChoice> DelayController::audio_sources() const
{
	std::scoped_lock lock(mutex_);
	return audioSources_;
}

QString DelayController::audio_preflight() const
{
	std::scoped_lock lock(mutex_);
	return audioPreflight_;
}

DelaySnapshot DelayController::snapshot() const
{
	std::scoped_lock lock(mutex_);
	DelaySnapshot aggregate;
	aggregate.configuredSeconds = settings_.delaySeconds;
	aggregate.state = DelayState::Bypass;
	aggregate.activeOutputs = sessions_.size();
	aggregate.estimateAvailable = !sessions_.empty();
	if (settings_.previewExpanded)
		aggregate.estimatedBytes += audience_preview_estimated_bytes(settings_.delaySeconds);
	if (sessions_.empty()) {
		if (!controllerError_.empty() || !outputErrors_.empty()) {
			aggregate.state = DelayState::Error;
			aggregate.detail = !controllerError_.empty() ? controllerError_ : outputErrors_.begin()->second;
		} else if (requestedActive_) {
			aggregate.state = DelayState::Preparing;
			aggregate.detail = "Armed — start streaming or recording";
		} else {
			aggregate.detail = "Live";
		}
		return aggregate;
	}

	double progressTotal = 0.0;
	for (const auto &[_, session] : sessions_) {
		const DelaySnapshot current = session->snapshot();
		aggregate.bufferedBytes += current.bufferedBytes;
		bool estimateAvailable = false;
		aggregate.estimatedBytes += session->estimated_bytes(settings_.delaySeconds, &estimateAvailable);
		aggregate.estimateAvailable = aggregate.estimateAvailable && estimateAvailable;
		aggregate.measuredMegabitsPerSecond += current.measuredMegabitsPerSecond;
		aggregate.emittingHold = aggregate.emittingHold || current.emittingHold;
		aggregate.emittingDelayed = aggregate.emittingDelayed || current.emittingDelayed;
		progressTotal += current.progress;
		if (state_rank(current.state) > state_rank(aggregate.state)) {
			aggregate.state = current.state;
			aggregate.detail = session->label() + ": " + current.detail;
		}
	}
	aggregate.progress = progressTotal / static_cast<double>(sessions_.size());
	if (aggregate.detail.empty())
		aggregate.detail = aggregate.state == DelayState::Delayed ? "Delay active" : "Live";
	if (!controllerError_.empty() || !outputErrors_.empty()) {
		aggregate.state = DelayState::Error;
		aggregate.detail = !controllerError_.empty() ? controllerError_ : outputErrors_.begin()->second;
	}
	return aggregate;
}

void DelayController::frontend_event(const enum obs_frontend_event event, void *privateData)
{
	auto *self = static_cast<DelayController *>(privateData);
	if (self && !self->shuttingDown_)
		self->handle_frontend_event(event);
}

void DelayController::source_event(void *privateData, calldata_t *)
{
	auto *self = static_cast<DelayController *>(privateData);
	if (!self || self->shuttingDown_ || self->topologyRefreshQueued_.exchange(true, std::memory_order_acq_rel))
		return;
	QMetaObject::invokeMethod(
		self,
		[self] {
			self->topologyRefreshQueued_.store(false, std::memory_order_release);
			if (self->shuttingDown_)
				return;
			self->refresh_scene_observers();
			self->refresh_audio_sources();
			self->mark_dirty();
		},
		Qt::QueuedConnection);
}

void DelayController::preflight_event(void *privateData, calldata_t *)
{
	auto *self = static_cast<DelayController *>(privateData);
	if (!self || self->shuttingDown_ || self->preflightRefreshQueued_.exchange(true, std::memory_order_acq_rel))
		return;
	QMetaObject::invokeMethod(
		self,
		[self] {
			self->preflightRefreshQueued_.store(false, std::memory_order_release);
			if (!self->shuttingDown_)
				self->refresh_audio_preflight();
		},
		Qt::QueuedConnection);
}

void DelayController::handle_frontend_event(const enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED: {
		obs_output_t *output = obs_frontend_get_streaming_output();
		add_output(output, "Streaming");
		obs_output_release(output);
		refresh_audio_preflight();
		bool startNow = false;
		{
			std::scoped_lock lock(mutex_);
			startNow = requestedActive_ && !rearmPending_;
		}
		if (startNow)
			start_delay_on_sessions();
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STARTED: {
		obs_output_t *output = obs_frontend_get_recording_output();
		add_output(output, "Recording");
		obs_output_release(output);
		refresh_audio_preflight();
		bool startNow = false;
		{
			std::scoped_lock lock(mutex_);
			startNow = requestedActive_ && !rearmPending_;
		}
		if (startNow)
			start_delay_on_sessions();
		break;
	}
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED: {
		obs_output_t *output = obs_frontend_get_streaming_output();
		remove_output(output, "Streaming");
		obs_output_release(output);
		refresh_audio_preflight();
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		obs_output_t *output = obs_frontend_get_recording_output();
		remove_output(output, "Recording");
		obs_output_release(output);
		refresh_audio_preflight();
		break;
	}
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		refresh_audio_preflight();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_CANVAS_ADDED:
	case OBS_FRONTEND_EVENT_CANVAS_REMOVED:
		refresh_scenes();
		refresh_audio_sources();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontendLoaded_ = true;
		refresh_scenes();
		refresh_audio_sources();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		// Program may have switched to the selected hold scene while its
		// private audio graph is filling.  Revalidate before OBS's duplicate
		// source routing can affect Program audio.
		refresh_audio_preflight();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		shutdown();
		return;
	default:
		break;
	}
	mark_dirty();
}

void DelayController::attach_current_outputs()
{
	if (obs_frontend_streaming_active()) {
		obs_output_t *output = obs_frontend_get_streaming_output();
		add_output(output, "Streaming");
		obs_output_release(output);
	}
	if (obs_frontend_recording_active()) {
		obs_output_t *output = obs_frontend_get_recording_output();
		add_output(output, "Recording");
		obs_output_release(output);
	}
}

void DelayController::add_output(obs_output_t *output, const std::string &label)
{
	if (!output)
		return;
	std::scoped_lock lock(mutex_);
	if (sessions_.contains(output))
		return;
	auto session = std::make_unique<OutputSession>(output, label, [this] { mark_dirty(); });
	std::string error;
	if (!session->attach(error)) {
		obs_log(LOG_WARNING, "%s output not attached: %s", label.c_str(), error.c_str());
		outputErrors_[label] = label + ": " + error;
		return;
	}
	outputErrors_.erase(label);
	sessions_.emplace(output, std::move(session));
}

void DelayController::remove_output(obs_output_t *output, const std::string &label)
{
	std::unique_ptr<OutputSession> removed;
	{
		std::scoped_lock lock(mutex_);
		outputErrors_.erase(label);
		if (!output)
			return;
		reconnectPending_.erase(output);
		auto it = sessions_.find(output);
		if (it == sessions_.end())
			return;
		removed = std::move(it->second);
		sessions_.erase(it);
	}
	removed.reset();
}

void DelayController::start_delay_on_output(obs_output_t *output)
{
	{
		std::scoped_lock lock(mutex_);
		if (!requestedActive_ || rearmPending_)
			return;
	}
	obs_source_t *scene = selected_scene();
	if (!scene) {
		std::scoped_lock lock(mutex_);
		controllerError_ = "Select a valid hold scene before adding delay.";
		requestedActive_ = false;
		mark_dirty();
		return;
	}
	std::string hubError;
	auto mediaHub = create_media_hub(scene, hubError);
	if (!mediaHub) {
		bool rearming = false;
		{
			std::scoped_lock lock(mutex_);
			rearming = rearmPending_;
			if (!rearming) {
				controllerError_ = hubError;
				requestedActive_ = false;
			}
		}
		obs_source_release(scene);
		mark_dirty();
		return;
	}

	{
		std::scoped_lock lock(mutex_);
		auto it = sessions_.find(output);
		if (requestedActive_ && !rearmPending_ && it != sessions_.end() && obs_output_active(output) &&
		    it->second->is_bypass()) {
			std::string error;
			if (!it->second->request_delay(settings_.delaySeconds, mediaHub, error)) {
				obs_log(LOG_ERROR, "%s delay activation failed: %s", it->second->label().c_str(),
					error.c_str());
				outputErrors_[it->second->label()] = it->second->label() + ": " + error;
			} else {
				outputErrors_.erase(it->second->label());
			}
		}
	}
	obs_source_release(scene);
	mark_dirty();
}

obs_source_t *DelayController::selected_scene() const
{
	std::string uuid;
	{
		std::scoped_lock lock(mutex_);
		uuid = settings_.holdSceneUuid;
	}
	if (uuid.empty())
		return nullptr;
	obs_source_t *source = obs_get_source_by_uuid(uuid.c_str());
	if (!source)
		return nullptr;
	if (obs_source_removed(source) || !obs_scene_from_source(source)) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

obs_source_t *DelayController::selected_audio_source() const
{
	std::string uuid;
	{
		std::scoped_lock lock(mutex_);
		uuid = settings_.holdAudioSourceUuid;
	}
	if (uuid.empty())
		return nullptr;
	obs_source_t *source = obs_get_source_by_uuid(uuid.c_str());
	if (!source)
		return nullptr;
	if (obs_source_removed(source) || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
	    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

std::shared_ptr<HoldMediaHub> DelayController::create_media_hub(obs_source_t *scene, std::string &error)
{
	obs_source_t *dedicatedSource = selected_audio_source();
	DelaySettings activationSettings;
	std::vector<obs_output_t *> outputs;
	{
		std::scoped_lock lock(mutex_);
		activationSettings = settings_;
		outputs.reserve(sessions_.size());
		for (const auto &[output, _] : sessions_)
			outputs.push_back(output);
	}
	AudioPreflightResult preflight = preflight_hold_audio(activationSettings, scene, dedicatedSource, outputs);

	auto publishDiagnostic = [this](const std::string &message) {
		const QString translated = QString::fromStdString(message);
		bool changed = false;
		{
			std::scoped_lock lock(mutex_);
			changed = audioPreflight_ != translated;
			audioPreflight_ = translated;
		}
		if (changed)
			emit audio_preflight_changed(translated);
	};
	std::string diagnostic = preflight.message;
	if (preflight.degraded)
		obs_log(LOG_WARNING, "Hold audio preflight fallback: %s", preflight.message.c_str());
	else
		obs_log(LOG_INFO, "Hold audio preflight: %s", preflight.message.c_str());

	const HoldAudioConfig requestedConfig{activationSettings.holdAudioMode, dedicatedSource,
					      activationSettings.holdAudioSourceUuid, preflight.reservedMixerIndex};
	HoldAudioConfig audioConfig{preflight.effectiveMode, dedicatedSource, activationSettings.holdAudioSourceUuid,
				    preflight.reservedMixerIndex};
	std::shared_ptr<HoldMediaHub> reusableHub;
	bool scheduledRearm = false;
	{
		std::scoped_lock lock(mutex_);
		if (activeMediaHub_ && activeMediaHub_->active() && activeMediaHub_.use_count() > 1) {
			if (activeMediaHub_->matches(scene, requestedConfig)) {
				reusableHub = activeMediaHub_;
			} else if (requestedActive_) {
				rearmPending_ = true;
				reconnectPending_.clear();
				for (auto &[_, session] : sessions_)
					session->request_bypass();
				scheduledRearm = true;
			}
		}
	}
	if (reusableHub) {
		const HoldAudioMode activeMode = reusableHub->audio_mode();
		bool muted = false;
		if (activeMode != HoldAudioMode::Silence) {
			DelaySettings backendSettings = activationSettings;
			backendSettings.holdAudioMode = activeMode;
			backendSettings.reservedAudioTrack =
				static_cast<uint32_t>(reusableHub->reserved_mixer_index() + 1U);
			const AudioPreflightResult backendCheck =
				preflight_hold_audio(backendSettings, reusableHub->hold_scene(),
						     reusableHub->dedicated_audio_source(), outputs);
			if (backendCheck.effectiveMode != activeMode && reusableHub->force_audio_silence()) {
				diagnostic += " The current activation was muted to preserve video delay.";
				muted = true;
			}
		}
		if (!muted && reusableHub->audio_mode() != preflight.effectiveMode)
			diagnostic +=
				" The current activation keeps its existing safe audio backend until delay is reactivated.";
		publishDiagnostic(diagnostic);
		if (muted)
			obs_log(LOG_WARNING, "%s", diagnostic.c_str());
		obs_source_release(dedicatedSource);
		error.clear();
		return reusableHub;
	}
	if (scheduledRearm) {
		publishDiagnostic(diagnostic);
		error = "Hold audio safety conditions changed; rebuilding the shared hold graph.";
		obs_log(LOG_WARNING, "%s", error.c_str());
		obs_source_release(dedicatedSource);
		mark_dirty();
		return {};
	}

	std::shared_ptr<HoldMediaHub> retiredHub;
	{
		std::scoped_lock lock(mutex_);
		if (activeMediaHub_ && activeMediaHub_.use_count() == 1)
			retiredHub = std::move(activeMediaHub_);
	}
	retiredHub.reset();

	std::string backendError;
	auto mediaHub = HoldMediaHub::create(scene, audioConfig, backendError);
	if (!mediaHub && preflight.effectiveMode != HoldAudioMode::Silence) {
		const std::string requestedBackendError = backendError;
		audioConfig.mode = HoldAudioMode::Silence;
		backendError.clear();
		mediaHub = HoldMediaHub::create(scene, audioConfig, backendError);
		if (mediaHub) {
			diagnostic +=
				" Audio backend initialization failed (" + requestedBackendError + "); using silence.";
			obs_log(LOG_WARNING, "%s", diagnostic.c_str());
		}
	}
	if (mediaHub)
		mediaHub->set_activation_signature(requestedConfig);
	obs_source_release(dedicatedSource);
	if (!mediaHub) {
		publishDiagnostic(diagnostic);
		error = backendError.empty() ? "Could not initialize the hold media graph." : backendError;
		return {};
	}
	publishDiagnostic(diagnostic);
	{
		std::scoped_lock lock(mutex_);
		retiredHub = std::move(activeMediaHub_);
		activeMediaHub_ = mediaHub;
	}
	retiredHub.reset();
	error.clear();
	return mediaHub;
}

void DelayController::start_delay_on_sessions()
{
	{
		std::scoped_lock lock(mutex_);
		if (!requestedActive_ || rearmPending_)
			return;
		if (sessions_.empty()) {
			controllerError_.clear();
			mark_dirty();
			return;
		}
	}
	obs_source_t *scene = selected_scene();
	if (!scene) {
		obs_log(LOG_WARNING, "Cannot activate delay: no valid hold scene selected");
		{
			std::scoped_lock lock(mutex_);
			requestedActive_ = false;
			controllerError_ = "Select a valid hold scene before adding delay.";
		}
		mark_dirty();
		return;
	}
	std::string hubError;
	auto mediaHub = create_media_hub(scene, hubError);
	if (!mediaHub) {
		bool rearming = false;
		{
			std::scoped_lock lock(mutex_);
			rearming = rearmPending_;
			if (!rearming) {
				requestedActive_ = false;
				controllerError_ = hubError;
			}
		}
		obs_log(rearming ? LOG_INFO : LOG_ERROR, "Cannot activate delay: %s", hubError.c_str());
		obs_source_release(scene);
		mark_dirty();
		return;
	}

	uint32_t seconds = 0;
	bool cancelled = false;
	{
		std::scoped_lock lock(mutex_);
		seconds = settings_.delaySeconds;
		controllerError_.clear();
		cancelled = !requestedActive_ || rearmPending_;
		for (auto &[_, session] : sessions_) {
			if (cancelled)
				break;
			if (!obs_output_active(session->output()) || !session->is_bypass())
				continue;
			std::string error;
			if (!session->request_delay(seconds, mediaHub, error)) {
				obs_log(LOG_ERROR, "%s delay activation failed: %s", session->label().c_str(),
					error.c_str());
				outputErrors_[session->label()] = session->label() + ": " + error;
			} else {
				outputErrors_.erase(session->label());
			}
		}
	}
	obs_source_release(scene);
	mark_dirty();
}

void DelayController::toggle_delay()
{
	bool activate = false;
	bool startImmediately = false;
	{
		std::scoped_lock lock(mutex_);
		activate = !requestedActive_;
		requestedActive_ = activate;
		if (activate)
			controllerError_.clear();
		if (!activate) {
			rearmPending_ = false;
			reconnectPending_.clear();
			for (auto &[_, session] : sessions_)
				session->request_bypass();
		} else {
			rearmPending_ = std::any_of(sessions_.begin(), sessions_.end(),
						    [](const auto &entry) { return !entry.second->is_bypass(); });
			startImmediately = !rearmPending_;
		}
	}
	if (activate && startImmediately)
		start_delay_on_sessions();
	mark_dirty();
}

void DelayController::request_rearm()
{
	std::scoped_lock lock(mutex_);
	if (!requestedActive_)
		return;
	rearmPending_ = true;
	reconnectPending_.clear();
	for (auto &[_, session] : sessions_)
		session->request_bypass();
}

void DelayController::set_delay_seconds(const int seconds)
{
	{
		std::scoped_lock lock(mutex_);
		settings_.delaySeconds = static_cast<uint32_t>(std::clamp(seconds, 1, 300));
	}
	save_settings();
	request_rearm();
	emit settings_changed(settings());
	mark_dirty();
}

void DelayController::set_hold_scene(const QString &uuid)
{
	{
		std::scoped_lock lock(mutex_);
		settings_.holdSceneUuid = uuid.toStdString();
	}
	save_settings();
	refresh_audio_preflight();
	request_rearm();
	emit settings_changed(settings());
}

void DelayController::set_transition_style(const int style)
{
	(void)style;
	{
		std::scoped_lock lock(mutex_);
		settings_.transition = TransitionStyle::Cut;
	}
	save_settings();
	emit settings_changed(settings());
}

void DelayController::set_hold_audio_mode(const int mode)
{
	HoldAudioMode next = HoldAudioMode::SceneMix;
	switch (mode) {
	case static_cast<int>(HoldAudioMode::DedicatedSource):
		next = HoldAudioMode::DedicatedSource;
		break;
	case static_cast<int>(HoldAudioMode::ReservedTrack):
		next = HoldAudioMode::ReservedTrack;
		break;
	case static_cast<int>(HoldAudioMode::Silence):
		next = HoldAudioMode::Silence;
		break;
	default:
		break;
	}
	bool changed = false;
	{
		std::scoped_lock lock(mutex_);
		changed = settings_.holdAudioMode != next;
		settings_.holdAudioMode = next;
	}
	if (!changed)
		return;
	save_settings();
	refresh_audio_preflight();
	request_rearm();
	emit settings_changed(settings());
	mark_dirty();
}

void DelayController::set_hold_audio_source(const QString &uuid)
{
	bool changed = false;
	{
		std::scoped_lock lock(mutex_);
		const std::string next = uuid.toStdString();
		changed = settings_.holdAudioSourceUuid != next;
		settings_.holdAudioSourceUuid = next;
	}
	if (!changed)
		return;
	save_settings();
	refresh_audio_preflight();
	request_rearm();
	emit settings_changed(settings());
	mark_dirty();
}

void DelayController::set_reserved_audio_track(const int track)
{
	const uint32_t next = static_cast<uint32_t>(std::clamp(track, 1, MAX_AUDIO_MIXES));
	bool changed = false;
	{
		std::scoped_lock lock(mutex_);
		changed = settings_.reservedAudioTrack != next;
		settings_.reservedAudioTrack = next;
	}
	if (!changed)
		return;
	save_settings();
	refresh_audio_preflight();
	request_rearm();
	emit settings_changed(settings());
	mark_dirty();
}

void DelayController::set_preview_expanded(const bool expanded)
{
	{
		std::scoped_lock lock(mutex_);
		settings_.previewExpanded = expanded;
	}
	save_settings();
	emit settings_changed(settings());
	mark_dirty();
}

void DelayController::poll()
{
	poll_reserved_audio_topology();
	bool shouldRearm = false;
	std::vector<obs_output_t *> reconnectReady;
	std::shared_ptr<HoldMediaHub> retiredHub;
	{
		std::scoped_lock lock(mutex_);
		for (auto &[_, session] : sessions_) {
			session->maintenance();
			if (session->take_rearm_request() && requestedActive_ && !rearmPending_)
				reconnectPending_.insert(session->output());
		}
		for (auto it = reconnectPending_.begin(); it != reconnectPending_.end();) {
			auto session = sessions_.find(*it);
			if (!requestedActive_ || session == sessions_.end()) {
				it = reconnectPending_.erase(it);
				continue;
			}
			if (!rearmPending_ && session->second->is_bypass()) {
				reconnectReady.push_back(*it);
				it = reconnectPending_.erase(it);
				continue;
			}
			++it;
		}
		if (rearmPending_) {
			shouldRearm = std::all_of(sessions_.begin(), sessions_.end(),
						  [](const auto &entry) { return entry.second->is_bypass(); });
			if (shouldRearm)
				rearmPending_ = false;
		}
		if (activeMediaHub_ && activeMediaHub_.use_count() == 1)
			retiredHub = std::move(activeMediaHub_);
	}
	retiredHub.reset();
	if (shouldRearm)
		start_delay_on_sessions();
	for (obs_output_t *output : reconnectReady)
		start_delay_on_output(output);
	if (dirty_.exchange(false, std::memory_order_relaxed) || requestedActive_)
		emit snapshot_changed(snapshot());
}

void DelayController::poll_reserved_audio_topology()
{
	bool monitor = false;
	{
		std::scoped_lock lock(mutex_);
		monitor = activeMediaHub_ && activeMediaHub_->active() && activeMediaHub_.use_count() > 1 &&
			  activeMediaHub_->configured_audio_mode() == HoldAudioMode::ReservedTrack;
	}
	if (!monitor) {
		reservedAudioFingerprintValid_ = false;
		return;
	}

	const uint64_t fingerprint = reserved_audio_fingerprint();
	const bool changed = !reservedAudioFingerprintValid_ || fingerprint != reservedAudioFingerprint_;
	reservedAudioFingerprint_ = fingerprint;
	reservedAudioFingerprintValid_ = true;
	if (changed)
		refresh_audio_preflight();
}

void DelayController::refresh_scenes()
{
	std::vector<SceneChoice> next;
	obs_frontend_source_list list{};
	obs_frontend_get_scenes(&list);
	for (size_t index = 0; index < list.sources.num; ++index) {
		obs_source_t *source = list.sources.array[index];
		if (!source)
			continue;
		next.push_back({QString::fromUtf8(obs_source_get_name(source)),
				QString::fromUtf8(obs_source_get_uuid(source))});
	}
	obs_frontend_source_list_free(&list);
	refresh_scene_observers();
	std::sort(next.begin(), next.end(), [](const SceneChoice &first, const SceneChoice &second) {
		return first.name.localeAwareCompare(second.name) < 0;
	});

	bool selectionChanged = false;
	{
		std::scoped_lock lock(mutex_);
		scenes_ = next;
		const bool selectedStillExists =
			std::any_of(scenes_.begin(), scenes_.end(), [this](const SceneChoice &scene) {
				return scene.uuid.toStdString() == settings_.holdSceneUuid;
			});
		if (frontendLoaded_ && !selectedStillExists) {
			const std::string previous = settings_.holdSceneUuid;
			settings_.holdSceneUuid = scenes_.empty() ? std::string{} : scenes_.front().uuid.toStdString();
			selectionChanged = previous != settings_.holdSceneUuid;
		}
	}
	save_settings();
	if (selectionChanged)
		request_rearm();
	emit scenes_changed(scenes());
	emit settings_changed(settings());
	refresh_audio_preflight();
}

void DelayController::refresh_scene_observers()
{
	std::vector<OBSSignal> nextObservers;
	std::unordered_set<obs_source_t *> sceneIdentities;
	struct CanvasContext {
		std::vector<OBSSignal> *observers = nullptr;
		std::unordered_set<obs_source_t *> *identities = nullptr;
		signal_callback_t callback = nullptr;
		void *privateData = nullptr;
	} context{&nextObservers, &sceneIdentities, &DelayController::preflight_event, this};
	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) {
			auto &context = *static_cast<CanvasContext *>(param);
			if (!canvas || obs_canvas_removed(canvas))
				return true;
			if (signal_handler_t *handler = obs_canvas_get_signal_handler(canvas))
				context.observers->emplace_back(handler, "channel_change", context.callback,
								context.privateData);
			obs_canvas_enum_scenes(
				canvas,
				[](void *sceneParam, obs_source_t *source) {
					auto &context = *static_cast<CanvasContext *>(sceneParam);
					if (source && !obs_source_removed(source) &&
					    context.identities->insert(source).second) {
						if (signal_handler_t *handler = obs_source_get_signal_handler(source)) {
							context.observers->emplace_back(handler, "item_add",
											context.callback,
											context.privateData);
							context.observers->emplace_back(handler, "item_remove",
											context.callback,
											context.privateData);
							context.observers->emplace_back(handler, "item_visible",
											context.callback,
											context.privateData);
							context.observers->emplace_back(handler, "reorder",
											context.callback,
											context.privateData);
						}
					}
					return true;
				},
				&context);
			return true;
		},
		&context);
	obs_enum_all_sources(
		[](void *param, obs_source_t *source) {
			auto &context = *static_cast<CanvasContext *>(param);
			if (!source || obs_source_removed(source) || !obs_group_from_source(source))
				return true;
			obs_canvas_t *canvas = obs_source_get_canvas(source);
			const bool liveCanvas = canvas && !obs_canvas_removed(canvas);
			obs_canvas_release(canvas);
			if (liveCanvas && context.identities->insert(source).second) {
				if (signal_handler_t *handler = obs_source_get_signal_handler(source)) {
					context.observers->emplace_back(handler, "item_add", context.callback,
									context.privateData);
					context.observers->emplace_back(handler, "item_remove", context.callback,
									context.privateData);
					context.observers->emplace_back(handler, "item_visible", context.callback,
									context.privateData);
					context.observers->emplace_back(handler, "reorder", context.callback,
									context.privateData);
				}
			}
			return true;
		},
		&context);

	// Ref-counted signal connections keep only each handler alive.  They do not
	// retain the source/canvas object, so third-party objects can reach their
	// normal destroy signal even when they were not explicitly removed first.
	sceneObserverSignals_ = std::move(nextObservers);
}

void DelayController::refresh_audio_sources()
{
	std::vector<AudioSourceChoice> next;
	std::vector<OBSSignal> nextObservers;
	struct AudioSourceContext {
		std::vector<AudioSourceChoice> *choices = nullptr;
		std::vector<OBSSignal> *observers = nullptr;
		signal_callback_t callback = nullptr;
		void *privateData = nullptr;
	} context{&next, &nextObservers, &DelayController::preflight_event, this};
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto &context = *static_cast<AudioSourceContext *>(data);
			if (!source || obs_source_removed(source) ||
			    obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
			    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			const char *name = obs_source_get_name(source);
			const char *uuid = obs_source_get_uuid(source);
			if (uuid && *uuid) {
				context.choices->push_back(
					{QString::fromUtf8(name ? name : ""), QString::fromUtf8(uuid)});
				if (signal_handler_t *handler = obs_source_get_signal_handler(source)) {
					context.observers->emplace_back(handler, "audio_mixers", context.callback,
									context.privateData);
					context.observers->emplace_back(handler, "audio_sync", context.callback,
									context.privateData);
				}
			}
			return true;
		},
		&context);
	std::sort(next.begin(), next.end(), [](const AudioSourceChoice &first, const AudioSourceChoice &second) {
		return first.name.localeAwareCompare(second.name) < 0;
	});
	{
		std::scoped_lock lock(mutex_);
		audioSources_ = std::move(next);
	}
	audioObserverSignals_ = std::move(nextObservers);
	refresh_audio_preflight();
	emit audio_sources_changed(audio_sources());
}

void DelayController::disconnect_observers()
{
	sceneObserverSignals_.clear();
	audioObserverSignals_.clear();
}

void DelayController::refresh_audio_preflight()
{
	DelaySettings current;
	std::vector<obs_output_t *> outputs;
	{
		std::scoped_lock lock(mutex_);
		current = settings_;
		outputs.reserve(sessions_.size());
		for (const auto &[output, _] : sessions_)
			outputs.push_back(output);
	}
	obs_source_t *scene = selected_scene();
	obs_source_t *dedicatedSource = selected_audio_source();
	const AudioPreflightResult result = preflight_hold_audio(current, scene, dedicatedSource, outputs);
	std::shared_ptr<HoldMediaHub> currentHub;
	{
		std::scoped_lock lock(mutex_);
		if (activeMediaHub_ && activeMediaHub_->active() && activeMediaHub_.use_count() > 1)
			currentHub = activeMediaHub_;
	}

	bool mutedNow = false;
	bool stickyBackend = false;
	HoldAudioMode currentMode = HoldAudioMode::Silence;
	if (currentHub) {
		currentMode = currentHub->audio_mode();
		if (currentMode != HoldAudioMode::Silence) {
			DelaySettings backendSettings = current;
			backendSettings.holdAudioMode = currentMode;
			backendSettings.reservedAudioTrack =
				static_cast<uint32_t>(currentHub->reserved_mixer_index() + 1U);
			const AudioPreflightResult backendCheck =
				preflight_hold_audio(backendSettings, currentHub->hold_scene(),
						     currentHub->dedicated_audio_source(), outputs);
			if (backendCheck.effectiveMode != currentMode) {
				mutedNow = currentHub->force_audio_silence();
				currentMode = HoldAudioMode::Silence;
			}
		}
		stickyBackend = currentMode != result.effectiveMode;
	}

	QString publishedMessage = QString::fromStdString(result.message);
	if (mutedNow) {
		publishedMessage += QStringLiteral(" The current activation was muted to preserve video delay.");
	} else if (stickyBackend) {
		publishedMessage += QStringLiteral(
			" The current activation keeps its existing safe audio backend until delay is reactivated.");
	}

	obs_source_release(dedicatedSource);
	obs_source_release(scene);

	bool changed = false;
	{
		std::scoped_lock lock(mutex_);
		changed = audioPreflight_ != publishedMessage;
		audioPreflight_ = publishedMessage;
	}
	if (mutedNow) {
		obs_log(LOG_WARNING, "Hold audio safety conditions changed; audio muted without resetting video: %s",
			result.message.c_str());
		mark_dirty();
	}
	if (changed)
		emit audio_preflight_changed(publishedMessage);
}

void DelayController::load_settings()
{
	ensure_config_directory();
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (data) {
		settings_.delaySeconds =
			static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(data, "delay_seconds"), 1, 300));
		if (!obs_data_has_user_value(data, "delay_seconds"))
			settings_.delaySeconds = 30;
		settings_.holdSceneUuid = obs_data_get_string(data, "hold_scene_uuid");
		if (obs_data_has_user_value(data, "hold_audio_mode")) {
			switch (obs_data_get_int(data, "hold_audio_mode")) {
			case static_cast<int64_t>(HoldAudioMode::DedicatedSource):
				settings_.holdAudioMode = HoldAudioMode::DedicatedSource;
				break;
			case static_cast<int64_t>(HoldAudioMode::ReservedTrack):
				settings_.holdAudioMode = HoldAudioMode::ReservedTrack;
				break;
			case static_cast<int64_t>(HoldAudioMode::Silence):
				settings_.holdAudioMode = HoldAudioMode::Silence;
				break;
			default:
				settings_.holdAudioMode = HoldAudioMode::SceneMix;
				break;
			}
		}
		settings_.holdAudioSourceUuid = obs_data_get_string(data, "hold_audio_source_uuid");
		if (obs_data_has_user_value(data, "reserved_audio_track"))
			settings_.reservedAudioTrack = static_cast<uint32_t>(std::clamp<int64_t>(
				obs_data_get_int(data, "reserved_audio_track"), 1, MAX_AUDIO_MIXES));
		settings_.transition = TransitionStyle::Cut;
		settings_.previewExpanded = obs_data_get_bool(data, "preview_expanded");
		obs_data_release(data);
	}
	bfree(path);
}

void DelayController::save_settings() const
{
	ensure_config_directory();
	DelaySettings copy;
	{
		std::scoped_lock lock(mutex_);
		copy = settings_;
	}
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "delay_seconds", copy.delaySeconds);
	obs_data_set_string(data, "hold_scene_uuid", copy.holdSceneUuid.c_str());
	obs_data_set_int(data, "hold_audio_mode", static_cast<int>(copy.holdAudioMode));
	obs_data_set_string(data, "hold_audio_source_uuid", copy.holdAudioSourceUuid.c_str());
	obs_data_set_int(data, "reserved_audio_track", copy.reservedAudioTrack);
	obs_data_set_int(data, "transition", static_cast<int>(copy.transition));
	obs_data_set_bool(data, "preview_expanded", copy.previewExpanded);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

} // namespace dynamic_delay
