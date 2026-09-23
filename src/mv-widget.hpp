#pragma once

#include <obs.h>

#include <QPoint>
#include <QWidget>

#include <atomic>

class QScreen;

/* Affichage du multiview (utilisé dans le dock ET en plein écran). */
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
	static void drawCallback(void *data, uint32_t cx, uint32_t cy);
	void destroyDisplay();
	int cellAt(const QPoint &pos) const;
	obs_source_t *cellSource(int idx) const; // nouvelle réf
	void selectCell(int idx, bool transition);

	obs_display_t *display = nullptr;
	bool fullscreenWindow = false;

	int pressCell = -1;
	QPoint pressPos;
	bool dragging = false;
	std::atomic<int> dragFrom{-1};
	std::atomic<int> dragOver{-1};
};
