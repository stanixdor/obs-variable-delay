#pragma once

#include "preview-capture.hpp"

#include <QWidget>

#include <cstddef>
#include <cstdint>

class QLabel;
class QTimer;
class QHideEvent;
class QShowEvent;

namespace dynamic_delay {

class DelayController;

[[nodiscard]] std::size_t audience_preview_estimated_bytes(uint32_t delaySeconds) noexcept;

class AudiencePreviewWidget final : public QWidget {
	Q_OBJECT

public:
	explicit AudiencePreviewWidget(DelayController &controller, QWidget *parent = nullptr);
	~AudiencePreviewWidget() override;
	void set_enabled(bool enabled);

protected:
	void hideEvent(QHideEvent *event) override;
	void showEvent(QShowEvent *event) override;

private slots:
	void present_frame();

private:
	void update_history_limit();
	bool start_capture();
	DelayController &controller_;
	QLabel *imageLabel_ = nullptr;
	QLabel *captionLabel_ = nullptr;
	QTimer *timer_ = nullptr;
	PreviewCapture capture_;
	qint64 lastPresentedKey_ = 0;
	QSize lastPresentedSize_;
	bool enabled_ = false;
};

} // namespace dynamic_delay
