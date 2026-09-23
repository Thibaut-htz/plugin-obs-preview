/*
Sanna Multiview
Copyright (C) 2026 Thibaut - Sanna Studio Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>
#include <QVBoxLayout>

#include "mv-state.hpp"
#include "mv-widget.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define DOCK_ID "sanna_multiview"

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	MvState &st = MvState::get();
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		st.createTopLabels();
		st.refreshFrontend();
		st.load();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		st.refreshFrontend();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		st.shutdown();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		st.createTopLabels();
		st.refreshFrontend();
		st.load();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		MultiviewWidget::closeAllFullscreen();
		st.shutdown();
		break;
	default:
		break;
	}
}

static void openFullscreenFromMenu(void *)
{
	/* par défaut : un écran autre que celui de la fenêtre OBS */
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	QScreen *mainScreen = main ? main->screen() : nullptr;
	QScreen *target = nullptr;
	for (QScreen *s : QGuiApplication::screens())
		if (s != mainScreen) {
			target = s;
			break;
		}
	MultiviewWidget::openFullscreen(target ? target : mainScreen);
}

bool obs_module_load(void)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		obs_log(LOG_ERROR, "main window not found, dock not created");
		return true;
	}

	auto *container = new QWidget(mainWindow);
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(new MultiviewWidget(container));
	container->setMinimumSize(320, 180);

	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("DockTitle"), container)) {
		obs_log(LOG_ERROR, "could not add dock");
		delete container;
	}

	obs_frontend_add_tools_menu_item(obs_module_text("ToolsMenu"), openFullscreenFromMenu, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);

	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
