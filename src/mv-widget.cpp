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
#include <cmath>
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

/* Couleurs 0xAABBGGRR */
static const uint32_t COLOR_PROGRAM = 0xFF0000D0;  // rouge
static const uint32_t COLOR_PREVIEW = 0xFF00C000;  // vert
static const uint32_t COLOR_SELECTED = 0xFF00D7FF; // jaune
static const uint32_t COLOR_EDIT = 0x90FFFFFF;     // contour en mode édition
static const uint32_t COLOR_HANDLE = 0xFFFFFFFF;
static const uint32_t COLOR_CANVAS = 0xFF101010;
static const uint32_t COLOR_TILE_BG = 0xFF000000;
static const uint32_t COLOR_EMPTY_BG = 0xFF262626;
static const uint32_t COLOR_LABEL_BG = 0xA0000000;

static const double SNAP = 10.0;

struct View {
	double s, ox, oy;
	QRectF map(const QRectF &r) const
	{
		return QRectF(ox + r.x() * s, oy + r.y() * s, r.width() * s, r.height() * s);
	}
};

static View computeView(double w, double h)
{
	const double s = std::max(0.0001, std::min(w / MV_CANVAS_W, h / MV_CANVAS_H));
	return {s, (w - MV_CANVAS_W * s) / 2.0, (h - MV_CANVAS_H * s) / 2.0};
}

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

static void drawOutline(const QRectF &r, uint32_t color, float t)
{
	drawBox((float)r.x(), (float)r.y(), (float)r.width(), t, color);
	drawBox((float)r.x(), (float)(r.bottom() - t), (float)r.width(), t, color);
	drawBox((float)r.x(), (float)(r.y() + t), t, (float)(r.height() - 2 * t), color);
	drawBox((float)(r.right() - t), (float)(r.y() + t), t, (float)(r.height() - 2 * t), color);
}

/* Rend une source dans un rectangle en gardant son ratio (viewport = découpe propre). */
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

static void drawLabel(obs_source_t *label, const QRectF &r, double targetH)
{
	if (!label)
		return;
	const uint32_t lw = obs_source_get_width(label);
	const uint32_t lh = obs_source_get_height(label);
	if (!lw || !lh || targetH < 4)
		return;

	double sc = targetH / lh;
	if (lw * sc > r.width() * 0.9)
		sc = r.width() * 0.9 / lw;
	const double w = lw * sc, h = lh * sc;
	const double pad = h * 0.25;
	const double x = r.x() + (r.width() - w) / 2.0;
	const double y = r.y() + r.height() - h - pad * 2.0;

	drawBox((float)(x - pad), (float)(y - pad * 0.5), (float)(w + pad * 2), (float)(h + pad), COLOR_LABEL_BG);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f((float)x, (float)y, 0.0f);
	gs_matrix_scale3f((float)sc, (float)sc, 1.0f);
	obs_source_video_render(label);
	gs_matrix_pop();
}

void MultiviewWidget::drawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiviewWidget *>(data);
	MvSnapshot s = MvState::get().snapshot();
	const View v = computeView(cx, cy);

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const uint32_t baseW = ovi.base_width ? ovi.base_width : 1920;
	const uint32_t baseH = ovi.base_height ? ovi.base_height : 1080;
	const bool studio = s.previewScene && s.previewScene != s.programScene;
	const bool edit = self->editMode.load();
	const int sel = self->selected.load();
	const float thick = (float)std::max(2.0, 4.0 * v.s);

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	drawBox(v.map(QRectF(0, 0, MV_CANVAS_W, MV_CANVAS_H)), COLOR_CANVAS);

	for (size_t i = 0; i < s.tiles.size(); i++) {
		const MvSnapshot::Item &t = s.tiles[i];
		const QRectF r = v.map(t.rect);
		const QRectF inner = r.adjusted(thick, thick, -thick, -thick);

		uint32_t border = 0;
		obs_source_t *content = nullptr;
		obs_source_t *label = nullptr;
		uint32_t sw = baseW, sh = baseH;
		bool release = false;

		switch (t.kind) {
		case MV_PREVIEW:
			border = COLOR_PREVIEW;
			content = s.previewScene;
			label = s.previewLabel;
			break;
		case MV_PROGRAM:
			border = COLOR_PROGRAM;
			content = obs_get_output_source(0); // inclut la transition en cours
			release = true;
			label = s.programLabel;
			break;
		case MV_SOURCE:
			content = t.src;
			label = t.label;
			if (t.src && t.src == s.programScene)
				border = COLOR_PROGRAM;
			else if (t.src && studio && t.src == s.previewScene)
				border = COLOR_PREVIEW;
			if (t.src && !obs_scene_from_source(t.src)) {
				sw = obs_source_get_width(t.src);
				sh = obs_source_get_height(t.src);
			}
			break;
		default:
			break;
		}

		if (border)
			drawBox(r, border);
		drawBox(border ? inner : r.adjusted(1, 1, -1, -1), content ? COLOR_TILE_BG : COLOR_EMPTY_BG);
		drawSourceFit(content, inner, sw, sh);
		if (release)
			obs_source_release(content);

		if (s.showLabels && content)
			drawLabel(label, inner, std::clamp(t.rect.height() * 0.12, 16.0, 44.0) * v.s);

		if (edit) {
			drawOutline(r, (int)i == sel ? COLOR_SELECTED : COLOR_EDIT,
				    (int)i == sel ? thick : std::max(1.0f, thick / 2));
			const float hs = (float)std::max(8.0, 24.0 * v.s);
			drawBox((float)(r.right() - hs), (float)(r.bottom() - hs), hs, hs,
				(int)i == sel ? COLOR_SELECTED : COLOR_HANDLE);
		}
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
	setFocusPolicy(Qt::StrongFocus);
	setMinimumSize(160, 90);
	if (fullscreen) {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(T("DockTitle"));
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
	gs_init_data info = {};
	info.cx = (uint32_t)std::max(1.0, width() * dpr);
	info.cy = (uint32_t)std::max(1.0, height() * dpr);
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
	info.window.hwnd = (void *)winId();
#elif defined(__linux__) || defined(__FreeBSD__)
	info.window.id = (uint32_t)winId();
	info.window.display = obs_get_nix_platform_display();
#else
	return; // macOS non géré
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
/* Coordonnées                                                        */

double MultiviewWidget::viewScale() const
{
	const qreal dpr = devicePixelRatioF();
	return computeView(width() * dpr, height() * dpr).s / dpr;
}

QPointF MultiviewWidget::toCanvas(const QPoint &p) const
{
	const qreal dpr = devicePixelRatioF();
	const View v = computeView(width() * dpr, height() * dpr);
	return QPointF((p.x() * dpr - v.ox) / v.s, (p.y() * dpr - v.oy) / v.s);
}

int MultiviewWidget::tileAt(const QPointF &c) const
{
	MvState &st = MvState::get();
	for (int i = st.tileCount() - 1; i >= 0; i--) // la plus au-dessus d'abord
		if (st.tileRect(i).contains(c))
			return i;
	return -1;
}

bool MultiviewWidget::onHandle(int tile, const QPointF &c) const
{
	const QRectF r = MvState::get().tileRect(tile);
	const double hs = std::max(30.0, 16.0 / viewScale()); // zone de prise en coin bas-droit
	return QRectF(r.right() - hs, r.bottom() - hs, hs, hs).contains(c);
}

static double snapV(double v, bool enabled)
{
	return enabled ? std::round(v / SNAP) * SNAP : v;
}

/* ------------------------------------------------------------------ */
/* Souris / clavier                                                   */

void MultiviewWidget::setEditMode(bool on)
{
	editMode = on;
	if (!on)
		selected = -1;
	setCursor(on ? Qt::SizeAllCursor : Qt::ArrowCursor);
}

void MultiviewWidget::selectTile(int tile, bool transition)
{
	MvState &st = MvState::get();
	if (st.tileKind(tile) != MV_SOURCE)
		return;
	obs_source_t *src = obs_get_source_by_name(st.tileName(tile).toUtf8().constData());
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
		pressCanvas = toCanvas(e->pos());
		pressTile = tileAt(pressCanvas);
		moved = false;
		dragMode = DragNone;
		if (editMode) {
			selected = pressTile;
			if (pressTile >= 0) {
				pressRect = MvState::get().tileRect(pressTile);
				dragMode = onHandle(pressTile, pressCanvas) ? DragResize : DragMove;
			}
		}
	}
	QWidget::mousePressEvent(e);
}

void MultiviewWidget::mouseMoveEvent(QMouseEvent *e)
{
	if (editMode && (e->buttons() & Qt::LeftButton) && pressTile >= 0 && dragMode != DragNone) {
		const QPointF d = toCanvas(e->pos()) - pressCanvas;
		const bool snap = !(e->modifiers() & Qt::AltModifier);
		QRectF r = pressRect;
		if (dragMode == DragMove) {
			r.moveTo(snapV(pressRect.x() + d.x(), snap), snapV(pressRect.y() + d.y(), snap));
		} else {
			const double w = std::max(80.0, snapV(pressRect.width() + d.x(), snap));
			double h;
			if (e->modifiers() & Qt::ShiftModifier) // Maj = taille libre
				h = std::max(45.0, snapV(pressRect.height() + d.y(), snap));
			else
				h = w * 9.0 / 16.0; // 16:9 par défaut
			r.setSize(QSizeF(w, h));
		}
		MvState::get().setTileRect(pressTile, r, false);
		moved = true;
	}
	QWidget::mouseMoveEvent(e);
}

void MultiviewWidget::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		if (editMode) {
			if (moved)
				MvState::get().save();
		} else if (pressTile >= 0) {
			selectTile(pressTile, false);
		}
		dragMode = DragNone;
		pressTile = -1;
		moved = false;
	}
	QWidget::mouseReleaseEvent(e);
}

void MultiviewWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && !editMode) {
		const int t = tileAt(toCanvas(e->pos()));
		if (t >= 0)
			selectTile(t, true);
	}
	QWidget::mouseDoubleClickEvent(e);
}

void MultiviewWidget::keyPressEvent(QKeyEvent *e)
{
	switch (e->key()) {
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		if (editMode && selected >= 0) {
			MvState::get().removeTile(selected);
			selected = -1;
			return;
		}
		break;
	case Qt::Key_E:
		setEditMode(!editMode);
		return;
	case Qt::Key_Escape:
		if (editMode) {
			setEditMode(false);
			return;
		}
		if (fullscreenWindow) {
			close();
			return;
		}
		break;
	default:
		break;
	}
	QWidget::keyPressEvent(e);
}

/* ------------------------------------------------------------------ */
/* Menu clic droit                                                    */

static bool collectVideoSource(void *param, obs_source_t *src)
{
	auto *list = static_cast<QStringList *>(param);
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) && obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT)
		list->append(QString::fromUtf8(obs_source_get_name(src)));
	return true;
}

static void addContentActions(QMenu *menu, QObject *ctx, int tile)
{
	MvState &st = MvState::get();
	const int kind = st.tileKind(tile);
	const QString current = kind == MV_SOURCE ? st.tileName(tile) : QString();

	QAction *prev = menu->addAction(T("Preview"));
	prev->setCheckable(true);
	prev->setChecked(kind == MV_PREVIEW);
	QObject::connect(prev, &QAction::triggered, ctx, [tile]() { MvState::get().setTileContent(tile, MV_PREVIEW); });

	QAction *prog = menu->addAction(T("Program"));
	prog->setCheckable(true);
	prog->setChecked(kind == MV_PROGRAM);
	QObject::connect(prog, &QAction::triggered, ctx, [tile]() { MvState::get().setTileContent(tile, MV_PROGRAM); });

	QMenu *scenesMenu = menu->addMenu(T("AssignScene"));
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const QString name = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
		QAction *a = scenesMenu->addAction(name);
		a->setCheckable(true);
		a->setChecked(name == current);
		QObject::connect(a, &QAction::triggered, ctx,
				 [tile, name]() { MvState::get().setTileContent(tile, MV_SOURCE, name); });
	}
	obs_frontend_source_list_free(&scenes);

	QMenu *sourcesMenu = menu->addMenu(T("AssignSource"));
	QStringList sources;
	obs_enum_sources(collectVideoSource, &sources);
	sources.sort(Qt::CaseInsensitive);
	for (const QString &name : sources) {
		QAction *a = sourcesMenu->addAction(name);
		a->setCheckable(true);
		a->setChecked(name == current);
		QObject::connect(a, &QAction::triggered, ctx,
				 [tile, name]() { MvState::get().setTileContent(tile, MV_SOURCE, name); });
	}
	sourcesMenu->setEnabled(!sources.isEmpty());

	QAction *clear = menu->addAction(T("ClearCell"));
	clear->setEnabled(kind != MV_EMPTY);
	QObject::connect(clear, &QAction::triggered, ctx, [tile]() { MvState::get().setTileContent(tile, MV_EMPTY); });
}

void MultiviewWidget::contextMenuEvent(QContextMenuEvent *e)
{
	MvState &st = MvState::get();
	const QPointF c = toCanvas(e->pos());
	const int tile = tileAt(c);
	QMenu menu(this);

	if (tile >= 0) {
		const int kind = st.tileKind(tile);
		QString title = kind == MV_PREVIEW   ? T("Preview")
				: kind == MV_PROGRAM ? T("Program")
				: kind == MV_SOURCE  ? st.tileName(tile)
						     : T("EmptyCell");
		menu.addSection(title);
		addContentActions(&menu, this, tile);
		menu.addSeparator();

		QMenu *size = menu.addMenu(T("TileSize"));
		const QList<QPair<QString, QSizeF>> sizes = {
			{QStringLiteral("1920 × 1080 (plein)"), QSizeF(1920, 1080)},
			{QStringLiteral("1440 × 810 (3/4)"), QSizeF(1440, 810)},
			{QStringLiteral("960 × 540 (1/2)"), QSizeF(960, 540)},
			{QStringLiteral("640 × 360 (1/3)"), QSizeF(640, 360)},
			{QStringLiteral("480 × 270 (1/4)"), QSizeF(480, 270)},
			{QStringLiteral("320 × 180 (1/6)"), QSizeF(320, 180)},
		};
		for (const auto &sz : sizes) {
			QAction *a = size->addAction(sz.first);
			const QSizeF s = sz.second;
			connect(a, &QAction::triggered, this, [tile, s]() {
				QRectF r = MvState::get().tileRect(tile);
				r.setSize(s);
				MvState::get().setTileRect(tile, r);
			});
		}

		QAction *dup = menu.addAction(T("Duplicate"));
		connect(dup, &QAction::triggered, this,
			[this, tile]() { selected = MvState::get().duplicateTile(tile); });
		QAction *raise = menu.addAction(T("BringToFront"));
		connect(raise, &QAction::triggered, this,
			[this, tile]() { selected = MvState::get().raiseTile(tile); });
		QAction *del = menu.addAction(T("DeleteTile"));
		connect(del, &QAction::triggered, this, [this, tile]() {
			MvState::get().removeTile(tile);
			selected = -1;
		});
	} else if (c.x() >= 0 && c.y() >= 0 && c.x() <= MV_CANVAS_W && c.y() <= MV_CANVAS_H) {
		QAction *add = menu.addAction(T("AddTileHere"));
		connect(add, &QAction::triggered, this, [this, c]() {
			const QRectF r(snapV(c.x(), true), snapV(c.y(), true), 480, 270);
			selected = MvState::get().addTile(r);
			if (!editMode)
				setEditMode(true);
		});
	}

	menu.addSeparator();
	QAction *edit = menu.addAction(T("EditMode"));
	edit->setCheckable(true);
	edit->setChecked(editMode);
	connect(edit, &QAction::toggled, this, [this](bool on) { setEditMode(on); });

	QMenu *presets = menu.addMenu(T("Presets"));
	const auto &list = mvPresets();
	for (size_t i = 0; i < list.size(); i++) {
		QAction *a = presets->addAction(T(list[i].key));
		connect(a, &QAction::triggered, this, [this, i]() {
			if (QMessageBox::question(this, T("DockTitle"), T("ConfirmPreset")) == QMessageBox::Yes) {
				MvState::get().applyPreset(i);
				selected = -1;
			}
		});
	}

	QAction *labels = menu.addAction(T("ShowLabels"));
	labels->setCheckable(true);
	labels->setChecked(st.showLabels());
	connect(labels, &QAction::toggled, this, [](bool v) { MvState::get().setShowLabels(v); });

	QAction *clearAll = menu.addAction(T("ClearAll"));
	connect(clearAll, &QAction::triggered, this, [this]() {
		if (QMessageBox::question(this, T("DockTitle"), T("ConfirmClearAll")) == QMessageBox::Yes) {
			MvState::get().clearAll();
			selected = -1;
		}
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
			QAction *a = fs->addAction(QStringLiteral("%1 %2 — %3 (%4×%5)")
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
		w->winId();
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
