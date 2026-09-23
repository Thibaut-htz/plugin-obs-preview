# Sanna Multiview (plugin OBS)

Multiview 1920×1080 entièrement personnalisable pour OBS 30.1+.

- **Docks → Multiview Sanna**, ou **Outils → Multiview Sanna (plein écran)**
- Clic = scène en Aperçu · double-clic = Transition
- **E** = mode édition : glisser pour déplacer, coin bas-droit pour redimensionner (16:9, **Maj** = libre, **Alt** = sans aimant), **Suppr** = supprimer la case
- Clic droit : contenu (Aperçu, Programme, scène, source), taille, dupliquer, supprimer, ajouter une case, modèles, plein écran
- Disposition sauvée par collection de scènes

## Build (Windows) — le plus simple : GitHub Actions
1. Crée un repo GitHub (ex. `sanna-preview-layouts`) et pousse ce dossier sur `main`.
2. Onglet **Actions** → le workflow *Push* build tout seul (~10 min).
3. Télécharge l'artifact `sanna-preview-layouts-1.0.0-windows-x64-…` → zip avec `obs-plugins/64bit/sanna-preview-layouts.dll` + `data/obs-plugins/sanna-preview-layouts/`.
4. Copie le contenu dans `C:\Program Files\obs-studio\` (ou `C:\ProgramData\obs-studio\plugins\sanna-preview-layouts\bin\64bit\` + `...\data\`), redémarre OBS.

Pour une release avec zip + installeur : `git tag 1.0.0 && git push --tags`.

## Build local (Windows)
Visual Studio 2022 + CMake 3.28+ :
```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Compatible OBS 30.1+ (Qt6).
