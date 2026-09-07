/*
OBS Box Layouts
Copyright (C) 2026 OBS Box Layouts contributors

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
#include <plugin-support.h>

#include "box-layout.h"
#include "live-control-dock.h"
#include "media-dock.h"
#include "playlist-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_register_source(&box_layout_source_info);
	if (!media_dock_init())
		obs_log(LOG_WARNING, "could not register media monitor dock");
	if (!live_control_dock_init())
		obs_log(LOG_WARNING, "could not register live control dock");
	if (!playlist_dock_init())
		obs_log(LOG_WARNING, "could not register playlist dock");
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	live_control_dock_shutdown();
	playlist_dock_shutdown();
	media_dock_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
