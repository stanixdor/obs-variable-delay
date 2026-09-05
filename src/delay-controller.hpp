#pragma once

#include "delay-types.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dynamic_delay {

class OutputSession;
class HoldMediaHub;

struct SceneChoice {
	QString name;
	QString uuid;
};

struct AudioSourceChoice {
	QString name;
	QString uuid;
};

class DelayController final : public QObject {
	Q_OBJECT

public:
	explicit DelayController(QObject *parent = nullptr);
	~DelayController() override;

	DelayController(const DelayController &) = delete;
	DelayController &operator=(const DelayController &) = delete;

	[[nodiscard]] DelaySettings settings() const;
	[[nodiscard]] DelaySnapshot snapshot() const;
	[[nodiscard]] std::vector<SceneChoice> scenes() const;
	[[nodiscard]] std::vector<AudioSourceChoice> audio_sources() const;
	[[nodiscard]] QString audio_preflight() const;
	[[nodiscard]] bool requested_active() const noexcept { return requestedActive_; }
	// Explicitly owned outputs only; never enumerate or alter third-party routes.
	bool add_managed_output(obs_output_t *output, std::string &error);
	void managed_output_started(obs_output_t *output);
	void remove_managed_output(obs_output_t *output);

public slots:
	void toggle_delay();
	void set_delay_seconds(int seconds);
	void set_hold_scene(const QString &uuid);
	void set_transition_style(int style);
	void set_hold_audio_mode(int mode);
	void set_hold_audio_source(const QString &uuid);
	void set_reserved_audio_track(int track);
	void set_preview_expanded(bool expanded);

signals:
	void about_to_shutdown();
	void snapshot_changed(const dynamic_delay::DelaySnapshot &snapshot);
	void settings_changed(const dynamic_delay::DelaySettings &settings);
	void scenes_changed(const std::vector<dynamic_delay::SceneChoice> &scenes);
	void audio_sources_changed(const std::vector<dynamic_delay::AudioSourceChoice> &sources);
	void audio_preflight_changed(const QString &message);

private slots:
	void poll();

private:
	static void frontend_event(enum obs_frontend_event event, void *privateData);
	static void source_event(void *privateData, calldata_t *data);
	static void preflight_event(void *privateData, calldata_t *data);
	void handle_frontend_event(enum obs_frontend_event event);
	void add_output(obs_output_t *output, const std::string &label);
	void remove_output(obs_output_t *output, const std::string &label);
	void attach_current_outputs();
	void start_delay_on_sessions();
	void start_delay_on_output(obs_output_t *output);
	void request_rearm();
	void refresh_scenes();
	void refresh_scene_observers();
	void refresh_audio_sources();
	void refresh_audio_preflight();
	void poll_reserved_audio_topology();
	void disconnect_observers();
	void shutdown();
	std::shared_ptr<HoldMediaHub> create_media_hub(obs_source_t *scene, std::string &error);
	void load_settings();
	void save_settings() const;
	obs_source_t *selected_scene() const;
	obs_source_t *selected_audio_source() const;
	void mark_dirty() noexcept { dirty_.store(true, std::memory_order_relaxed); }

	mutable std::mutex mutex_;
	DelaySettings settings_;
	std::vector<SceneChoice> scenes_;
	std::vector<AudioSourceChoice> audioSources_;
	std::vector<OBSSignal> audioObserverSignals_;
	std::vector<OBSSignal> sceneObserverSignals_;
	QString audioPreflight_;
	std::unordered_map<obs_output_t *, std::unique_ptr<OutputSession>> sessions_;
	std::shared_ptr<HoldMediaHub> activeMediaHub_;
	std::unordered_set<obs_output_t *> reconnectPending_;
	QTimer pollTimer_;
	std::atomic_bool dirty_{true};
	std::atomic_bool topologyRefreshQueued_{false};
	std::atomic_bool preflightRefreshQueued_{false};
	std::atomic_bool shuttingDown_{false};
	std::atomic_bool shutdownComplete_{false};
	bool requestedActive_ = false;
	bool rearmPending_ = false;
	bool frontendLoaded_ = false;
	bool reservedAudioFingerprintValid_ = false;
	uint64_t reservedAudioFingerprint_ = 0;
	std::string controllerError_;
	std::unordered_map<std::string, std::string> outputErrors_;
};

} // namespace dynamic_delay

Q_DECLARE_METATYPE(dynamic_delay::DelaySnapshot)
Q_DECLARE_METATYPE(dynamic_delay::DelaySettings)
Q_DECLARE_METATYPE(std::vector<dynamic_delay::SceneChoice>)
Q_DECLARE_METATYPE(std::vector<dynamic_delay::AudioSourceChoice>)
