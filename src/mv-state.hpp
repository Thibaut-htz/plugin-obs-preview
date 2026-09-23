#pragma once

#include <obs.h>

#include <QRectF>
#include <QString>

#include <mutex>
#include <vector>

/* Canevas du multiview : toutes les cases sont placées en coordonnées 1920x1080. */
constexpr double MV_CANVAS_W = 1920.0;
constexpr double MV_CANVAS_H = 1080.0;

enum MvKind { MV_EMPTY = 0, MV_PREVIEW = 1, MV_PROGRAM = 2, MV_SOURCE = 3 };

struct MvTile {
	QRectF rect; // coordonnées canevas
	int kind = MV_EMPTY;
	QString name; // scène ou source (MV_SOURCE)
	obs_weak_source_t *weak = nullptr;
	obs_source_t *label = nullptr;
};

/* Copie « sûre » pour le thread graphique (références fortes). */
struct MvSnapshot {
	bool showLabels = true;
	struct Item {
		QRectF rect;
		int kind = MV_EMPTY;
		obs_source_t *src = nullptr;
		obs_source_t *label = nullptr;
	};
	std::vector<Item> tiles;
	obs_source_t *previewScene = nullptr;
	obs_source_t *programScene = nullptr;
	obs_source_t *previewLabel = nullptr;
	obs_source_t *programLabel = nullptr;
	void release();
};

struct MvPreset {
	const char *key; // clé de traduction
	std::vector<std::pair<QRectF, int>> tiles;
};
const std::vector<MvPreset> &mvPresets();

/* État global partagé (dock + plein écran). Fonctions publiques : thread UI,
 * sauf snapshot() (thread graphique). */
class MvState {
public:
	static MvState &get();

	MvSnapshot snapshot();

	int tileCount();
	QRectF tileRect(int i);
	int tileKind(int i);
	QString tileName(int i);
	bool showLabels();

	int addTile(const QRectF &rect, int kind = MV_EMPTY, const QString &name = QString());
	void removeTile(int i);
	void setTileRect(int i, const QRectF &rect, bool saveNow = true);
	void setTileContent(int i, int kind, const QString &name = QString());
	int raiseTile(int i);
	int duplicateTile(int i);
	void applyPreset(size_t presetIdx);
	void clearAll();
	void setShowLabels(bool v);

	void refreshFrontend();
	void createTopLabels();
	void load();
	void save();
	void shutdown();

private:
	MvState() = default;
	void replaceTiles(std::vector<MvTile> &&next);

	std::mutex mtx;
	std::vector<MvTile> tiles_;
	bool showLabels_ = true;
	obs_weak_source_t *preview_ = nullptr;
	obs_weak_source_t *program_ = nullptr;
	obs_source_t *previewLabel_ = nullptr;
	obs_source_t *programLabel_ = nullptr;
	bool loading_ = false;
	bool loaded_ = false;
};
