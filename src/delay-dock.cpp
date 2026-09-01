#include "delay-dock.hpp"

#include "audience-preview.hpp"
#include "delay-controller.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace dynamic_delay {

DelayDock::DelayDock(DelayController &controller, QWidget *parent) : QWidget(parent), controller_(controller)
{
	build_ui();
	connect(&controller_, &DelayController::snapshot_changed, this, &DelayDock::apply_snapshot);
	connect(&controller_, &DelayController::settings_changed, this, &DelayDock::apply_settings);
	connect(&controller_, &DelayController::scenes_changed, this, [this] { rebuild_scenes(); });
	rebuild_scenes();
	apply_settings(controller_.settings());
	apply_snapshot(controller_.snapshot());
}

DelayDock::~DelayDock() = default;

void DelayDock::build_ui()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	auto *statusRow = new QHBoxLayout;
	statusDot_ = new QLabel(QStringLiteral("●"), this);
	statusDot_->setAccessibleName(tr("Delay status"));
	statusText_ = new QLabel(this);
	QFont statusFont = statusText_->font();
	statusFont.setBold(true);
	statusText_->setFont(statusFont);
	statusRow->addWidget(statusDot_);
	statusRow->addWidget(statusText_, 1);
	root->addLayout(statusRow);

	detailText_ = new QLabel(this);
	detailText_->setWordWrap(true);
	detailText_->setStyleSheet(QStringLiteral("color: palette(mid);"));
	root->addWidget(detailText_);

	auto *delayLabel = new QLabel(tr("Delay"), this);
	QFont sectionFont = delayLabel->font();
	sectionFont.setBold(true);
	delayLabel->setFont(sectionFont);
	root->addWidget(delayLabel);

	auto *delayRow = new QHBoxLayout;
	delaySlider_ = new QSlider(Qt::Horizontal, this);
	delaySlider_->setRange(1, 300);
	delaySlider_->setSingleStep(1);
	delaySlider_->setPageStep(5);
	delaySpin_ = new QSpinBox(this);
	delaySpin_->setRange(1, 300);
	delaySpin_->setSuffix(tr(" s"));
	delaySpin_->setMinimumWidth(82);
	delayRow->addWidget(delaySlider_, 1);
	delayRow->addWidget(delaySpin_);
	root->addLayout(delayRow);

	connect(delaySlider_, &QSlider::valueChanged, delaySpin_, &QSpinBox::setValue);
	connect(delaySpin_, qOverload<int>(&QSpinBox::valueChanged), delaySlider_, &QSlider::setValue);
	connect(delaySpin_, qOverload<int>(&QSpinBox::valueChanged), &controller_, &DelayController::set_delay_seconds);

	auto *form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	sceneCombo_ = new QComboBox(this);
	sceneCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	transitionCombo_ = new QComboBox(this);
	transitionCombo_->addItem(tr("Cut on keyframe"), static_cast<int>(TransitionStyle::Cut));
	transitionCombo_->addItem(tr("Fade through black (requires transcoding)"),
				  static_cast<int>(TransitionStyle::FadeThroughBlack));
	if (auto *model = qobject_cast<QStandardItemModel *>(transitionCombo_->model())) {
		if (QStandardItem *fade = model->item(1))
			fade->setFlags(fade->flags() & ~Qt::ItemIsEnabled);
	}
	transitionCombo_->setToolTip(
		tr("Cut is the low-resource production path. A real fade of already encoded delayed video requires "
		   "decode/composite/re-encode and is intentionally unavailable in this build."));
	form->addRow(tr("Hold scene"), sceneCombo_);
	form->addRow(tr("State transition"), transitionCombo_);
	root->addLayout(form);

	connect(sceneCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (!applying_ && index >= 0)
			controller_.set_hold_scene(sceneCombo_->itemData(index).toString());
	});
	connect(transitionCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (!applying_ && index >= 0)
			controller_.set_transition_style(transitionCombo_->itemData(index).toInt());
	});

	auto *memoryFrame = new QFrame(this);
	memoryFrame->setFrameShape(QFrame::StyledPanel);
	auto *memoryLayout = new QFormLayout(memoryFrame);
	memoryEstimate_ = new QLabel(this);
	memoryActual_ = new QLabel(this);
	memoryLayout->addRow(tr("Estimated RAM"), memoryEstimate_);
	memoryLayout->addRow(tr("Packet buffer now"), memoryActual_);
	root->addWidget(memoryFrame);

	progress_ = new QProgressBar(this);
	progress_->setRange(0, 1000);
	progress_->setTextVisible(true);
	root->addWidget(progress_);

	toggleButton_ = new QPushButton(this);
	toggleButton_->setMinimumHeight(36);
	connect(toggleButton_, &QPushButton::clicked, &controller_, &DelayController::toggle_delay);
	root->addWidget(toggleButton_);

	previewToggle_ = new QToolButton(this);
	previewToggle_->setText(tr("OBS output preview"));
	previewToggle_->setCheckable(true);
	previewToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	previewToggle_->setArrowType(Qt::RightArrow);
	connect(previewToggle_, &QToolButton::toggled, this, &DelayDock::set_preview_visible);
	root->addWidget(previewToggle_);

	preview_ = new AudiencePreviewWidget(controller_, this);
	preview_->setVisible(false);
	root->addWidget(preview_);
	root->addStretch(1);
}

QString DelayDock::bytes_text(const std::size_t bytes)
{
	constexpr double MiB = 1024.0 * 1024.0;
	constexpr double GiB = 1024.0 * MiB;
	if (bytes >= static_cast<std::size_t>(GiB))
		return tr("%1 GiB").arg(static_cast<double>(bytes) / GiB, 0, 'f', 2);
	return tr("%1 MiB").arg(static_cast<double>(bytes) / MiB, 0, 'f', 1);
}

QString DelayDock::state_text(const DelayState state)
{
	switch (state) {
	case DelayState::Bypass:
		return tr("LIVE");
	case DelayState::Preparing:
		return tr("PREPARING");
	case DelayState::Filling:
		return tr("ADDING DELAY");
	case DelayState::Delayed:
		return tr("DELAY ACTIVE");
	case DelayState::ReturningLive:
		return tr("RETURNING LIVE");
	case DelayState::Error:
		return tr("ERROR / LIVE FALLBACK");
	}
	return tr("UNKNOWN");
}

void DelayDock::apply_snapshot(const DelaySnapshot &snapshot)
{
	statusText_->setText(state_text(snapshot.state));
	detailText_->setText(QString::fromStdString(snapshot.detail));
	if (snapshot.estimateAvailable)
		memoryEstimate_->setText(bytes_text(snapshot.estimatedBytes));
	else if (snapshot.activeOutputs != 0)
		memoryEstimate_->setText(tr("Measuring live bitrate…"));
	else
		memoryEstimate_->setText(tr("Start an output to estimate"));
	memoryActual_->setText(bytes_text(snapshot.bufferedBytes));
	progress_->setValue(static_cast<int>(std::clamp(snapshot.progress, 0.0, 1.0) * 1000.0));
	progress_->setFormat(snapshot.state == DelayState::Filling ? tr("Buffering %p%") : state_text(snapshot.state));

	QString color = QStringLiteral("#36c56f");
	if (snapshot.state == DelayState::Preparing || snapshot.state == DelayState::Filling ||
	    snapshot.state == DelayState::ReturningLive)
		color = QStringLiteral("#f3b33d");
	else if (snapshot.state == DelayState::Delayed)
		color = QStringLiteral("#4b9cff");
	else if (snapshot.state == DelayState::Error)
		color = QStringLiteral("#ef5350");
	statusDot_->setStyleSheet(QStringLiteral("color: %1;").arg(color));

	const bool active = controller_.requested_active();
	toggleButton_->setText(active ? tr("Remove delay / cancel") : tr("Add delay"));
	toggleButton_->setProperty("delayActive", active);
	delaySlider_->setEnabled(snapshot.state == DelayState::Bypass || snapshot.state == DelayState::Delayed ||
				 snapshot.state == DelayState::Error);
	delaySpin_->setEnabled(delaySlider_->isEnabled());
	const bool configurationEnabled = snapshot.state == DelayState::Bypass ||
					  snapshot.state == DelayState::Delayed || snapshot.state == DelayState::Error;
	sceneCombo_->setEnabled(configurationEnabled);
	transitionCombo_->setEnabled(configurationEnabled);
}

void DelayDock::apply_settings(const DelaySettings &settings)
{
	applying_ = true;
	delaySlider_->setValue(static_cast<int>(settings.delaySeconds));
	delaySpin_->setValue(static_cast<int>(settings.delaySeconds));
	const int transitionIndex = transitionCombo_->findData(static_cast<int>(settings.transition));
	if (transitionIndex >= 0)
		transitionCombo_->setCurrentIndex(transitionIndex);
	const int sceneIndex = sceneCombo_->findData(QString::fromStdString(settings.holdSceneUuid));
	if (sceneIndex >= 0)
		sceneCombo_->setCurrentIndex(sceneIndex);
	previewToggle_->setChecked(settings.previewExpanded);
	applying_ = false;
}

void DelayDock::rebuild_scenes()
{
	const DelaySettings current = controller_.settings();
	applying_ = true;
	sceneCombo_->clear();
	for (const SceneChoice &scene : controller_.scenes())
		sceneCombo_->addItem(scene.name, scene.uuid);
	const int index = sceneCombo_->findData(QString::fromStdString(current.holdSceneUuid));
	if (index >= 0)
		sceneCombo_->setCurrentIndex(index);
	applying_ = false;
}

void DelayDock::set_preview_visible(const bool visible)
{
	previewToggle_->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
	preview_->setVisible(visible);
	preview_->set_enabled(visible);
	if (!applying_)
		controller_.set_preview_expanded(visible);
}

} // namespace dynamic_delay
