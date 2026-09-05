#include "multistream-dock.hpp"

#include "multistream-controller.hpp"
#include "multistream-transport.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace dynamic_delay {
namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void set_text(QLabel *label, const QString &value)
{
	if (label->text() != value)
		label->setText(value);
}

QString server_display(const std::string &server)
{
	// Ingest URLs can contain credentials in the path or query. The list
	// only needs to identify the host; the full URL is shown when editing.
	const QUrl url(QString::fromStdString(server), QUrl::StrictMode);
	if (!url.isValid() || url.host().isEmpty())
		return text("Multistream.InvalidServer");
	QUrl display;
	display.setScheme(url.scheme());
	display.setHost(url.host());
	display.setPort(url.port());
	return display.toDisplayString();
}

QString state_text(const MultistreamState state)
{
	switch (state) {
	case MultistreamState::Connecting:
		return text("Multistream.State.Connecting");
	case MultistreamState::WaitingForKeyframe:
		return text("Multistream.State.WaitingForKeyframe");
	case MultistreamState::Streaming:
		return text("Multistream.State.Streaming");
	case MultistreamState::Reconnecting:
		return text("Multistream.State.Reconnecting");
	case MultistreamState::Stopped:
		return text("Multistream.State.Stopped");
	case MultistreamState::Error:
		return text("Multistream.State.Error");
	}
	return text("Multistream.State.Stopped");
}

QString state_color(const MultistreamState state, const bool darkBackground)
{
	switch (state) {
	case MultistreamState::Streaming:
		return darkBackground ? QStringLiteral("#36c56f") : QStringLiteral("#18773d");
	case MultistreamState::Connecting:
	case MultistreamState::WaitingForKeyframe:
	case MultistreamState::Reconnecting:
		return darkBackground ? QStringLiteral("#f3b33d") : QStringLiteral("#966300");
	case MultistreamState::Error:
		return darkBackground ? QStringLiteral("#ef5350") : QStringLiteral("#ba2323");
	case MultistreamState::Stopped:
		return QStringLiteral("palette(text)");
	}
	return QStringLiteral("palette(text)");
}

class TargetEditor final : public QDialog {
public:
	TargetEditor(const MultistreamTarget &target, QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(text(target.id.empty() ? "Multistream.AddTitle" : "Multistream.EditTitle"));
		setModal(true);
		setMinimumWidth(440);
		auto *root = new QVBoxLayout(this);
		auto *form = new QFormLayout;
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		name_ = new QLineEdit(QString::fromStdString(target.name), this);
		name_->setObjectName(QStringLiteral("multistreamDestinationName"));
		name_->setMaxLength(128);
		name_->setPlaceholderText(text("Multistream.NamePlaceholder"));
		server_ = new QLineEdit(QString::fromStdString(target.server), this);
		server_->setObjectName(QStringLiteral("multistreamDestinationServer"));
		server_->setMaxLength(4096);
		server_->setPlaceholderText(QStringLiteral("rtmps://example.com/live"));
		server_->setInputMethodHints(Qt::ImhUrlCharactersOnly | Qt::ImhNoAutoUppercase);
		key_ = new QLineEdit(QString::fromStdString(target.key), this);
		key_->setObjectName(QStringLiteral("multistreamDestinationKey"));
		key_->setMaxLength(4096);
		key_->setEchoMode(QLineEdit::Password);
		key_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
		for (auto *input : {name_, server_, key_})
			input->setMinimumHeight(28);
		form->addRow(text("Multistream.Name"), name_);
		form->addRow(text("Multistream.Server"), server_);
		form->addRow(text("Multistream.StreamKey"), key_);
		root->addLayout(form);

		auto *showKey = new QCheckBox(text("Multistream.ShowKey"), this);
		showKey->setObjectName(QStringLiteral("multistreamShowKey"));
		connect(showKey, &QCheckBox::toggled, this, [this](const bool shown) {
			key_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		});
		root->addWidget(showKey);
		auto *keyWarning = new QLabel(text("Multistream.KeyWarning"), this);
		keyWarning->setWordWrap(true);
		root->addWidget(keyWarning);
		validation_ = new QLabel(this);
		validation_->setWordWrap(true);
		validation_->setStyleSheet(QStringLiteral("color: #ef5350;"));
		validation_->hide();
		root->addWidget(validation_);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
		buttons->button(QDialogButtonBox::Save)->setText(text("Multistream.Save"));
		buttons->button(QDialogButtonBox::Cancel)->setText(text("Multistream.Cancel"));
		for (auto *button : buttons->buttons())
			button->setMinimumHeight(28);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			if (name_->text().trimmed().isEmpty()) {
				validation_->setText(text("Multistream.NameRequired"));
				validation_->show();
				name_->setFocus();
				return;
			}
			if (key_->text().isEmpty()) {
				validation_->setText(text("Multistream.Error.Fields"));
				validation_->show();
				key_->setFocus();
				return;
			}
			const QString server = server_->text().trimmed();
			const QString key = key_->text();
			const auto invalidSpace = [](const QChar value) {
				return value.unicode() <= 0x20 || value.unicode() == 0x7f;
			};
			const QUrl url(server, QUrl::StrictMode);
			if (std::any_of(server.begin(), server.end(), invalidSpace) ||
			    std::any_of(key.begin(), key.end(), invalidSpace) || !url.isValid() ||
			    url.host().isEmpty() || url.path().isEmpty() || !url.userInfo().isEmpty() ||
			    url.hasQuery() || url.hasFragment() ||
			    (url.scheme() != QStringLiteral("rtmp") && url.scheme() != QStringLiteral("rtmps"))) {
				validation_->setText(text("Multistream.Error.URL"));
				validation_->show();
				server_->setFocus();
				return;
			}
			accept();
		});
		root->addWidget(buttons);
	}

	void apply_to(MultistreamTarget &target) const
	{
		target.name = name_->text().trimmed().toStdString();
		target.server = server_->text().trimmed().toStdString();
		target.key = key_->text().toStdString();
	}

private:
	QLineEdit *name_ = nullptr;
	QLineEdit *server_ = nullptr;
	QLineEdit *key_ = nullptr;
	QLabel *validation_ = nullptr;
};

} // namespace

MultistreamDock::MultistreamDock(MultistreamController &controller, QWidget *parent)
	: QWidget(parent),
	  controller_(controller)
{
	build_ui();
	// Coalesce the controller's transport updates without a second idle poll.
	pollTimer_.setSingleShot(true);
	pollTimer_.setInterval(500);
	connect(&controller_, &MultistreamController::changed, this, [this] {
		if (!pollTimer_.isActive())
			pollTimer_.start();
	});
	connect(&pollTimer_, &QTimer::timeout, this, &MultistreamDock::refresh);
	refresh();
}

MultistreamDock::~MultistreamDock() = default;

void MultistreamDock::build_ui()
{
	setObjectName(QStringLiteral("multistreamDock"));
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);
	summary_ = new QLabel(this);
	QFont font = summary_->font();
	font.setBold(true);
	summary_->setFont(font);
	root->addWidget(summary_);
	auto *description = new QLabel(text("Multistream.Description"), this);
	description->setWordWrap(true);
	root->addWidget(description);
	auto *compatibility = new QLabel(text("Multistream.Compatibility"), this);
	compatibility->setWordWrap(true);
	root->addWidget(compatibility);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumHeight(130);
	auto *targets = new QWidget(scroll);
	targets->setObjectName(QStringLiteral("multistreamTargets"));
	targetsLayout_ = new QVBoxLayout(targets);
	targetsLayout_->setContentsMargins(0, 0, 0, 0);
	targetsLayout_->setSpacing(8);
	targetsLayout_->setAlignment(Qt::AlignTop);
	empty_ = new QLabel(text("Multistream.Empty"), targets);
	empty_->setWordWrap(true);
	targetsLayout_->addWidget(empty_);
	scroll->setWidget(targets);
	root->addWidget(scroll, 1);
	auto *add = new QPushButton(text("Multistream.Add"), this);
	add->setObjectName(QStringLiteral("multistreamAddTarget"));
	add->setMinimumHeight(32);
	connect(add, &QPushButton::clicked, this, [this] { edit_target({}); });
	root->addWidget(add);
}

void MultistreamDock::rebuild_rows()
{
	for (const auto &row : rows_) {
		targetsLayout_->removeWidget(row.widget);
		row.widget->hide();
		row.widget->deleteLater();
	}
	rows_.clear();
	for (const auto &target : controller_.targets()) {
		TargetRow row;
		row.id = target.id;
		auto *frame = new QFrame(this);
		frame->setObjectName(QStringLiteral("multistreamTarget_%1").arg(QString::fromStdString(target.id)));
		frame->setFrameShape(QFrame::StyledPanel);
		row.widget = frame;
		auto *layout = new QVBoxLayout(frame);
		layout->setContentsMargins(10, 10, 10, 10);
		layout->setSpacing(6);
		row.name = new QLabel(frame);
		row.name->setTextFormat(Qt::PlainText);
		row.name->setWordWrap(true);
		QFont font = row.name->font();
		font.setBold(true);
		row.name->setFont(font);
		layout->addWidget(row.name);
		row.server = new QLabel(frame);
		row.server->setTextFormat(Qt::PlainText);
		row.server->setWordWrap(true);
		layout->addWidget(row.server);
		row.state = new QLabel(frame);
		row.state->setTextFormat(Qt::PlainText);
		row.state->setWordWrap(true);
		layout->addWidget(row.state);
		row.detail = new QLabel(frame);
		row.detail->setTextFormat(Qt::PlainText);
		row.detail->setWordWrap(true);
		layout->addWidget(row.detail);
		auto *buttons = new QHBoxLayout;
		row.toggle = new QPushButton(frame);
		row.edit = new QPushButton(text("Multistream.Edit"), frame);
		row.remove = new QPushButton(text("Multistream.Remove"), frame);
		buttons->addWidget(row.toggle, 1);
		buttons->addWidget(row.edit);
		buttons->addWidget(row.remove);
		layout->addLayout(buttons);
		connect(row.toggle, &QPushButton::clicked, this, [this, id = target.id] { toggle_target(id); });
		connect(row.edit, &QPushButton::clicked, this, [this, id = target.id] { edit_target(id); });
		connect(row.remove, &QPushButton::clicked, this, [this, id = target.id] { remove_target(id); });
		targetsLayout_->addWidget(frame);
		rows_.push_back(std::move(row));
	}
	// Stable widgets keep keyboard focus and pointer targets during polling.
	empty_->setVisible(rows_.empty());
}

void MultistreamDock::refresh()
{
	const auto &targets = controller_.targets();
	if (targets.size() != rows_.size() ||
	    !std::equal(targets.begin(), targets.end(), rows_.begin(),
			[](const auto &target, const auto &row) { return target.id == row.id; }))
		rebuild_rows();
	const auto statuses = controller_.statuses();
	std::size_t streaming = 0;
	for (std::size_t index = 0; index < rows_.size(); ++index) {
		auto &row = rows_[index];
		const auto &target = targets[index];
		const auto found = std::find_if(statuses.begin(), statuses.end(),
						[&row](const auto &status) { return status.id == row.id; });
		const MultistreamState state = found == statuses.end() ? MultistreamState::Stopped : found->state;
		streaming += state == MultistreamState::Streaming ? 1U : 0U;
		const bool running = controller_.running(row.id);
		set_text(row.name, QString::fromStdString(target.name));
		set_text(row.server, server_display(target.server));
		set_text(row.state, state_text(state));
		const QString detail = found == statuses.end() ? QString{} : QString::fromStdString(found->detail);
		set_text(row.detail, detail);
		row.detail->setVisible(!detail.isEmpty());
		const QString action = text(running ? "Multistream.Stop" : "Multistream.Start");
		if (row.toggle->text() != action)
			row.toggle->setText(action);
		row.toggle->setAccessibleName(action + QStringLiteral(" ") + QString::fromStdString(target.name));
		row.edit->setEnabled(!running);
		row.remove->setEnabled(!running);
		const QString editingHint = running ? text("Multistream.StopBeforeEditing") : QString{};
		row.edit->setToolTip(editingHint);
		row.remove->setToolTip(editingHint);
		if (row.lastState != static_cast<int>(state)) {
			row.state->setStyleSheet(
				QStringLiteral("color: %1;")
					.arg(state_color(state, palette().color(QPalette::Window).lightness() < 128)));
			row.lastState = static_cast<int>(state);
		}
	}
	set_text(summary_, text("Multistream.Summary")
				   .arg(static_cast<qulonglong>(streaming))
				   .arg(static_cast<qulonglong>(targets.size())));
}

void MultistreamDock::edit_target(const std::string &id)
{
	MultistreamTarget target{};
	if (!id.empty()) {
		if (controller_.running(id))
			return;
		const auto &targets = controller_.targets();
		const auto found = std::find_if(targets.begin(), targets.end(),
						[&id](const auto &entry) { return entry.id == id; });
		if (found == targets.end())
			return;
		target = *found;
	}
	TargetEditor editor(target, this);
	if (editor.exec() != QDialog::Accepted)
		return;
	editor.apply_to(target);
	QString error;
	if (!controller_.save_target(std::move(target), error))
		show_error(error);
	refresh();
}

void MultistreamDock::remove_target(const std::string &id)
{
	if (controller_.running(id))
		return;
	const auto &targets = controller_.targets();
	const auto found =
		std::find_if(targets.begin(), targets.end(), [&id](const auto &entry) { return entry.id == id; });
	if (found == targets.end())
		return;
	QMessageBox confirm(QMessageBox::Question, text("Multistream.RemoveTitle"),
			    text("Multistream.RemoveConfirm").arg(QString::fromStdString(found->name)),
			    QMessageBox::Yes | QMessageBox::No, this);
	confirm.setTextFormat(Qt::PlainText);
	confirm.button(QMessageBox::Yes)->setText(text("Multistream.Remove"));
	confirm.button(QMessageBox::No)->setText(text("Multistream.Cancel"));
	confirm.setDefaultButton(QMessageBox::No);
	if (confirm.exec() != QMessageBox::Yes)
		return;
	QString error;
	if (!controller_.remove_target(id, error))
		show_error(error);
	refresh();
}

void MultistreamDock::toggle_target(const std::string &id)
{
	if (controller_.running(id)) {
		controller_.stop_target(id);
	} else {
		QString error;
		if (!controller_.start_target(id, error))
			show_error(error);
	}
	refresh();
}

void MultistreamDock::show_error(const QString &error)
{
	QMessageBox message(QMessageBox::Warning, text("Multistream.ErrorTitle"), error, QMessageBox::Ok, this);
	message.setTextFormat(Qt::PlainText);
	message.exec();
}

} // namespace dynamic_delay
