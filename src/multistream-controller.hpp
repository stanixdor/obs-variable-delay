#pragma once

#include "multistream-transport.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace dynamic_delay {

class DelayController;

// Owns a network-independent Program collector. All configured destinations
// consume the final packets of this ONE OutputSession, never raw live packets.
class MultistreamController final : public QObject {
	Q_OBJECT
public:
	explicit MultistreamController(DelayController &delay, QObject *parent = nullptr);
	~MultistreamController() override;
	[[nodiscard]] const std::vector<MultistreamTarget> &targets() const noexcept { return targets_; }
	[[nodiscard]] std::vector<MultistreamStatus> statuses() const;
	[[nodiscard]] bool running(const std::string &id) const;
	bool save_target(MultistreamTarget target, QString &error);
	bool remove_target(const std::string &id, QString &error);
	bool start_target(const std::string &id, QString &error);
	void stop_target(const std::string &id);
	void stop_all();
	void receive(encoder_packet *packet) noexcept;
	static void register_output_type();

signals:
	void changed();

private:
	static void frontend_event(enum obs_frontend_event event, void *data);
	bool create_master(QString &error);
	void destroy_master();
	bool describe_stream(QString &error);
	void load_settings();
	bool save_settings(QString &error) const;
	void poll();

	DelayController &delay_;
	std::vector<MultistreamTarget> targets_;
	std::unique_ptr<MultistreamTransport> transport_;
	StreamDescription description_;
	obs_output_t *master_ = nullptr;
	QTimer timer_;
	QElapsedTimer startupTimer_;
	std::unordered_set<std::string> pendingStarts_;
	std::unordered_map<std::string, std::string> errors_;
	std::atomic_bool captureFailed_{false};
	bool shuttingDown_ = false;
};

} // namespace dynamic_delay
