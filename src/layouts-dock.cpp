#include "layouts-dock.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Clé unique d'un item : nom de la source + numéro d'occurrence
 * (au cas où la même source est ajoutée 2 fois dans la scène). */
static QString itemKey(const QString &source, int occurrence)
{
	return source + QStringLiteral("#") + QString::number(occurrence);
}

/* ------------------------------------------------------------------ */

LayoutsDock::LayoutsDock(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(220);

	targetLabel = new QLabel(this);
	targetLabel->setWordWrap(true);

	list = new QListWidget(this);
	list->setSelectionMode(QAbstractItemView::SingleSelection);

	nameEdit = new QLineEdit(this);
	nameEdit->setPlaceholderText(T("NamePlaceholder"));
	saveBtn = new QPushButton(T("Save"), this);
	saveBtn->setToolTip(T("SaveTip"));

	auto *saveRow = new QHBoxLayout();
	saveRow->addWidget(nameEdit, 1);
	saveRow->addWidget(saveBtn);

	applyBtn = new QPushButton(T("Apply"), this);
	applyBtn->setToolTip(T("ApplyTip"));
	applyBtn->setDefault(true);
	overwriteBtn = new QPushButton(T("Overwrite"), this);
	overwriteBtn->setToolTip(T("OverwriteTip"));
	renameBtn = new QPushButton(T("Rename"), this);
	deleteBtn = new QPushButton(T("Delete"), this);
	upBtn = new QPushButton(QStringLiteral("▲"), this);
	downBtn = new QPushButton(QStringLiteral("▼"), this);
	upBtn->setFixedWidth(28);
	downBtn->setFixedWidth(28);

	auto *actRow1 = new QHBoxLayout();
	actRow1->addWidget(applyBtn, 1);
	actRow1->addWidget(overwriteBtn, 1);

	auto *actRow2 = new QHBoxLayout();
	actRow2->addWidget(renameBtn, 1);
	actRow2->addWidget(deleteBtn, 1);
	actRow2->addWidget(upBtn);
	actRow2->addWidget(downBtn);

	applyVisibility = new QCheckBox(T("OptVisibility"), this);
	applyOrder = new QCheckBox(T("OptOrder"), this);
	applyCrop = new QCheckBox(T("OptCrop"), this);
	applyVisibility->setChecked(true);
	applyOrder->setChecked(true);
	applyCrop->setChecked(true);

	statusLabel = new QLabel(this);
	statusLabel->setWordWrap(true);
	statusLabel->setStyleSheet(QStringLiteral("color: gray;"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->addWidget(targetLabel);
	root->addWidget(list, 1);
	root->addLayout(actRow1);
	root->addLayout(actRow2);
	root->addLayout(saveRow);
	root->addWidget(applyVisibility);
	root->addWidget(applyOrder);
	root->addWidget(applyCrop);
	root->addWidget(statusLabel);

	connect(saveBtn, &QPushButton::clicked, this, &LayoutsDock::onSave);
	connect(nameEdit, &QLineEdit::returnPressed, this, &LayoutsDock::onSave);
	connect(applyBtn, &QPushButton::clicked, this, &LayoutsDock::onApply);
	connect(overwriteBtn, &QPushButton::clicked, this, &LayoutsDock::onOverwrite);
	connect(renameBtn, &QPushButton::clicked, this, &LayoutsDock::onRename);
	connect(deleteBtn, &QPushButton::clicked, this, &LayoutsDock::onDelete);
	connect(upBtn, &QPushButton::clicked, this, &LayoutsDock::onMoveUp);
	connect(downBtn, &QPushButton::clicked, this, &LayoutsDock::onMoveDown);
	connect(list, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { applyRow(list->row(item)); });
	connect(list, &QListWidget::currentRowChanged, this, &LayoutsDock::updateButtons);
	for (QCheckBox *cb : {applyVisibility, applyOrder, applyCrop})
		connect(cb, &QCheckBox::toggled, this, [this]() { saveFile(); });

	loadFile();
	rebuildList(layouts.isEmpty() ? -1 : 0);
	refreshTarget();

	obs_frontend_add_event_callback(frontendEvent, this);
}

LayoutsDock::~LayoutsDock()
{
	obs_frontend_remove_event_callback(frontendEvent, this);
}

void LayoutsDock::frontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<LayoutsDock *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		QMetaObject::invokeMethod(self, "refreshTarget", Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Scène cible : la scène en PREVIEW en Studio Mode, sinon la scène
 * courante (quand le Studio Mode est off, c'est ce que tu vois).   */

obs_source_t *LayoutsDock::targetScene(bool *isPreview) const
{
	const bool studio = obs_frontend_preview_program_mode_active();
	if (isPreview)
		*isPreview = studio;
	return studio ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
}

void LayoutsDock::refreshTarget()
{
	bool isPreview = false;
	obs_source_t *src = targetScene(&isPreview);
	if (!src) {
		targetLabel->setText(T("NoScene"));
	} else {
		const QString name = QString::fromUtf8(obs_source_get_name(src));
		targetLabel->setText(
			QStringLiteral("<b>%1</b> %2")
				.arg(isPreview ? T("TargetPreview") : T("TargetProgram"), name.toHtmlEscaped()));
		obs_source_release(src);
	}
	updateButtons();
}

void LayoutsDock::updateButtons()
{
	const int row = currentRow();
	const bool sel = row >= 0;
	applyBtn->setEnabled(sel);
	overwriteBtn->setEnabled(sel);
	renameBtn->setEnabled(sel);
	deleteBtn->setEnabled(sel);
	upBtn->setEnabled(sel && row > 0);
	downBtn->setEnabled(sel && row < layouts.size() - 1);
}

/* ------------------------------------------------------------------ */
/* Capture / application                                             */

QJsonArray LayoutsDock::captureScene(obs_scene_t *scene) const
{
	struct Ctx {
		QJsonArray arr;
		QHash<QString, int> seen;
		int order = 0;
	} ctx;

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const QString name = QString::fromUtf8(obs_source_get_name(src));
			const int occ = c->seen.value(name, 0);
			c->seen[name] = occ + 1;

			obs_transform_info info;
			obs_sceneitem_get_info2(item, &info);
			obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(item, &crop);

			QJsonObject o;
			o["source"] = name;
			o["occurrence"] = occ;
			o["order"] = c->order++;
			o["visible"] = obs_sceneitem_visible(item);
			o["pos_x"] = info.pos.x;
			o["pos_y"] = info.pos.y;
			o["rot"] = info.rot;
			o["scale_x"] = info.scale.x;
			o["scale_y"] = info.scale.y;
			o["alignment"] = (int)info.alignment;
			o["bounds_type"] = (int)info.bounds_type;
			o["bounds_alignment"] = (int)info.bounds_alignment;
			o["bounds_x"] = info.bounds.x;
			o["bounds_y"] = info.bounds.y;
			o["crop_left"] = crop.left;
			o["crop_top"] = crop.top;
			o["crop_right"] = crop.right;
			o["crop_bottom"] = crop.bottom;
			c->arr.append(o);
			return true;
		},
		&ctx);

	return ctx.arr;
}

int LayoutsDock::applyToScene(obs_scene_t *scene, const PreviewLayout &layout) const
{
	QHash<QString, QJsonObject> saved;
	for (const QJsonValue &v : layout.items) {
		const QJsonObject o = v.toObject();
		saved.insert(itemKey(o["source"].toString(), o["occurrence"].toInt()), o);
	}

	struct Ctx {
		const QHash<QString, QJsonObject> *saved;
		QHash<QString, int> seen;
		std::vector<std::pair<int, obs_sceneitem_t *>> matched; // (ordre sauvé, item)
		bool vis, crop;
	} ctx{&saved, {}, {}, applyVisibility->isChecked(), applyCrop->isChecked()};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			const QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
			const int occ = c->seen.value(name, 0);
			c->seen[name] = occ + 1;

			auto it = c->saved->constFind(itemKey(name, occ));
			if (it == c->saved->constEnd())
				return true;
			const QJsonObject &o = it.value();

			obs_sceneitem_defer_update_begin(item);

			obs_transform_info info;
			obs_sceneitem_get_info2(item, &info); // garde les champs qu'on ne gère pas
			info.pos.x = (float)o["pos_x"].toDouble();
			info.pos.y = (float)o["pos_y"].toDouble();
			info.rot = (float)o["rot"].toDouble();
			info.scale.x = (float)o["scale_x"].toDouble(1.0);
			info.scale.y = (float)o["scale_y"].toDouble(1.0);
			info.alignment = (uint32_t)o["alignment"].toInt();
			info.bounds_type = (enum obs_bounds_type)o["bounds_type"].toInt();
			info.bounds_alignment = (uint32_t)o["bounds_alignment"].toInt();
			info.bounds.x = (float)o["bounds_x"].toDouble();
			info.bounds.y = (float)o["bounds_y"].toDouble();
			obs_sceneitem_set_info2(item, &info);

			if (c->crop) {
				obs_sceneitem_crop crop;
				crop.left = o["crop_left"].toInt();
				crop.top = o["crop_top"].toInt();
				crop.right = o["crop_right"].toInt();
				crop.bottom = o["crop_bottom"].toInt();
				obs_sceneitem_set_crop(item, &crop);
			}

			obs_sceneitem_defer_update_end(item);

			if (c->vis)
				obs_sceneitem_set_visible(item, o["visible"].toBool(true));

			obs_sceneitem_addref(item);
			c->matched.emplace_back(o["order"].toInt(), item);
			return true;
		},
		&ctx);

	const int count = (int)ctx.matched.size();

	/* Ordre : on redistribue les positions actuelles des items trouvés
	 * selon l'ordre sauvegardé (les autres items ne bougent pas trop). */
	if (applyOrder->isChecked() && count > 1) {
		std::vector<int> positions;
		positions.reserve(count);
		for (auto &m : ctx.matched)
			positions.push_back(obs_sceneitem_get_order_position(m.second));
		std::sort(positions.begin(), positions.end());
		std::stable_sort(ctx.matched.begin(), ctx.matched.end(),
				 [](const auto &a, const auto &b) { return a.first < b.first; });
		for (int i = 0; i < count; i++)
			obs_sceneitem_set_order_position(ctx.matched[i].second, positions[i]);
	}

	for (auto &m : ctx.matched)
		obs_sceneitem_release(m.second);

	return count;
}

/* ------------------------------------------------------------------ */
/* Actions                                                           */

void LayoutsDock::onSave()
{
	QString name = nameEdit->text().trimmed();

	obs_source_t *src = targetScene();
	obs_scene_t *scene = src ? obs_scene_from_source(src) : nullptr;
	if (!scene) {
		if (src)
			obs_source_release(src);
		flash(T("NoScene"));
		return;
	}

	const QString sceneName = QString::fromUtf8(obs_source_get_name(src));
	if (name.isEmpty())
		name = sceneName + QStringLiteral(" ") + QString::number(layouts.size() + 1);

	PreviewLayout l{name, sceneName, captureScene(scene)};
	obs_source_release(src);

	// même nom = on écrase (après confirmation)
	for (int i = 0; i < layouts.size(); i++) {
		if (layouts[i].name == name) {
			if (QMessageBox::question(this, T("DockTitle"), T("ConfirmReplace").arg(name)) !=
			    QMessageBox::Yes)
				return;
			layouts[i] = l;
			saveFile();
			rebuildList(i);
			nameEdit->clear();
			flash(T("Saved").arg(name).arg(l.items.size()));
			return;
		}
	}

	layouts.append(l);
	saveFile();
	rebuildList(layouts.size() - 1);
	nameEdit->clear();
	flash(T("Saved").arg(name).arg(l.items.size()));
}

void LayoutsDock::onOverwrite()
{
	const int row = currentRow();
	if (row < 0)
		return;

	obs_source_t *src = targetScene();
	obs_scene_t *scene = src ? obs_scene_from_source(src) : nullptr;
	if (!scene) {
		if (src)
			obs_source_release(src);
		flash(T("NoScene"));
		return;
	}

	if (QMessageBox::question(this, T("DockTitle"), T("ConfirmReplace").arg(layouts[row].name)) !=
	    QMessageBox::Yes) {
		obs_source_release(src);
		return;
	}

	layouts[row].fromScene = QString::fromUtf8(obs_source_get_name(src));
	layouts[row].items = captureScene(scene);
	obs_source_release(src);

	saveFile();
	rebuildList(row);
	flash(T("Saved").arg(layouts[row].name).arg(layouts[row].items.size()));
}

void LayoutsDock::onApply()
{
	applyRow(currentRow());
}

void LayoutsDock::applyRow(int row)
{
	if (row < 0 || row >= layouts.size())
		return;

	obs_source_t *src = targetScene();
	obs_scene_t *scene = src ? obs_scene_from_source(src) : nullptr;
	if (!scene) {
		if (src)
			obs_source_release(src);
		flash(T("NoScene"));
		return;
	}

	const int n = applyToScene(scene, layouts[row]);
	const QString sceneName = QString::fromUtf8(obs_source_get_name(src));
	obs_source_release(src);

	if (n == 0)
		flash(T("NothingMatched").arg(layouts[row].name, sceneName));
	else
		flash(T("Applied").arg(layouts[row].name, sceneName).arg(n));
}

void LayoutsDock::onRename()
{
	const int row = currentRow();
	if (row < 0)
		return;
	bool ok = false;
	const QString name =
		QInputDialog::getText(this, T("Rename"), T("NewName"), QLineEdit::Normal, layouts[row].name, &ok)
			.trimmed();
	if (!ok || name.isEmpty())
		return;
	layouts[row].name = name;
	saveFile();
	rebuildList(row);
}

void LayoutsDock::onDelete()
{
	const int row = currentRow();
	if (row < 0)
		return;
	if (QMessageBox::question(this, T("Delete"), T("ConfirmDelete").arg(layouts[row].name)) != QMessageBox::Yes)
		return;
	layouts.removeAt(row);
	saveFile();
	rebuildList(std::min(row, (int)layouts.size() - 1));
}

void LayoutsDock::onMoveUp()
{
	const int row = currentRow();
	if (row <= 0)
		return;
	std::swap(layouts[row], layouts[row - 1]);
	saveFile();
	rebuildList(row - 1);
}

void LayoutsDock::onMoveDown()
{
	const int row = currentRow();
	if (row < 0 || row >= layouts.size() - 1)
		return;
	std::swap(layouts[row], layouts[row + 1]);
	saveFile();
	rebuildList(row + 1);
}

/* ------------------------------------------------------------------ */
/* Liste + stockage                                                  */

int LayoutsDock::currentRow() const
{
	return list->currentRow();
}

void LayoutsDock::rebuildList(int selectRow)
{
	list->blockSignals(true);
	list->clear();
	for (const PreviewLayout &l : layouts) {
		auto *item = new QListWidgetItem(l.name, list);
		item->setToolTip(T("ItemTip").arg(l.fromScene).arg(l.items.size()));
	}
	list->blockSignals(false);
	if (selectRow >= 0 && selectRow < layouts.size())
		list->setCurrentRow(selectRow);
	updateButtons();
}

void LayoutsDock::flash(const QString &msg)
{
	statusLabel->setText(msg);
	QTimer::singleShot(4000, statusLabel, [lbl = statusLabel, msg]() {
		if (lbl->text() == msg)
			lbl->clear();
	});
}

QString LayoutsDock::storagePath() const
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("layouts.json");
	const QString p = QString::fromUtf8(path ? path : "");
	bfree(path);
	return p;
}

void LayoutsDock::loadFile()
{
	QFile f(storagePath());
	if (!f.open(QIODevice::ReadOnly))
		return;

	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

	const QJsonObject opts = root["options"].toObject();
	applyVisibility->setChecked(opts["visibility"].toBool(true));
	applyOrder->setChecked(opts["order"].toBool(true));
	applyCrop->setChecked(opts["crop"].toBool(true));

	layouts.clear();
	for (const QJsonValue &v : root["layouts"].toArray()) {
		const QJsonObject o = v.toObject();
		layouts.append({o["name"].toString(), o["scene"].toString(), o["items"].toArray()});
	}
}

void LayoutsDock::saveFile() const
{
	QJsonArray arr;
	for (const PreviewLayout &l : layouts) {
		QJsonObject o;
		o["name"] = l.name;
		o["scene"] = l.fromScene;
		o["items"] = l.items;
		arr.append(o);
	}

	QJsonObject opts;
	opts["visibility"] = applyVisibility->isChecked();
	opts["order"] = applyOrder->isChecked();
	opts["crop"] = applyCrop->isChecked();

	QJsonObject root;
	root["version"] = 1;
	root["options"] = opts;
	root["layouts"] = arr;

	QFile f(storagePath());
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_WARNING, "could not write %s", f.fileName().toUtf8().constData());
		return;
	}
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}
