#pragma once

#include <obs.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QCheckBox;
class QPushButton;

struct PreviewLayout {
	QString name;
	QString fromScene;
	QJsonArray items;
};

class LayoutsDock : public QWidget {
	Q_OBJECT

public:
	explicit LayoutsDock(QWidget *parent = nullptr);
	~LayoutsDock() override;

private slots:
	void onSave();
	void onOverwrite();
	void onApply();
	void onRename();
	void onDelete();
	void onMoveUp();
	void onMoveDown();
	void refreshTarget();
	void updateButtons();

private:
	static void frontendEvent(enum obs_frontend_event event, void *data);

	obs_source_t *targetScene(bool *isPreview = nullptr) const; // new ref
	QJsonArray captureScene(obs_scene_t *scene) const;
	int applyToScene(obs_scene_t *scene, const PreviewLayout &layout) const;
	void applyRow(int row);

	QString storagePath() const;
	void loadFile();
	void saveFile() const;
	void rebuildList(int selectRow = -1);
	int currentRow() const;
	void flash(const QString &msg);

	QVector<PreviewLayout> layouts;

	QLabel *targetLabel = nullptr;
	QLabel *statusLabel = nullptr;
	QListWidget *list = nullptr;
	QLineEdit *nameEdit = nullptr;
	QPushButton *saveBtn = nullptr;
	QPushButton *applyBtn = nullptr;
	QPushButton *overwriteBtn = nullptr;
	QPushButton *renameBtn = nullptr;
	QPushButton *deleteBtn = nullptr;
	QPushButton *upBtn = nullptr;
	QPushButton *downBtn = nullptr;
	QCheckBox *applyVisibility = nullptr;
	QCheckBox *applyOrder = nullptr;
	QCheckBox *applyCrop = nullptr;
};
