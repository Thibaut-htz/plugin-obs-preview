#include "mv-state.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */

static obs_source_t *createLabel(const QString &text)
{
	if (text.isEmpty())
		return nullptr;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Arial");
	obs_data_set_string(font, "style", "Bold");
	obs_data_set_int(font, "size", 64);
	obs_data_set_int(font, "flags", 1);

	obs_data_t *settings = obs_data_create();
	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.toUtf8().constData());

#ifdef _WIN32
	const char *id = "text_gdiplus";
#else
	const char *id = "text_ft2_source";
#endif
	obs_source_t *src = obs_source_create_private(id, "sanna-mv-label", settings);
	obs_data_release(settings);
	obs_data_release(font);
	return src;
}

/* Libère le contenu d'une case (hors verrou !) */
static void releaseContent(MvTile &t)
{
	if (t.weak) {
		obs_source_t *src = obs_weak_source_get_source(t.weak);
		if (src) {
			obs_source_dec_showing(src);
			obs_source_release(src);
		}
		obs_weak_source_release(t.weak);
	}
	obs_source_release(t.label);
	t.weak = nullptr;
	t.label = nullptr;
}

/* Remplit le contenu d'une case (hors verrou !) */
static void makeContent(MvTile &t, int kind, const QString &name)
{
	t.kind = kind;
	t.name.clear();
	t.weak = nullptr;
	t.label = nullptr;
	if (kind != MV_SOURCE)
		return;
	obs_source_t *src = name.isEmpty() ? nullptr : obs_get_source_by_name(name.toUtf8().constData());
	if (!src) {
		t.kind = MV_EMPTY;
		return;
	}
	t.name = name;
	t.weak = obs_source_get_weak_source(src);
	t.label = createLabel(name);
	obs_source_inc_showing(src); // les médias jouent même hors programme
	obs_source_release(src);
}

static QRectF clampRect(QRectF r)
{
	r.setWidth(std::clamp(r.width(), 40.0, MV_CANVAS_W));
	r.setHeight(std::clamp(r.height(), 22.0, MV_CANVAS_H));
	r.moveLeft(std::clamp(r.x(), 0.0, MV_CANVAS_W - r.width()));
	r.moveTop(std::clamp(r.y(), 0.0, MV_CANVAS_H - r.height()));
	return r;
}

static QStringList sceneNames()
{
	QStringList names;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const QString n = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
		if (!n.startsWith(QStringLiteral("--"))) // séparateurs "-----|Caméra|-----"
			names << n;
	}
	obs_frontend_source_list_free(&scenes);
	return names;
}

/* ------------------------------------------------------------------ */
/* Modèles                                                            */

static void addGrid(MvPreset &p, double x0, double y0, int rows, int cols, double w, double h, int kind)
{
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			p.tiles.push_back({QRectF(x0 + c * w, y0 + r * h, w, h), kind});
}

const std::vector<MvPreset> &mvPresets()
{
	static std::vector<MvPreset> presets = [] {
		std::vector<MvPreset> v;

		MvPreset a{"PresetTop18", {}};
		a.tiles.push_back({QRectF(0, 0, 960, 540), MV_PREVIEW});
		a.tiles.push_back({QRectF(960, 0, 960, 540), MV_PROGRAM});
		addGrid(a, 0, 540, 3, 6, 320, 180, MV_SOURCE);
		v.push_back(a);

		MvPreset b{"PresetTop8", {}};
		b.tiles.push_back({QRectF(0, 0, 960, 540), MV_PREVIEW});
		b.tiles.push_back({QRectF(960, 0, 960, 540), MV_PROGRAM});
		addGrid(b, 0, 540, 2, 4, 480, 270, MV_SOURCE);
		v.push_back(b);

		MvPreset c{"PresetBigProgram", {}};
		c.tiles.push_back({QRectF(0, 0, 1440, 810), MV_PROGRAM});
		c.tiles.push_back({QRectF(1440, 0, 480, 270), MV_PREVIEW});
		addGrid(c, 1440, 270, 3, 1, 480, 270, MV_SOURCE);
		addGrid(c, 0, 810, 1, 3, 480, 270, MV_SOURCE);
		v.push_back(c);

		MvPreset d{"PresetGrid16", {}};
		addGrid(d, 0, 0, 4, 4, 480, 270, MV_SOURCE);
		v.push_back(d);

		MvPreset e{"PresetEmpty", {}};
		v.push_back(e);
		return v;
	}();
	return presets;
}

void MvSnapshot::release()
{
	for (Item &i : tiles) {
		obs_source_release(i.src);
		obs_source_release(i.label);
	}
	tiles.clear();
	obs_source_release(previewScene);
	obs_source_release(programScene);
	obs_source_release(previewLabel);
	obs_source_release(programLabel);
	previewScene = programScene = previewLabel = programLabel = nullptr;
}

/* ------------------------------------------------------------------ */

MvState &MvState::get()
{
	static MvState s;
	return s;
}

MvSnapshot MvState::snapshot()
{
	MvSnapshot s;
	std::lock_guard<std::mutex> lock(mtx);
	s.showLabels = showLabels_;
	s.tiles.reserve(tiles_.size());
	for (MvTile &t : tiles_) {
		MvSnapshot::Item it;
		it.rect = t.rect;
		it.kind = t.kind;
		it.src = t.weak ? obs_weak_source_get_source(t.weak) : nullptr;
		it.label = obs_source_get_ref(t.label);
		s.tiles.push_back(it);
	}
	s.previewScene = preview_ ? obs_weak_source_get_source(preview_) : nullptr;
	s.programScene = program_ ? obs_weak_source_get_source(program_) : nullptr;
	s.previewLabel = obs_source_get_ref(previewLabel_);
	s.programLabel = obs_source_get_ref(programLabel_);
	return s;
}

int MvState::tileCount()
{
	std::lock_guard<std::mutex> lock(mtx);
	return (int)tiles_.size();
}

QRectF MvState::tileRect(int i)
{
	std::lock_guard<std::mutex> lock(mtx);
	return (i >= 0 && i < (int)tiles_.size()) ? tiles_[i].rect : QRectF();
}

int MvState::tileKind(int i)
{
	std::lock_guard<std::mutex> lock(mtx);
	return (i >= 0 && i < (int)tiles_.size()) ? tiles_[i].kind : MV_EMPTY;
}

QString MvState::tileName(int i)
{
	obs_source_t *src = nullptr;
	QString n;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (i < 0 || i >= (int)tiles_.size())
			return QString();
		src = tiles_[i].weak ? obs_weak_source_get_source(tiles_[i].weak) : nullptr;
		n = tiles_[i].name;
	}
	if (src) // la source a peut-être été renommée
		n = QString::fromUtf8(obs_source_get_name(src));
	obs_source_release(src);
	return n;
}

bool MvState::showLabels()
{
	std::lock_guard<std::mutex> lock(mtx);
	return showLabels_;
}

int MvState::addTile(const QRectF &rect, int kind, const QString &name)
{
	MvTile t;
	makeContent(t, kind, name);
	t.rect = clampRect(rect);
	int idx;
	{
		std::lock_guard<std::mutex> lock(mtx);
		tiles_.push_back(t);
		idx = (int)tiles_.size() - 1;
	}
	save();
	return idx;
}

void MvState::removeTile(int i)
{
	MvTile old;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (i < 0 || i >= (int)tiles_.size())
			return;
		old = tiles_[i];
		tiles_.erase(tiles_.begin() + i);
	}
	releaseContent(old);
	save();
}

void MvState::setTileRect(int i, const QRectF &rect, bool saveNow)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (i < 0 || i >= (int)tiles_.size())
			return;
		tiles_[i].rect = clampRect(rect);
	}
	if (saveNow)
		save();
}

void MvState::setTileContent(int i, int kind, const QString &name)
{
	MvTile fresh;
	makeContent(fresh, kind, name);
	MvTile old;
	bool ok = false;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (i >= 0 && i < (int)tiles_.size()) {
			old = tiles_[i];
			fresh.rect = old.rect;
			tiles_[i] = fresh;
			ok = true;
		}
	}
	releaseContent(ok ? old : fresh);
	save();
}

int MvState::raiseTile(int i)
{
	int idx = i;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (i < 0 || i >= (int)tiles_.size())
			return i;
		MvTile t = tiles_[i];
		tiles_.erase(tiles_.begin() + i);
		tiles_.push_back(t);
		idx = (int)tiles_.size() - 1;
	}
	save();
	return idx;
}

int MvState::duplicateTile(int i)
{
	const QRectF r = tileRect(i);
	if (r.isNull())
		return -1;
	return addTile(r.translated(40, 40), tileKind(i), tileName(i));
}

void MvState::replaceTiles(std::vector<MvTile> &&next)
{
	std::vector<MvTile> old;
	{
		std::lock_guard<std::mutex> lock(mtx);
		old.swap(tiles_);
		tiles_ = std::move(next);
	}
	for (MvTile &t : old)
		releaseContent(t);
}

void MvState::applyPreset(size_t presetIdx)
{
	const auto &presets = mvPresets();
	if (presetIdx >= presets.size())
		return;
	const QStringList names = sceneNames();
	int n = 0;

	std::vector<MvTile> next;
	for (const auto &pt : presets[presetIdx].tiles) {
		MvTile t;
		if (pt.second == MV_SOURCE)
			makeContent(t, n < names.size() ? MV_SOURCE : MV_EMPTY,
				    n < names.size() ? names[n] : QString());
		else
			makeContent(t, pt.second, QString());
		if (pt.second == MV_SOURCE)
			n++;
		t.rect = pt.first;
		next.push_back(t);
	}
	replaceTiles(std::move(next));
	save();
}

void MvState::clearAll()
{
	replaceTiles(std::vector<MvTile>());
	save();
}

void MvState::setShowLabels(bool v)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		showLabels_ = v;
	}
	save();
}

void MvState::refreshFrontend()
{
	const bool studio = obs_frontend_preview_program_mode_active();
	obs_source_t *prog = obs_frontend_get_current_scene();
	obs_source_t *prev = studio ? obs_frontend_get_current_preview_scene() : obs_source_get_ref(prog);

	obs_weak_source_t *wProg = prog ? obs_source_get_weak_source(prog) : nullptr;
	obs_weak_source_t *wPrev = prev ? obs_source_get_weak_source(prev) : nullptr;
	obs_source_release(prog);
	obs_source_release(prev);
	{
		std::lock_guard<std::mutex> lock(mtx);
		std::swap(program_, wProg);
		std::swap(preview_, wPrev);
	}
	obs_weak_source_release(wProg);
	obs_weak_source_release(wPrev);
}

void MvState::createTopLabels()
{
	obs_source_t *p = createLabel(QString::fromUtf8(obs_module_text("Preview")));
	obs_source_t *g = createLabel(QString::fromUtf8(obs_module_text("Program")));
	{
		std::lock_guard<std::mutex> lock(mtx);
		std::swap(previewLabel_, p);
		std::swap(programLabel_, g);
	}
	obs_source_release(p);
	obs_source_release(g);
}

/* ------------------------------------------------------------------ */
/* Sauvegarde (une disposition par collection de scènes)              */

static QString configPath()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("multiview.json");
	const QString p = QString::fromUtf8(path ? path : "");
	bfree(path);
	return p;
}

static QString currentCollection()
{
	char *c = obs_frontend_get_current_scene_collection();
	const QString s = QString::fromUtf8(c ? c : "");
	bfree(c);
	return s;
}

static QJsonObject readRoot()
{
	QFile f(configPath());
	if (!f.open(QIODevice::ReadOnly))
		return QJsonObject();
	return QJsonDocument::fromJson(f.readAll()).object();
}

void MvState::load()
{
	const QJsonObject cfg = readRoot()["collections"].toObject()[currentCollection()].toObject();
	loading_ = true;

	{
		std::lock_guard<std::mutex> lock(mtx);
		showLabels_ = cfg["showLabels"].toBool(true);
	}

	if (cfg.contains("tiles")) {
		std::vector<MvTile> next;
		for (const QJsonValue &v : cfg["tiles"].toArray()) {
			const QJsonObject o = v.toObject();
			MvTile t;
			makeContent(t, o["kind"].toInt(MV_EMPTY), o["name"].toString());
			t.rect = clampRect(QRectF(o["x"].toDouble(), o["y"].toDouble(), o["w"].toDouble(480),
						  o["h"].toDouble(270)));
			next.push_back(t);
		}
		replaceTiles(std::move(next));
		loaded_ = true;
		loading_ = false;
		return;
	}

	loaded_ = true;
	loading_ = false;

	if (cfg.contains("cells")) {
		/* ancienne version (grille) : on reprend les noms dans le modèle 3x6 */
		applyPreset(0);
		const QJsonArray cells = cfg["cells"].toArray();
		const int first = 2; // après Aperçu / Programme
		for (int i = 0; i < cells.size() && first + i < tileCount(); i++)
			setTileContent(first + i, cells[i].toString().isEmpty() ? MV_EMPTY : MV_SOURCE,
				       cells[i].toString());
		return;
	}

	applyPreset(0); // première fois
}

void MvState::save()
{
	if (!loaded_ || loading_)
		return;

	QJsonArray arr;
	const int n = tileCount();
	for (int i = 0; i < n; i++) {
		const QRectF r = tileRect(i);
		QJsonObject o;
		o["x"] = r.x();
		o["y"] = r.y();
		o["w"] = r.width();
		o["h"] = r.height();
		o["kind"] = tileKind(i);
		o["name"] = tileName(i);
		arr.append(o);
	}
	QJsonObject cfg;
	cfg["tiles"] = arr;
	cfg["showLabels"] = showLabels();

	QJsonObject root = readRoot();
	QJsonObject cols = root["collections"].toObject();
	cols[currentCollection()] = cfg;
	root["collections"] = cols;
	root["version"] = 3;

	QFile f(configPath());
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_WARNING, "could not write %s", f.fileName().toUtf8().constData());
		return;
	}
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MvState::shutdown()
{
	save();
	loaded_ = false;
	replaceTiles(std::vector<MvTile>());

	obs_weak_source_t *wp = nullptr, *wg = nullptr;
	obs_source_t *lp = nullptr, *lg = nullptr;
	{
		std::lock_guard<std::mutex> lock(mtx);
		std::swap(preview_, wp);
		std::swap(program_, wg);
		std::swap(previewLabel_, lp);
		std::swap(programLabel_, lg);
	}
	obs_weak_source_release(wp);
	obs_weak_source_release(wg);
	obs_source_release(lp);
	obs_source_release(lg);
}
