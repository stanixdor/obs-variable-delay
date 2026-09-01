#pragma once

#include "delay-types.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

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

struct SceneChoice {
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
	[[nodiscard]] bool requested_active() const noexcept { return requestedActive_; }

public slots:
	void toggle_delay();
	void set_delay_seconds(int seconds);
	void set_hold_scene(const QString &uuid);
	void set_transition_style(int style);
	void set_preview_expanded(bool expanded);

signals:
	void snapshot_changed(const dynamic_delay::DelaySnapshot &snapshot);
	void settings_changed(const dynamic_delay::DelaySettings &settings);
	void scenes_changed(const std::vector<dynamic_delay::SceneChoice> &scenes);

private slots:
	void poll();

private:
	static void frontend_event(enum obs_frontend_event event, void *privateData);
	void handle_frontend_event(enum obs_frontend_event event);
	void add_output(obs_output_t *output, const std::string &label);
	void remove_output(obs_output_t *output, const std::string &label);
	void attach_current_outputs();
	void start_delay_on_sessions();
	void start_delay_on_output(obs_output_t *output);
	void request_rearm();
	void refresh_scenes();
	void load_settings();
	void save_settings() const;
	obs_source_t *selected_scene() const;
	void mark_dirty() noexcept { dirty_.store(true, std::memory_order_relaxed); }

	mutable std::mutex mutex_;
	DelaySettings settings_;
	std::vector<SceneChoice> scenes_;
	std::unordered_map<obs_output_t *, std::unique_ptr<OutputSession>> sessions_;
	std::unordered_set<obs_output_t *> reconnectPending_;
	QTimer pollTimer_;
	std::atomic_bool dirty_{true};
	bool requestedActive_ = false;
	bool rearmPending_ = false;
	bool shuttingDown_ = false;
	std::string controllerError_;
	std::unordered_map<std::string, std::string> outputErrors_;
};

} // namespace dynamic_delay

Q_DECLARE_METATYPE(dynamic_delay::DelaySnapshot)
Q_DECLARE_METATYPE(dynamic_delay::DelaySettings)
Q_DECLARE_METATYPE(std::vector<dynamic_delay::SceneChoice>)
