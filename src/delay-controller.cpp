#include "delay-controller.hpp"

#include "audience-preview.hpp"
#include "output-session.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QMetaType>

#include <algorithm>
#include <utility>

namespace dynamic_delay {
namespace {

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
	load_settings();
	refresh_scenes();
	obs_frontend_add_event_callback(&DelayController::frontend_event, this);
	connect(&pollTimer_, &QTimer::timeout, this, &DelayController::poll);
	pollTimer_.start(100);
	attach_current_outputs();
}

DelayController::~DelayController()
{
	shuttingDown_ = true;
	pollTimer_.stop();
	obs_frontend_remove_event_callback(&DelayController::frontend_event, this);
	std::scoped_lock lock(mutex_);
	sessions_.clear();
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

void DelayController::handle_frontend_event(const enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED: {
		obs_output_t *output = obs_frontend_get_streaming_output();
		add_output(output, "Streaming");
		obs_output_release(output);
		if (requestedActive_)
			start_delay_on_sessions();
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STARTED: {
		obs_output_t *output = obs_frontend_get_recording_output();
		add_output(output, "Recording");
		obs_output_release(output);
		if (requestedActive_)
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
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		obs_output_t *output = obs_frontend_get_recording_output();
		remove_output(output, "Recording");
		obs_output_release(output);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		refresh_scenes();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		shuttingDown_ = true;
		break;
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
	obs_source_t *scene = selected_scene();
	if (!scene) {
		std::scoped_lock lock(mutex_);
		controllerError_ = "Select a valid hold scene before adding delay.";
		requestedActive_ = false;
		mark_dirty();
		return;
	}

	{
		std::scoped_lock lock(mutex_);
		auto it = sessions_.find(output);
		if (requestedActive_ && it != sessions_.end() && obs_output_active(output) && it->second->is_bypass()) {
			std::string error;
			if (!it->second->request_delay(settings_.delaySeconds, scene, error)) {
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
	return obs_get_source_by_uuid(uuid.c_str());
}

void DelayController::start_delay_on_sessions()
{
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

	uint32_t seconds = 0;
	{
		std::scoped_lock lock(mutex_);
		seconds = settings_.delaySeconds;
		controllerError_.clear();
		for (auto &[_, session] : sessions_) {
			if (!obs_output_active(session->output()) || !session->is_bypass())
				continue;
			std::string error;
			if (!session->request_delay(seconds, scene, error)) {
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
	bool shouldRearm = false;
	std::vector<obs_output_t *> reconnectReady;
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
	}
	if (shouldRearm)
		start_delay_on_sessions();
	for (obs_output_t *output : reconnectReady)
		start_delay_on_output(output);
	if (dirty_.exchange(false, std::memory_order_relaxed) || requestedActive_)
		emit snapshot_changed(snapshot());
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
		if (!selectedStillExists) {
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
	obs_data_set_int(data, "transition", static_cast<int>(copy.transition));
	obs_data_set_bool(data, "preview_expanded", copy.previewExpanded);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

} // namespace dynamic_delay
