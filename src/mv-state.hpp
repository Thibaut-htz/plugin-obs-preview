#pragma once

#include <obs.h>

#include <QRectF>
#include <QString>

#include <mutex>
#include <vector>

/* Une case du multiview : une scène OU une source, + son étiquette texte. */
struct MvCell {
	QString name;
	obs_weak_source_t *weak = nullptr;
	obs_source_t *label = nullptr; // source texte privée (nom affiché)
};

/* Copie « sûre » de l'état pour le thread de rendu (références fortes). */
struct MvSnapshot {
	int rows = 0, cols = 0;
	bool showTop = true, showLabels = true;
	struct Item {
		obs_source_t *src = nullptr;
		obs_source_t *label = nullptr;
	};
	std::vector<Item> cells;
	obs_source_t *previewScene = nullptr;
	obs_source_t *programScene = nullptr;
	obs_source_t *previewLabel = nullptr;
	obs_source_t *programLabel = nullptr;
	void release();
};

struct MvLayout {
	QRectF preview, program;
	std::vector<QRectF> cells;
};

MvLayout mvComputeLayout(double w, double h, int rows, int cols, bool showTop);

/* État global partagé par le dock et les fenêtres plein écran.
 * Toutes les fonctions publiques (sauf snapshot) : thread UI seulement. */
class MvState {
public:
	static MvState &get();

	MvSnapshot snapshot(); // appelé depuis le thread graphique

	int rows();
	int cols();
	bool showTop();
	bool showLabels();
	QString cellName(int idx);
	int cellCount();

	void setCell(int idx, const QString &name);
	void swapCells(int a, int b);
	void setGrid(int rows, int cols);
	void setShowTop(bool v);
	void setShowLabels(bool v);
	void autoFill();
	void clearCells();

	void refreshFrontend(); // met à jour preview/programme
	void createTopLabels();
	void load(); // charge la config de la collection de scènes courante
	void save();
	void shutdown(); // libère tout (fermeture d'OBS / changement de collection)

private:
	MvState() = default;
	void setCellInternal(int idx, const QString &name);
	void replaceCells(std::vector<MvCell> &&newCells);

	std::mutex mtx;
	int rows_ = 3, cols_ = 6;
	bool showTop_ = true, showLabels_ = true;
	std::vector<MvCell> cells_;
	obs_weak_source_t *preview_ = nullptr;
	obs_weak_source_t *program_ = nullptr;
	obs_source_t *previewLabel_ = nullptr;
	obs_source_t *programLabel_ = nullptr;
	bool loading_ = false;
	bool loaded_ = false;
};
