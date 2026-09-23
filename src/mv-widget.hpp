#pragma once

#include <obs.h>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <atomic>

class QScreen;

/* Affichage du multiview (dock ET plein écran). */
class MultiviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit MultiviewWidget(QWidget *parent = nullptr, bool fullscreenWindow = false);
	~MultiviewWidget() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

	static MultiviewWidget *openFullscreen(QScreen *screen);
	static void closeAllFullscreen();

protected:
	bool event(QEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *) override {}
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private slots:
	void createDisplay();

private:
	enum DragMode { DragNone, DragMove, DragResize };

	static void drawCallback(void *data, uint32_t cx, uint32_t cy);
	void destroyDisplay();
	QPointF toCanvas(const QPoint &p) const;
	double viewScale() const; // pixels écran par pixel canevas
	int tileAt(const QPointF &c) const;
	bool onHandle(int tile, const QPointF &c) const;
	void selectTile(int tile, bool transition);
	void setEditMode(bool on);

	obs_display_t *display = nullptr;
	bool fullscreenWindow = false;

	std::atomic<bool> editMode{false};
	std::atomic<int> selected{-1};

	DragMode dragMode = DragNone;
	int pressTile = -1;
	QPointF pressCanvas;
	QRectF pressRect;
	bool moved = false;
};
