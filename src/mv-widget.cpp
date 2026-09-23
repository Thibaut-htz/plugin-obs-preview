#include "mv-widget.hpp"
#include "mv-state.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/vec4.h>
#ifdef __linux__
#include <obs-nix-platform.h>
#endif

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <vector>

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static std::vector<QPointer<MultiviewWidget>> &fullscreenWindows()
{
	static std::vector<QPointer<MultiviewWidget>> w;
	return w;
}

/* Couleurs au format 0xAABBGGRR */
static const uint32_t COLOR_PROGRAM = 0xFF0000D0;  // rouge
static const uint32_t COLOR_PREVIEW = 0xFF00C000;  // vert
static const uint32_t COLOR_DRAGFROM = 0xFF00D7FF; // jaune
static const uint32_t COLOR_DRAGOVER = 0xFFFFA000; // bleu
static const uint32_t COLOR_CELL_BG = 0xFF000000;
static const uint32_t COLOR_EMPTY_BG = 0xFF1C1C1C;
static const uint32_t COLOR_LABEL_BG = 0xA0000000;

/* ------------------------------------------------------------------ */
/* Dessin                                                             */

static void drawBox(float x, float y, float w, float h, uint32_t color)
{
	if (w < 1.0f || h < 1.0f)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(param, &c);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, (uint32_t)w, (uint32_t)h);
	gs_matrix_pop();
}

static void drawBox(const QRectF &r, uint32_t color)
{
	drawBox((float)r.x(), (float)r.y(), (float)r.width(), (float)r.height(), color);
}

/* Rend une source dans un rectangle, en gardant son ratio (viewport = découpe propre). */
static void drawSourceFit(obs_source_t *src, const QRectF &r, uint32_t sw, uint32_t sh)
{
	if (!src || !sw || !sh || r.width() < 2 || r.height() < 2)
		return;
	const double scale = std::min(r.width() / sw, r.height() / sh);
	const double dw = sw * scale, dh = sh * scale;
	const double dx = r.x() + (r.width() - dw) / 2.0;
	const double dy = r.y() + (r.height() - dh) / 2.0;

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport((int)dx, (int)dy, (int)dw, (int)dh);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	obs_source_video_render(src);
	gs_projection_pop();
	gs_viewport_pop();
}

static void drawLabel(obs_source_t *label, const QRectF &cell, double heightRatio)
{
	if (!label)
		return;
	const uint32_t lw = obs_source_get_width(label);
	const uint32_t lh = obs_source_get_height(label);
	if (!lw || !lh)
		return;

	const double targetH = std::max(10.0, cell.height() * heightRatio);
	double sc = targetH / lh;
	if (lw * sc > cell.width() * 0.9)
		sc = cell.width() * 0.9 / lw;
	const double w = lw * sc, h = lh * sc;
	const double pad = h * 0.25;
	const double x = cell.x() + (cell.width() - w) / 2.0;
	const double y = cell.y() + cell.height() - h - pad * 2.0;

	drawBox((float)(x - pad), (float)(y - pad * 0.5), (float)(w + pad * 2), (float)(h + pad), COLOR_LABEL_BG);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f((float)x, (float)y, 0.0f);
	gs_matrix_scale3f((float)sc, (float)sc, 1.0f);
	obs_source_video_render(label);
	gs_matrix_pop();
}

static void drawFramed(const QRectF &r, uint32_t border, float thickness)
{
	if (border) {
		drawBox(r, border);
		drawBox(r.adjusted(thickness, thickness, -thickness, -thickness), COLOR_CELL_BG);
	} else {
		drawBox(r.adjusted(1, 1, -1, -1), COLOR_CELL_BG);
	}
}

void MultiviewWidget::drawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiviewWidget *>(data);
	MvSnapshot s = MvState::get().snapshot();
	const MvLayout l = mvComputeLayout(cx, cy, s.rows, s.cols, s.showTop);

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const uint32_t baseW = ovi.base_width ? ovi.base_width : 1920;
	const uint32_t baseH = ovi.base_height ? ovi.base_height : 1080;
	const bool studio = s.previewScene && s.previewScene != s.programScene;
	const float thick = std::max(2.0f, (float)cy / 270.0f);

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	/* --- Haut : Aperçu + Programme --- */
	if (s.showTop) {
		drawFramed(l.preview, COLOR_PREVIEW, thick);
		drawSourceFit(s.previewScene, l.preview.adjusted(thick, thick, -thick, -thick), baseW, baseH);
		if (s.showLabels)
			drawLabel(s.previewLabel, l.preview, 0.07);

		drawFramed(l.program, COLOR_PROGRAM, thick);
		obs_source_t *out = obs_get_output_source(0); // inclut les transitions en cours
		drawSourceFit(out, l.program.adjusted(thick, thick, -thick, -thick), baseW, baseH);
		obs_source_release(out);
		if (s.showLabels)
			drawLabel(s.programLabel, l.program, 0.07);
	}

	/* --- Grille --- */
	const int from = self->dragFrom.load();
	const int over = self->dragOver.load();
	for (size_t i = 0; i < l.cells.size() && i < s.cells.size(); i++) {
		const QRectF &r = l.cells[i];
		obs_source_t *src = s.cells[i].src;

		uint32_t border = 0;
		if ((int)i == from)
			border = COLOR_DRAGFROM;
		else if ((int)i == over)
			border = COLOR_DRAGOVER;
		else if (src && src == s.programScene)
			border = COLOR_PROGRAM;
		else if (src && studio && src == s.previewScene)
			border = COLOR_PREVIEW;

		drawFramed(r, border, thick);
		const QRectF inner = r.adjusted(thick, thick, -thick, -thick);
		if (!src) {
			drawBox(inner, COLOR_EMPTY_BG);
			continue;
		}

		const bool isScene = obs_scene_from_source(src) != nullptr;
		const uint32_t sw = isScene ? baseW : obs_source_get_width(src);
		const uint32_t sh = isScene ? baseH : obs_source_get_height(src);
		drawSourceFit(src, inner, sw, sh);
		if (s.showLabels)
			drawLabel(s.cells[i].label, inner, 0.13);
	}

	gs_blend_state_pop();
	gs_projection_pop();
	gs_viewport_pop();

	s.release();
}

/* ------------------------------------------------------------------ */
/* Widget / display natif                                             */

MultiviewWidget::MultiviewWidget(QWidget *parent, bool fullscreen) : QWidget(parent), fullscreenWindow(fullscreen)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMouseTracking(false);
	setFocusPolicy(Qt::StrongFocus);
	setMinimumSize(160, 90);
	if (fullscreen) {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(T("DockTitle"));
		setCursor(Qt::ArrowCursor);
	}
}

MultiviewWidget::~MultiviewWidget()
{
	destroyDisplay();
}

void MultiviewWidget::createDisplay()
{
	if (display || !isVisible() || !windowHandle())
		return;

	const qreal dpr = devicePixelRatioF();
	const uint32_t w = (uint32_t)std::max(1.0, width() * dpr);
	const uint32_t h = (uint32_t)std::max(1.0, height() * dpr);

	gs_init_data info = {};
	info.cx = w;
	info.cy = h;
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
	info.window.hwnd = (void *)winId();
#elif defined(__linux__) || defined(__FreeBSD__)
	info.window.id = (uint32_t)winId();
	info.window.display = obs_get_nix_platform_display();
#else
	return; // macOS non géré dans cette version
#endif

	display = obs_display_create(&info, 0xFF4C4C4C);
	if (display)
		obs_display_add_draw_callback(display, drawCallback, this);
}

void MultiviewWidget::destroyDisplay()
{
	if (!display)
		return;
	obs_display_remove_draw_callback(display, drawCallback, this);
	obs_display_destroy(display);
	display = nullptr;
}

bool MultiviewWidget::event(QEvent *e)
{
	if (e->type() == QEvent::PlatformSurface) {
		auto *pe = static_cast<QPlatformSurfaceEvent *>(e);
		if (pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			destroyDisplay();
		else if (pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
			QTimer::singleShot(0, this, &MultiviewWidget::createDisplay);
	} else if (e->type() == QEvent::WinIdChange) {
		destroyDisplay();
		QTimer::singleShot(0, this, &MultiviewWidget::createDisplay);
	}
	return QWidget::event(e);
}

void MultiviewWidget::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	QTimer::singleShot(0, this, &MultiviewWidget::createDisplay);
}

void MultiviewWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	if (display) {
		const qreal dpr = devicePixelRatioF();
		obs_display_resize(display, (uint32_t)std::max(1.0, width() * dpr),
				   (uint32_t)std::max(1.0, height() * dpr));
	} else {
		createDisplay();
	}
}

/* ------------------------------------------------------------------ */
/* Souris                                                             */

int MultiviewWidget::cellAt(const QPoint &pos) const
{
	MvState &st = MvState::get();
	const qreal dpr = devicePixelRatioF();
	const MvLayout l = mvComputeLayout(width() * dpr, height() * dpr, st.rows(), st.cols(), st.showTop());
	const QPointF p(pos.x() * dpr, pos.y() * dpr);
	for (size_t i = 0; i < l.cells.size(); i++)
		if (l.cells[i].contains(p))
			return (int)i;
	return -1;
}

obs_source_t *MultiviewWidget::cellSource(int idx) const
{
	const QString name = MvState::get().cellName(idx);
	if (name.isEmpty())
		return nullptr;
	return obs_get_source_by_name(name.toUtf8().constData());
}

void MultiviewWidget::selectCell(int idx, bool transition)
{
	obs_source_t *src = cellSource(idx);
	if (!src)
		return;
	if (obs_scene_from_source(src)) {
		if (obs_frontend_preview_program_mode_active()) {
			obs_frontend_set_current_preview_scene(src);
			if (transition)
				obs_frontend_preview_program_trigger_transition();
		} else {
			obs_frontend_set_current_scene(src);
		}
	}
	obs_source_release(src);
}

void MultiviewWidget::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		pressCell = cellAt(e->pos());
		pressPos = e->pos();
		dragging = false;
	}
	QWidget::mousePressEvent(e);
}

void MultiviewWidget::mouseMoveEvent(QMouseEvent *e)
{
	if ((e->buttons() & Qt::LeftButton) && pressCell >= 0) {
		if (!dragging && (e->pos() - pressPos).manhattanLength() > 10) {
			dragging = true;
			dragFrom = pressCell;
			setCursor(Qt::ClosedHandCursor);
		}
		if (dragging) {
			const int over = cellAt(e->pos());
			dragOver = (over == pressCell) ? -1 : over;
		}
	}
	QWidget::mouseMoveEvent(e);
}

void MultiviewWidget::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		if (dragging) {
			const int over = cellAt(e->pos());
			if (over >= 0 && over != pressCell)
				MvState::get().swapCells(pressCell, over);
			unsetCursor();
		} else if (pressCell >= 0) {
			selectCell(pressCell, false);
		}
		dragging = false;
		dragFrom = -1;
		dragOver = -1;
		pressCell = -1;
	}
	QWidget::mouseReleaseEvent(e);
}

void MultiviewWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		const int idx = cellAt(e->pos());
		if (idx >= 0)
			selectCell(idx, true);
	}
	QWidget::mouseDoubleClickEvent(e);
}

void MultiviewWidget::keyPressEvent(QKeyEvent *e)
{
	if (fullscreenWindow && e->key() == Qt::Key_Escape) {
		close();
		return;
	}
	QWidget::keyPressEvent(e);
}

/* ------------------------------------------------------------------ */
/* Menu clic droit                                                    */

static bool collectVideoSource(void *param, obs_source_t *src)
{
	auto *list = static_cast<QStringList *>(param);
	const uint32_t flags = obs_source_get_output_flags(src);
	if ((flags & OBS_SOURCE_VIDEO) && obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT)
		list->append(QString::fromUtf8(obs_source_get_name(src)));
	return true;
}

void MultiviewWidget::contextMenuEvent(QContextMenuEvent *e)
{
	MvState &st = MvState::get();
	const int idx = cellAt(e->pos());
	QMenu menu(this);

	if (idx >= 0) {
		const QString current = st.cellName(idx);
		menu.addSection(current.isEmpty() ? T("EmptyCell") : current);

		QMenu *scenesMenu = menu.addMenu(T("AssignScene"));
		obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			const QString name = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
			QAction *a = scenesMenu->addAction(name);
			a->setCheckable(true);
			a->setChecked(name == current);
			connect(a, &QAction::triggered, this, [idx, name]() { MvState::get().setCell(idx, name); });
		}
		obs_frontend_source_list_free(&scenes);

		QMenu *sourcesMenu = menu.addMenu(T("AssignSource"));
		QStringList sources;
		obs_enum_sources(collectVideoSource, &sources);
		sources.sort(Qt::CaseInsensitive);
		for (const QString &name : sources) {
			QAction *a = sourcesMenu->addAction(name);
			a->setCheckable(true);
			a->setChecked(name == current);
			connect(a, &QAction::triggered, this, [idx, name]() { MvState::get().setCell(idx, name); });
		}
		if (sources.isEmpty())
			sourcesMenu->setEnabled(false);

		QAction *clear = menu.addAction(T("ClearCell"));
		clear->setEnabled(!current.isEmpty());
		connect(clear, &QAction::triggered, this, [idx]() { MvState::get().setCell(idx, QString()); });
		menu.addSeparator();
	}

	/* Grille */
	QMenu *gridMenu = menu.addMenu(T("Grid"));
	QMenu *rowsMenu = gridMenu->addMenu(T("Rows"));
	for (int r = 1; r <= 8; r++) {
		QAction *a = rowsMenu->addAction(QString::number(r));
		a->setCheckable(true);
		a->setChecked(r == st.rows());
		connect(a, &QAction::triggered, this, [r]() { MvState::get().setGrid(r, MvState::get().cols()); });
	}
	QMenu *colsMenu = gridMenu->addMenu(T("Columns"));
	for (int c = 1; c <= 10; c++) {
		QAction *a = colsMenu->addAction(QString::number(c));
		a->setCheckable(true);
		a->setChecked(c == st.cols());
		connect(a, &QAction::triggered, this, [c]() { MvState::get().setGrid(MvState::get().rows(), c); });
	}

	QAction *top = menu.addAction(T("ShowTop"));
	top->setCheckable(true);
	top->setChecked(st.showTop());
	connect(top, &QAction::toggled, this, [](bool v) { MvState::get().setShowTop(v); });

	QAction *labels = menu.addAction(T("ShowLabels"));
	labels->setCheckable(true);
	labels->setChecked(st.showLabels());
	connect(labels, &QAction::toggled, this, [](bool v) { MvState::get().setShowLabels(v); });

	QAction *fill = menu.addAction(T("AutoFill"));
	connect(fill, &QAction::triggered, this, [this]() {
		if (QMessageBox::question(this, T("DockTitle"), T("ConfirmAutoFill")) == QMessageBox::Yes)
			MvState::get().autoFill();
	});
	QAction *clearAll = menu.addAction(T("ClearAll"));
	connect(clearAll, &QAction::triggered, this, [this]() {
		if (QMessageBox::question(this, T("DockTitle"), T("ConfirmClearAll")) == QMessageBox::Yes)
			MvState::get().clearCells();
	});

	menu.addSeparator();
	if (fullscreenWindow) {
		QAction *close = menu.addAction(T("CloseFullscreen"));
		connect(close, &QAction::triggered, this, &QWidget::close);
	} else {
		QMenu *fs = menu.addMenu(T("OpenFullscreen"));
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QScreen *scr = screens[i];
			const QRect g = scr->geometry();
			QAction *a = fs->addAction(QStringLiteral("%1 %2 — %3 (%4x%5)")
							   .arg(T("Screen"))
							   .arg(i + 1)
							   .arg(scr->name())
							   .arg(g.width())
							   .arg(g.height()));
			connect(a, &QAction::triggered, this, [scr]() { MultiviewWidget::openFullscreen(scr); });
		}
	}

	menu.exec(e->globalPos());
}

/* ------------------------------------------------------------------ */
/* Plein écran                                                        */

MultiviewWidget *MultiviewWidget::openFullscreen(QScreen *screen)
{
	auto *w = new MultiviewWidget(nullptr, true);
	w->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
	if (screen) {
		w->setGeometry(screen->geometry());
		w->winId(); // crée la fenêtre native pour pouvoir choisir l'écran
		if (w->windowHandle())
			w->windowHandle()->setScreen(screen);
	}
	w->showFullScreen();
	w->activateWindow();

	auto &list = fullscreenWindows();
	list.erase(std::remove_if(list.begin(), list.end(),
				  [](const QPointer<MultiviewWidget> &p) { return p.isNull(); }),
		   list.end());
	list.emplace_back(w);
	return w;
}

void MultiviewWidget::closeAllFullscreen()
{
	for (auto &p : fullscreenWindows())
		if (p)
			p->close();
	fullscreenWindows().clear();
}
