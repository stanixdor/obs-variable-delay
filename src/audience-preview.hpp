#pragma once

#include <obs.h>

#include <QImage>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

class QLabel;
class QTimer;

namespace dynamic_delay {

class DelayController;

[[nodiscard]] std::size_t audience_preview_estimated_bytes(uint32_t delaySeconds) noexcept;

class AudiencePreviewWidget final : public QWidget {
	Q_OBJECT

public:
	explicit AudiencePreviewWidget(DelayController &controller, QWidget *parent = nullptr);
	~AudiencePreviewWidget() override;

	void set_enabled(bool enabled);

private slots:
	void present_frame();

private:
	struct PreviewFrame {
		uint64_t capturedAtNs = 0;
		QImage image;
	};

	static void video_frame(void *param, video_data *frame);
	void receive_frame(video_data *frame);
	bool start_capture();
	void stop_capture();

	DelayController &controller_;
	QLabel *imageLabel_ = nullptr;
	QLabel *captionLabel_ = nullptr;
	QTimer *timer_ = nullptr;
	std::mutex framesMutex_;
	std::deque<PreviewFrame> frames_;
	std::atomic<uint32_t> delaySeconds_{30};
	uint64_t captureStartedNs_ = 0;
	bool connected_ = false;
};

} // namespace dynamic_delay
