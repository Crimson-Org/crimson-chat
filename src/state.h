#pragma once

#include "common/window-geometry.h"
#include "model.h"

void load_window_geometry_state(WindowGeometry *geometry);
void save_window_geometry_state(const WindowGeometry *geometry);
/* The network the rail shows and the open room, kept for the next launch */
void save_window_choice(const char *network_key, CrimsonRoom *room);
void load_window_choice(char **network_key, char **room_network, char **room_name);
/* The width the side panel was dragged to, or the fallback before it ever was */
void save_sidebar_width(int width);
/* Servers whose offer to register was closed, so it stays away */
void save_registration_dismissed(const char *network_key);
gboolean is_registration_dismissed(const char *network_key);
int load_sidebar_width(int fallback);
