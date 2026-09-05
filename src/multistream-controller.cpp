#include "multistream-controller.hpp"

#include "delay-controller.hpp"
#include "multistream-encoders.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dynamic_delay {
namespace {
constexpr const char *MasterId = "obs_dynamic_delay_multistream_master";
constexpr std::size_t MaxTargets = 8;

struct MasterSink {
	MultistreamController *owner = nullptr;
	obs_output_t *output = nullptr;
};

QString configuration_path()
{
	char *path = obs_module_config_path("multistream.json");
	QString result = QString::fromUtf8(path ? path : "");
	bfree(path);
	return result;
}

bool valid_target(const MultistreamTarget &target, QString &error)
{
	const QString server = QString::fromStdString(target.server);
	const QUrl url(server, QUrl::StrictMode);
	if (target.name.empty() || target.name.size() > 256 || target.server.size() > 4096 || target.key.empty() ||
	    target.key.size() > 4096) {
		error = obs_module_text("Multistream.Error.Fields");
		return false;
	}
	for (const auto *value : {&target.server, &target.key}) {
		if (std::any_of(value->begin(), value->end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
			error = obs_module_text("Multistream.Error.URL");
			return false;
		}
	}
	if (!url.isValid() || (url.scheme() != "rtmp" && url.scheme() != "rtmps") || url.host().isEmpty() ||
	    !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() || url.path().isEmpty()) {
		error = obs_module_text("Multistream.Error.URL");
		return false;
	}
	return true;
}

const char *master_name(void *)
{
	return "Dynamic Delay shared multistream collector";
}

void *master_create(obs_data_t *settings, obs_output_t *output)
{
	return new MasterSink{
		reinterpret_cast<MultistreamController *>(static_cast<uintptr_t>(obs_data_get_int(settings, "owner"))),
		output};
}

void master_destroy(void *data)
{
	delete static_cast<MasterSink *>(data);
}

bool master_start(void *data)
{
	auto *sink = static_cast<MasterSink *>(data);
	return sink && obs_output_can_begin_data_capture(sink->output, 0) &&
	       obs_output_initialize_encoders(sink->output, 0) && obs_output_begin_data_capture(sink->output, 0);
}

void master_stop(void *data, uint64_t)
{
	auto *sink = static_cast<MasterSink *>(data);
	if (sink)
		obs_output_end_data_capture(sink->output);
}

void master_packet(void *data, encoder_packet *packet)
{
	auto *sink = static_cast<MasterSink *>(data);
	if (sink && sink->owner)
		sink->owner->receive(packet);
}
} // namespace

void MultistreamController::register_output_type()
{
	obs_output_info info{};
	info.id = MasterId;
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	info.get_name = master_name;
	info.create = master_create;
	info.destroy = master_destroy;
	info.start = master_start;
	info.stop = master_stop;
	info.encoded_packet = master_packet;
	info.encoded_video_codecs = "h264";
	info.encoded_audio_codecs = "aac";
	obs_register_output(&info);
}

MultistreamController::MultistreamController(DelayController &delay, QObject *parent)
	: QObject(parent),
	  delay_(delay),
	  transport_(std::make_unique<MultistreamTransport>())
{
	load_settings();
	connect(&delay_, &DelayController::about_to_shutdown, this, [this] {
		shuttingDown_ = true;
		stop_all();
	});
	obs_frontend_add_event_callback(frontend_event, this);
	connect(&timer_, &QTimer::timeout, this, &MultistreamController::poll);
	timer_.start(100);
}

MultistreamController::~MultistreamController()
{
	shuttingDown_ = true;
	timer_.stop();
	obs_frontend_remove_event_callback(frontend_event, this);
	stop_all();
}

void MultistreamController::frontend_event(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<MultistreamController *>(data);
	if (event != OBS_FRONTEND_EVENT_EXIT && event != OBS_FRONTEND_EVENT_PROFILE_CHANGING &&
	    event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING)
		return;
	// Frontend collection/profile changes must not leave an old Program graph
	// transmitting. A new session always requires an explicit Start action.
	self->stop_all();
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		self->shuttingDown_ = true;
		self->timer_.stop();
	}
}

std::vector<MultistreamStatus> MultistreamController::statuses() const
{
	auto result = transport_->snapshot();
	std::erase_if(result, [this](const auto &status) {
		return std::none_of(targets_.begin(), targets_.end(), [&](const auto &t) { return t.id == status.id; });
	});
	for (const auto &target : targets_) {
		auto found =
			std::find_if(result.begin(), result.end(), [&](const auto &s) { return s.id == target.id; });
		if (found == result.end()) {
			MultistreamStatus status{};
			status.id = target.id;
			status.name = target.name;
			status.state = MultistreamState::Stopped;
			result.push_back(std::move(status));
			found = std::prev(result.end());
		}
		found->name = target.name;
		if (pendingStarts_.contains(target.id))
			found->state = MultistreamState::Connecting;
		if (const auto error = errors_.find(target.id); error != errors_.end()) {
			found->state = MultistreamState::Error;
			found->detail = error->second;
		}
	}
	return result;
}

bool MultistreamController::running(const std::string &id) const
{
	if (pendingStarts_.contains(id))
		return true;
	const auto statuses = transport_->snapshot();
	return std::any_of(statuses.begin(), statuses.end(), [&](const auto &s) {
		return s.id == id && s.state != MultistreamState::Stopped && s.state != MultistreamState::Error;
	});
}

bool MultistreamController::save_target(MultistreamTarget target, QString &error)
{
	target.name = QString::fromStdString(target.name).trimmed().toStdString();
	target.server = QString::fromStdString(target.server).trimmed().toStdString();
	if (!valid_target(target, error))
		return false;
	if (target.id.empty())
		target.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	if (running(target.id)) {
		error = obs_module_text("Multistream.Error.StopFirst");
		return false;
	}
	const auto before = targets_;
	auto it = std::find_if(targets_.begin(), targets_.end(), [&](const auto &t) { return t.id == target.id; });
	if (it == targets_.end()) {
		if (targets_.size() >= MaxTargets) {
			error = obs_module_text("Multistream.Error.Limit");
			return false;
		}
		targets_.push_back(std::move(target));
	} else {
		*it = std::move(target);
	}
	if (!save_settings(error)) {
		targets_ = before;
		return false;
	}
	emit changed();
	return true;
}

bool MultistreamController::remove_target(const std::string &id, QString &error)
{
	if (running(id)) {
		error = obs_module_text("Multistream.Error.StopFirst");
		return false;
	}
	const auto before = targets_;
	std::erase_if(targets_, [&](const auto &t) { return t.id == id; });
	if (!save_settings(error)) {
		targets_ = before;
		return false;
	}
	transport_->stop(id);
	emit changed();
	return true;
}

bool MultistreamController::create_master(QString &error)
{
	if (master_)
		return true;
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "owner", static_cast<long long>(reinterpret_cast<uintptr_t>(this)));
	master_ = obs_output_create(MasterId, "Dynamic Delay Multistream", settings, nullptr);
	obs_data_release(settings);
	std::string reason;
	if (!master_ || !configure_multistream_encoders(master_, reason) ||
	    !delay_.add_managed_output(master_, reason) || !obs_output_start(master_)) {
		error = reason.empty() ? QString::fromUtf8(obs_module_text("Multistream.Error.Capture"))
				       : QString::fromStdString(reason);
		destroy_master();
		return false;
	}
	startupTimer_.start();
	delay_.managed_output_started(master_);
	return true;
}

bool MultistreamController::describe_stream(QString &error)
{
	obs_encoder_t *video = obs_output_get_video_encoder(master_);
	obs_encoder_t *audio = obs_output_get_audio_encoder(master_, 0);
	uint8_t *extra = nullptr;
	size_t size = 0;
	if (!video || !audio || !obs_encoder_get_extra_data(video, &extra, &size) || !size || size > 1'048'576) {
		error = obs_module_text("Multistream.Error.Headers");
		return false;
	}
	description_.videoExtraData.assign(extra, extra + size);
	if (!obs_encoder_get_extra_data(audio, &extra, &size) || !size || size > 1'048'576) {
		error = obs_module_text("Multistream.Error.Headers");
		return false;
	}
	description_.audioExtraData.assign(extra, extra + size);
	description_.width = obs_encoder_get_width(video);
	description_.height = obs_encoder_get_height(video);
	description_.sampleRate = audio_output_get_sample_rate(obs_encoder_audio(audio));
	description_.channels = static_cast<uint32_t>(audio_output_get_channels(obs_encoder_audio(audio)));
	return true;
}

bool MultistreamController::start_target(const std::string &id, QString &error)
{
	if (shuttingDown_)
		return false;
	const auto it = std::find_if(targets_.begin(), targets_.end(), [&](const auto &t) { return t.id == id; });
	if (it == targets_.end() || !valid_target(*it, error))
		return false;
	if (!create_master(error))
		return false;
	errors_.erase(id);
	QString metadataError;
	if ((description_.videoExtraData.empty() || description_.audioExtraData.empty()) &&
	    !describe_stream(metadataError)) {
		pendingStarts_.insert(id);
		emit changed();
		return true;
	}
	std::string reason;
	if (!transport_->start(*it, description_, reason)) {
		error = QString::fromStdString(reason);
		if (!transport_->has_targets())
			destroy_master();
		return false;
	}
	emit changed();
	return true;
}

void MultistreamController::stop_target(const std::string &id)
{
	pendingStarts_.erase(id);
	errors_.erase(id);
	transport_->stop(id);
	if (!transport_->has_targets() && pendingStarts_.empty())
		destroy_master();
	emit changed();
}

void MultistreamController::stop_all()
{
	pendingStarts_.clear();
	transport_->stop_all();
	destroy_master();
	emit changed();
}

void MultistreamController::destroy_master()
{
	if (!master_)
		return;
	if (obs_output_active(master_))
		obs_output_force_stop(master_);
	delay_.remove_managed_output(master_);
	obs_output_release(master_);
	master_ = nullptr;
	description_ = {};
	captureFailed_.store(false, std::memory_order_release);
}

void MultistreamController::receive(encoder_packet *packet) noexcept
{
	if (!packet) {
		captureFailed_.store(true, std::memory_order_release);
		return;
	}
	if (captureFailed_.load(std::memory_order_acquire))
		return;
	if (!transport_->has_targets() || !packet->data || packet->size == 0 ||
	    (packet->type == OBS_ENCODER_AUDIO && packet->track_idx != 0))
		return;
	if (packet->size > 16 * 1024 * 1024 || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
		captureFailed_.store(true, std::memory_order_release);
		return;
	}
	try {
		SharedEncodedPacket finalPacket;
		finalPacket.data =
			std::make_shared<const std::vector<uint8_t>>(packet->data, packet->data + packet->size);
		finalPacket.video = packet->type == OBS_ENCODER_VIDEO;
		finalPacket.keyframe = packet->keyframe;
		// OBS advances packet timestamps by timebase_num already (1001 at
		// 29.97 fps). Multiplying by that numerator again corrupts A/V timing.
		const AVRational sourceTime{1, packet->timebase_den};
		finalPacket.ptsUsec = av_rescale_q(packet->pts, sourceTime, AVRational{1, 1'000'000});
		finalPacket.dtsUsec = av_rescale_q(packet->dts, sourceTime, AVRational{1, 1'000'000});
		transport_->submit(finalPacket);
	} catch (...) {
		// No exception may escape into OBS's encoder thread. Stop all network
		// destinations rather than accidentally forwarding a different signal.
		captureFailed_.store(true, std::memory_order_release);
	}
}

void MultistreamController::poll()
{
	if (captureFailed_.exchange(false, std::memory_order_acq_rel)) {
		stop_all();
		obs_log(LOG_ERROR, "Multistream capture failed; destinations stopped.");
	}
	if (master_ && !pendingStarts_.empty()) {
		QString reason;
		const bool ready = describe_stream(reason);
		if (ready || startupTimer_.elapsed() > 10'000) {
			const auto pending = std::exchange(pendingStarts_, {});
			for (const auto &id : pending) {
				QString error = reason;
				if (!ready || !start_target(id, error))
					errors_[id] = error.toStdString();
			}
			if (!transport_->has_targets())
				destroy_master();
		}
	}
	emit changed();
}

void MultistreamController::load_settings()
{
	QFile file(configuration_path());
	if (!file.open(QIODevice::ReadOnly) || file.size() > 131'072)
		return;
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &entry : document.object().value("targets").toArray()) {
		const auto obj = entry.toObject();
		MultistreamTarget target{obj.value("id").toString().toStdString(),
					 obj.value("name").toString().toStdString(),
					 obj.value("server").toString().toStdString(),
					 obj.value("key").toString().toStdString()};
		QString error;
		if (target.id.empty() || targets_.size() >= MaxTargets || !valid_target(target, error) ||
		    std::any_of(targets_.begin(), targets_.end(), [&](const auto &t) { return t.id == target.id; }))
			continue;
		targets_.push_back(std::move(target));
	}
	// Deliberately never restore running state or auto-publish at OBS launch.
}

bool MultistreamController::save_settings(QString &error) const
{
	const auto path = configuration_path();
	if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
		error = obs_module_text("Multistream.Error.Save");
		return false;
	}
	QJsonArray targets;
	for (const auto &target : targets_) {
		targets.append(QJsonObject{{"id", QString::fromStdString(target.id)},
					   {"name", QString::fromStdString(target.name)},
					   {"server", QString::fromStdString(target.server)},
					   {"key", QString::fromStdString(target.key)}});
	}
	QSaveFile file(path);
	const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"targets", targets}}).toJson();
	if (!file.open(QIODevice::WriteOnly) ||
	    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
	    file.write(bytes) != bytes.size() || !file.commit()) {
		error = obs_module_text("Multistream.Error.Save");
		return false;
	}
	return true;
}

} // namespace dynamic_delay
