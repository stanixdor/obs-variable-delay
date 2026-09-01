#include "audience-preview.hpp"

#include "delay-controller.hpp"
#include "plugin-support.h"

#include <util/platform.h>

#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace dynamic_delay {
namespace {

constexpr uint32_t PreviewWidth = 320;
constexpr uint32_t PreviewHeight = 180;
constexpr uint32_t PreviewFps = 2;
constexpr uint32_t PreviewSafetySeconds = 3;
constexpr uint64_t NsPerSecond = 1'000'000'000ULL;

} // namespace

std::size_t audience_preview_estimated_bytes(const uint32_t delaySeconds) noexcept
{
	return static_cast<std::size_t>(PreviewWidth) * PreviewHeight * sizeof(uint16_t) * PreviewFps *
	       (static_cast<std::size_t>(delaySeconds) + PreviewSafetySeconds);
}

AudiencePreviewWidget::AudiencePreviewWidget(DelayController &controller, QWidget *parent)
	: QWidget(parent),
	  controller_(controller)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 4, 0, 0);
	imageLabel_ = new QLabel(this);
	imageLabel_->setAlignment(Qt::AlignCenter);
	imageLabel_->setMinimumSize(PreviewWidth, PreviewHeight);
	imageLabel_->setStyleSheet(QStringLiteral("background: #111; border: 1px solid palette(mid);"));
	imageLabel_->setText(tr("Preview is off"));
	captionLabel_ = new QLabel(tr("Approximate packets sent by OBS; CDN/player latency is not included."), this);
	captionLabel_->setWordWrap(true);
	captionLabel_->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));
	layout->addWidget(imageLabel_);
	layout->addWidget(captionLabel_);

	timer_ = new QTimer(this);
	timer_->setInterval(200);
	connect(timer_, &QTimer::timeout, this, &AudiencePreviewWidget::present_frame);
	connect(&controller_, &DelayController::settings_changed, this, [this](const DelaySettings &settings) {
		delaySeconds_.store(settings.delaySeconds, std::memory_order_relaxed);
	});
	delaySeconds_.store(controller_.settings().delaySeconds, std::memory_order_relaxed);
}

AudiencePreviewWidget::~AudiencePreviewWidget()
{
	stop_capture();
}

void AudiencePreviewWidget::set_enabled(const bool enabled)
{
	if (enabled) {
		if (start_capture())
			timer_->start();
	} else {
		timer_->stop();
		stop_capture();
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Preview is off"));
	}
}

bool AudiencePreviewWidget::start_capture()
{
	if (connected_)
		return true;
	obs_video_info info{};
	if (!obs_get_video_info(&info)) {
		imageLabel_->setText(tr("OBS video is not ready"));
		return false;
	}

	video_scale_info conversion{};
	conversion.format = VIDEO_FORMAT_BGRA;
	conversion.width = PreviewWidth;
	conversion.height = PreviewHeight;
	conversion.range = VIDEO_RANGE_FULL;
	conversion.colorspace = VIDEO_CS_709;
	const double sourceFps = static_cast<double>(info.fps_num) / std::max<uint32_t>(1, info.fps_den);
	const uint32_t divisor = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(sourceFps / PreviewFps)));
	obs_add_raw_video_callback2(&conversion, divisor, &AudiencePreviewWidget::video_frame, this);
	connected_ = true;
	captureStartedNs_ = os_gettime_ns();
	return true;
}

void AudiencePreviewWidget::stop_capture()
{
	if (connected_)
		obs_remove_raw_video_callback(&AudiencePreviewWidget::video_frame, this);
	connected_ = false;
	captureStartedNs_ = 0;
	std::scoped_lock lock(framesMutex_);
	frames_.clear();
}

void AudiencePreviewWidget::video_frame(void *param, video_data *frame)
{
	if (param && frame)
		static_cast<AudiencePreviewWidget *>(param)->receive_frame(frame);
}

void AudiencePreviewWidget::receive_frame(video_data *frame)
{
	if (!frame->data[0])
		return;
	QImage wrapped(frame->data[0], PreviewWidth, PreviewHeight, static_cast<qsizetype>(frame->linesize[0]),
		       QImage::Format_ARGB32);
	PreviewFrame saved{os_gettime_ns(), wrapped.convertToFormat(QImage::Format_RGB16)};
	const uint64_t keepNs =
		(static_cast<uint64_t>(delaySeconds_.load(std::memory_order_relaxed)) + PreviewSafetySeconds) *
		NsPerSecond;
	std::scoped_lock lock(framesMutex_);
	frames_.push_back(std::move(saved));
	const uint64_t cutoff = frames_.back().capturedAtNs > keepNs ? frames_.back().capturedAtNs - keepNs : 0;
	while (!frames_.empty() && frames_.front().capturedAtNs < cutoff)
		frames_.pop_front();
}

void AudiencePreviewWidget::present_frame()
{
	const DelaySnapshot snapshot = controller_.snapshot();
	if (snapshot.emittingHold) {
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Audience is seeing the selected hold scene"));
		return;
	}

	const uint64_t now = os_gettime_ns();
	const uint64_t target =
		snapshot.emittingDelayed
			? now - std::min<uint64_t>(now, static_cast<uint64_t>(snapshot.configuredSeconds) * NsPerSecond)
			: now;
	QImage selected;
	bool hasRequiredHistory = !snapshot.emittingDelayed;
	{
		std::scoped_lock lock(framesMutex_);
		for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
			if (it->capturedAtNs <= target) {
				selected = it->image;
				hasRequiredHistory = true;
				break;
			}
		}
		if (!snapshot.emittingDelayed && selected.isNull() && !frames_.empty())
			selected = frames_.front().image;
	}
	if (!hasRequiredHistory) {
		const uint64_t elapsed = captureStartedNs_ != 0 && now > captureStartedNs_ ? now - captureStartedNs_
											   : 0;
		const uint32_t remaining =
			snapshot.configuredSeconds > elapsed / NsPerSecond
				? snapshot.configuredSeconds - static_cast<uint32_t>(elapsed / NsPerSecond)
				: 0;
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Building accurate preview history… %1 s").arg(remaining));
		return;
	}
	if (selected.isNull()) {
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Collecting preview frames…"));
		return;
	}
	imageLabel_->setText({});
	imageLabel_->setPixmap(QPixmap::fromImage(selected).scaled(imageLabel_->size(), Qt::KeepAspectRatio,
								   Qt::SmoothTransformation));
}

} // namespace dynamic_delay
