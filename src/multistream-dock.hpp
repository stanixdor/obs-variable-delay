#pragma once

#include <QTimer>
#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace dynamic_delay {

class MultistreamController;

class MultistreamDock final : public QWidget {
	Q_OBJECT

public:
	explicit MultistreamDock(MultistreamController &controller, QWidget *parent = nullptr);
	~MultistreamDock() override;

private:
	struct TargetRow {
		std::string id;
		QWidget *widget = nullptr;
		QLabel *name = nullptr;
		QLabel *server = nullptr;
		QLabel *state = nullptr;
		QLabel *detail = nullptr;
		QPushButton *toggle = nullptr;
		QPushButton *edit = nullptr;
		QPushButton *remove = nullptr;
		int lastState = -1;
	};

	void build_ui();
	void rebuild_rows();
	void refresh();
	void edit_target(const std::string &id);
	void remove_target(const std::string &id);
	void toggle_target(const std::string &id);
	void show_error(const QString &error);

	MultistreamController &controller_;
	QTimer pollTimer_;
	QLabel *summary_ = nullptr;
	QLabel *empty_ = nullptr;
	QVBoxLayout *targetsLayout_ = nullptr;
	std::vector<TargetRow> rows_;
};

} // namespace dynamic_delay
