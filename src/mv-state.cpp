#include "mv-state.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
	obs_data_set_int(font, "flags", 1); // bold

	obs_data_t *settings = obs_data_create();
	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.toUtf8().constData());
	obs_data_set_bool(settings, "outline", false);

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

static void releaseCell(MvCell &c)
{
	if (c.weak) {
		obs_source_t *src = obs_weak_source_get_source(c.weak);
		if (src) {
			obs_source_dec_showing(src);
			obs_source_release(src);
		}
		obs_weak_source_release(c.weak);
	}
	obs_source_release(c.label);
	c = MvCell();
}

static MvCell makeCell(const QString &name)
{
	MvCell c;
	if (name.isEmpty())
		return c;
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return c;
	c.name = name;
	c.weak = obs_source_get_weak_source(src);
	c.label = createLabel(name);
	obs_source_inc_showing(src); // pour que les médias jouent même hors programme
	obs_source_release(src);
	return c;
}

static double canvasAspect()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height)
		return (double)ovi.base_width / (double)ovi.base_height;
	return 16.0 / 9.0;
}

MvLayout mvComputeLayout(double w, double h, int rows, int cols, bool showTop)
{
	MvLayout l;
	if (w <= 0 || h <= 0 || rows <= 0 || cols <= 0)
		return l;

	const double aspect = canvasAspect();
	const double topH = showTop ? (w / 2.0) / aspect : 0.0;
	const double cellW = w / cols;
	const double cellH = cellW / aspect;
	const double totalH = topH + rows * cellH;
	const double s = totalH > h ? h / totalH : 1.0;

	const double W = w * s;
	const double x0 = (w - W) / 2.0;
	const double y0 = (h - totalH * s) / 2.0;

	if (showTop) {
		l.preview = QRectF(x0, y0, W / 2.0, topH * s);
		l.program = QRectF(x0 + W / 2.0, y0, W / 2.0, topH * s);
	}
	l.cells.reserve(rows * cols);
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			l.cells.emplace_back(x0 + c * cellW * s, y0 + topH * s + r * cellH * s, cellW * s, cellH * s);
	return l;
}

void MvSnapshot::release()
{
	for (Item &i : cells) {
		obs_source_release(i.src);
		obs_source_release(i.label);
	}
	cells.clear();
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
	s.rows = rows_;
	s.cols = cols_;
	s.showTop = showTop_;
	s.showLabels = showLabels_;
	s.cells.reserve(cells_.size());
	for (MvCell &c : cells_) {
		MvSnapshot::Item it;
		it.src = c.weak ? obs_weak_source_get_source(c.weak) : nullptr;
		it.label = obs_source_get_ref(c.label);
		s.cells.push_back(it);
	}
	s.previewScene = preview_ ? obs_weak_source_get_source(preview_) : nullptr;
	s.programScene = program_ ? obs_weak_source_get_source(program_) : nullptr;
	s.previewLabel = obs_source_get_ref(previewLabel_);
	s.programLabel = obs_source_get_ref(programLabel_);
	return s;
}

int MvState::rows()
{
	std::lock_guard<std::mutex> lock(mtx);
	return rows_;
}

int MvState::cols()
{
	std::lock_guard<std::mutex> lock(mtx);
	return cols_;
}

bool MvState::showTop()
{
	std::lock_guard<std::mutex> lock(mtx);
	return showTop_;
}

bool MvState::showLabels()
{
	std::lock_guard<std::mutex> lock(mtx);
	return showLabels_;
}

int MvState::cellCount()
{
	std::lock_guard<std::mutex> lock(mtx);
	return (int)cells_.size();
}

QString MvState::cellName(int idx)
{
	obs_source_t *src = nullptr;
	QString n;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (idx < 0 || idx >= (int)cells_.size())
			return QString();
		src = cells_[idx].weak ? obs_weak_source_get_source(cells_[idx].weak) : nullptr;
		n = cells_[idx].name;
	}
	/* le nom peut avoir changé (source renommée) */
	if (src)
		n = QString::fromUtf8(obs_source_get_name(src));
	obs_source_release(src); // hors verrou
	return n;
}

void MvState::setCellInternal(int idx, const QString &name)
{
	MvCell fresh = makeCell(name); // hors verrou (peut toucher au graphique)
	MvCell old;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (idx >= 0 && idx < (int)cells_.size()) {
			old = cells_[idx];
			cells_[idx] = fresh;
			fresh = MvCell();
		}
	}
	releaseCell(old);
	releaseCell(fresh); // si idx invalide
}

void MvState::setCell(int idx, const QString &name)
{
	setCellInternal(idx, name);
	save();
}

void MvState::swapCells(int a, int b)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (a < 0 || b < 0 || a >= (int)cells_.size() || b >= (int)cells_.size() || a == b)
			return;
		std::swap(cells_[a], cells_[b]);
	}
	save();
}

void MvState::replaceCells(std::vector<MvCell> &&newCells)
{
	std::vector<MvCell> old;
	{
		std::lock_guard<std::mutex> lock(mtx);
		old.swap(cells_);
		cells_ = std::move(newCells);
	}
	for (MvCell &c : old)
		releaseCell(c);
}

void MvState::setGrid(int rows, int cols)
{
	rows = std::clamp(rows, 1, 8);
	cols = std::clamp(cols, 1, 10);

	std::vector<MvCell> current;
	int oldCols;
	{
		std::lock_guard<std::mutex> lock(mtx);
		current.swap(cells_);
		oldCols = cols_;
		rows_ = rows;
		cols_ = cols;
	}

	/* on garde chaque case à la même position (ligne, colonne) si elle existe encore */
	std::vector<MvCell> next(rows * cols);
	for (int i = 0; i < (int)current.size(); i++) {
		const int r = oldCols ? i / oldCols : 0;
		const int c = oldCols ? i % oldCols : 0;
		if (r < rows && c < cols) {
			next[r * cols + c] = current[i];
			current[i] = MvCell();
		}
	}
	for (MvCell &c : current)
		releaseCell(c);

	{
		std::lock_guard<std::mutex> lock(mtx);
		cells_ = std::move(next);
	}
	save();
}

void MvState::setShowTop(bool v)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		showTop_ = v;
	}
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

void MvState::clearCells()
{
	const int n = cellCount();
	std::vector<MvCell> empty(n);
	replaceCells(std::move(empty));
	save();
}

void MvState::autoFill()
{
	QStringList names;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const QString n = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
		if (n.startsWith(QStringLiteral("--"))) // séparateurs du genre "-----|Caméra|-----"
			continue;
		names << n;
	}
	obs_frontend_source_list_free(&scenes);

	int r = rows(), c = cols();
	while (r * c < names.size() && r < 8)
		r++;
	if (r != rows())
		setGrid(r, c);

	const int n = cellCount();
	loading_ = true;
	for (int i = 0; i < n; i++)
		setCellInternal(i, i < names.size() ? names[i] : QString());
	loading_ = false;
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
/* Sauvegarde : un réglage par collection de scènes                   */

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
	if (cfg.isEmpty()) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			showTop_ = true;
			showLabels_ = true;
		}
		setGrid(3, 6);
		loaded_ = true;
		loading_ = false;
		autoFill(); // première fois : on remplit avec les scènes
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mtx);
		showTop_ = cfg["showTop"].toBool(true);
		showLabels_ = cfg["showLabels"].toBool(true);
	}
	const int r = std::clamp(cfg["rows"].toInt(3), 1, 8);
	const int c = std::clamp(cfg["cols"].toInt(6), 1, 10);

	std::vector<MvCell> next(r * c);
	const QJsonArray arr = cfg["cells"].toArray();
	for (int i = 0; i < r * c && i < arr.size(); i++)
		next[i] = makeCell(arr[i].toString());
	{
		std::lock_guard<std::mutex> lock(mtx);
		rows_ = r;
		cols_ = c;
	}
	replaceCells(std::move(next));

	loaded_ = true;
	loading_ = false;
}

void MvState::save()
{
	if (!loaded_ || loading_)
		return;

	QJsonObject cfg;
	QJsonArray arr;
	const int n = cellCount();
	for (int i = 0; i < n; i++)
		arr.append(cellName(i));
	cfg["rows"] = rows();
	cfg["cols"] = cols();
	cfg["showTop"] = showTop();
	cfg["showLabels"] = showLabels();
	cfg["cells"] = arr;

	QJsonObject root = readRoot();
	QJsonObject cols = root["collections"].toObject();
	cols[currentCollection()] = cfg;
	root["collections"] = cols;
	root["version"] = 2;

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

	replaceCells(std::vector<MvCell>());

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
