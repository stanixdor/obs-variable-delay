#include "audience-preview.hpp"

#include "delay-controller.hpp"

#include <util/platform.h>

#include <QHideEvent>
#include <QLabel>
#include <QPixmap>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace dynamic_delay {

std::size_t audience_preview_estimated_bytes(const uint32_t delaySeconds) noexcept
{
	return static_cast<std::size_t>(PreviewCapture::Width) * PreviewCapture::Height * sizeof(uint16_t) * 2U *
	       (static_cast<std::size_t>(delaySeconds) + 3U);
}

AudiencePreviewWidget::AudiencePreviewWidget(DelayController &controller, QWidget *parent)
	: QWidget(parent),
	  controller_(controller)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 4, 0, 0);
	imageLabel_ = new QLabel(this);
	imageLabel_->setAlignment(Qt::AlignCenter);
	imageLabel_->setMinimumSize(PreviewCapture::Width, PreviewCapture::Height);
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
	connect(&controller_, &DelayController::settings_changed, this, [this] { update_history_limit(); });
	connect(&controller_, &DelayController::snapshot_changed, this, [this] { update_history_limit(); });
	update_history_limit();
}

AudiencePreviewWidget::~AudiencePreviewWidget()
{
	timer_->stop();
	capture_.stop();
}

bool AudiencePreviewWidget::start_capture()
{
	if (capture_.start())
		return true;
	imageLabel_->setText(tr("OBS video is not ready"));
	return false;
}

void AudiencePreviewWidget::set_enabled(const bool enabled)
{
	enabled_ = enabled;
	if (enabled) {
		if (start_capture() && isVisible())
			timer_->start();
	} else {
		timer_->stop();
		capture_.stop();
		lastPresentedKey_ = 0;
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Preview is off"));
	}
}

void AudiencePreviewWidget::hideEvent(QHideEvent *event)
{
	// Retain low-rate history for immediate reopening without painting an
	// invisible widget. Folding the preview itself frees all resources.
	timer_->stop();
	QWidget::hideEvent(event);
}

void AudiencePreviewWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (enabled_ && start_capture()) {
		timer_->start();
		present_frame();
	}
}

void AudiencePreviewWidget::update_history_limit()
{
	const DelaySnapshot snapshot = controller_.snapshot();
	if (!snapshot.outputTimings.empty()) {
		auto output = std::find_if(snapshot.outputTimings.begin(), snapshot.outputTimings.end(),
					   [](const auto &timing) { return timing.label == "Streaming"; });
		if (output == snapshot.outputTimings.end())
			output = snapshot.outputTimings.begin();
		capture_.set_history_seconds(
			core::preview_history_seconds(snapshot.configuredSeconds, output->effectiveSeconds));
		capture_.set_playback_state(output->paused, output->emittingHold, output->emittingDelayed,
					    output->emittedVideoTimestampNs, output->bufferStartTimestampNs);
	} else {
		capture_.set_history_seconds(
			core::preview_history_seconds(snapshot.configuredSeconds, snapshot.effectiveSeconds));
		capture_.set_playback_state(false, false, false, 0);
	}
}

void AudiencePreviewWidget::present_frame()
{
	if (!isVisible())
		return;
	const DelaySnapshot snapshot = controller_.snapshot();
	bool emittingHold = snapshot.emittingHold;
	bool emittingDelayed = snapshot.emittingDelayed;
	bool paused = snapshot.paused;
	double effectiveSeconds = snapshot.effectiveSeconds;
	uint64_t emittedAtNs = snapshot.emittedVideoTimestampNs;
	if (!snapshot.outputTimings.empty()) {
		auto output = std::find_if(snapshot.outputTimings.begin(), snapshot.outputTimings.end(),
					   [](const auto &timing) { return timing.label == "Streaming"; });
		if (output == snapshot.outputTimings.end())
			output = snapshot.outputTimings.begin();
		emittingHold = output->emittingHold;
		emittingDelayed = output->emittingDelayed;
		paused = output->paused;
		effectiveSeconds = output->effectiveSeconds;
		emittedAtNs = output->emittedVideoTimestampNs;
	}
	if (emittingHold) {
		lastPresentedKey_ = 0;
		imageLabel_->setPixmap({});
		imageLabel_->setText(tr("Audience is seeing the selected hold scene"));
		return;
	}

	const uint64_t now = os_gettime_ns();
	// Use the original capture timestamp of the video actually emitted, not
	// the slider. This also freezes a paused recording at the right frame.
	const uint64_t target =
		emittedAtNs != 0 ? emittedAtNs : core::preview_target_ns(now, emittingDelayed ? effectiveSeconds : 0.0);
	const PreviewCapture::Frame selected = capture_.frame_at(target);
	if (selected.image.isNull()) {
		// A paused frame may age out of history while remaining displayed.
		if (lastPresentedKey_ != 0 && emittedAtNs != 0 && paused)
			return;
		lastPresentedKey_ = 0;
		imageLabel_->setPixmap({});
		if (paused) {
			imageLabel_->setText(tr("Recording is paused; preview history resumes with recording."));
		} else if (emittingDelayed) {
			const uint64_t startedAt = capture_.started_at_ns();
			const uint64_t elapsed = startedAt != 0 && now > startedAt ? now - startedAt : 0;
			const uint64_t requiredNs = core::preview_delay_ns(effectiveSeconds);
			const auto remaining = static_cast<uint32_t>((requiredNs > elapsed ? requiredNs - elapsed : 0) /
								     1'000'000'000ULL);
			imageLabel_->setText(tr("Building accurate preview history… %1 s").arg(remaining));
		} else {
			imageLabel_->setText(tr("Collecting preview frames…"));
		}
		return;
	}
	if (lastPresentedKey_ == selected.image.cacheKey() && lastPresentedSize_ == imageLabel_->size())
		return;
	lastPresentedKey_ = selected.image.cacheKey();
	lastPresentedSize_ = imageLabel_->size();
	imageLabel_->setText({});
	imageLabel_->setPixmap(QPixmap::fromImage(selected.image)
				       .scaled(imageLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace dynamic_delay
