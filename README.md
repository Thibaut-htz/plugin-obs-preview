# Sanna Preview Layouts (plugin OBS)

Dock OBS pour **sauver la disposition de tes sources** (position, taille, rotation, bounding box, crop, visibilité, ordre des calques) et **la rappeler sur la scène en Preview** en un clic.

- Studio Mode ON → agit sur la scène en **Preview**
- Studio Mode OFF → agit sur la scène courante
- Les layouts sont globaux : un layout sauvé sur la scène A peut s'appliquer sur la scène B. Les sources sont matchées **par nom** (les sources absentes sont ignorées).
- Stockage : `%APPDATA%\obs-studio\plugin_config\sanna-preview-layouts\layouts.json`

## Utilisation
1. `Docks` → **Layouts Preview**
2. Place tes sources dans la scène en Preview → tape un nom → **Sauver**
3. Sélectionne un layout → **Appliquer** (ou double-clic)
4. **Mettre à jour** = remplace le layout sélectionné par la disposition actuelle

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
