#pragma once

#include "delay-types.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;

namespace dynamic_delay {

class AudiencePreviewWidget;
class DelayController;

class DelayDock final : public QWidget {
	Q_OBJECT

public:
	explicit DelayDock(DelayController &controller, QWidget *parent = nullptr);
	~DelayDock() override;

private slots:
	void apply_snapshot(const dynamic_delay::DelaySnapshot &snapshot);
	void apply_settings(const dynamic_delay::DelaySettings &settings);
	void rebuild_scenes();
	void rebuild_audio_sources();
	void set_preview_visible(bool visible);

private:
	static QString bytes_text(std::size_t bytes);
	static QString state_text(DelayState state);
	void build_ui();
	void update_audio_controls();

	DelayController &controller_;
	QLabel *statusDot_ = nullptr;
	QLabel *statusText_ = nullptr;
	QLabel *detailText_ = nullptr;
	QSlider *delaySlider_ = nullptr;
	QSpinBox *delaySpin_ = nullptr;
	QLabel *memoryEstimate_ = nullptr;
	QLabel *memoryActual_ = nullptr;
	QComboBox *transitionCombo_ = nullptr;
	QComboBox *sceneCombo_ = nullptr;
	QComboBox *audioModeCombo_ = nullptr;
	QComboBox *audioSourceCombo_ = nullptr;
	QComboBox *reservedTrackCombo_ = nullptr;
	QLabel *audioSourceLabel_ = nullptr;
	QLabel *reservedTrackLabel_ = nullptr;
	QLabel *audioWarning_ = nullptr;
	QPushButton *toggleButton_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QToolButton *previewToggle_ = nullptr;
	AudiencePreviewWidget *preview_ = nullptr;
	bool applying_ = false;
	bool configurationEnabled_ = true;
	int lastStatusColor_ = -1;
};

} // namespace dynamic_delay
