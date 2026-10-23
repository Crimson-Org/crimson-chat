#include "common/key-store.h"
#include "state.h"

#define STATE_FILE "window.ini"
#define GROUP "choice"
#define DEFAULT_WIDTH 1080
#define DEFAULT_HEIGHT 760


static GKeyFile *load_state(void) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	return load_key_file(path, G_KEY_FILE_NONE);
}


static void save_state(GKeyFile *file) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	save_key_file(file, path);
}


void load_window_geometry_state(WindowGeometry *geometry) {
	g_autoptr(GKeyFile) file = load_state();
	load_window_geometry(file, geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
}


void save_window_geometry_state(const WindowGeometry *geometry) {
	g_autoptr(GKeyFile) file = load_state();
	store_window_geometry(file, geometry);
	save_state(file);
}


void save_window_choice(const char *network_key, CrimsonRoom *room) {
	g_autoptr(GKeyFile) file = load_state();
	g_key_file_set_string(file, GROUP, "network", network_key ? network_key : "");
	if (room) {
		g_key_file_set_string(file, GROUP, "room-network", room->network->key);
		g_key_file_set_string(file, GROUP, "room", room->name);
	}
	save_state(file);
}


void load_window_choice(char **network_key, char **room_network, char **room_name) {
	g_autoptr(GKeyFile) file = load_state();
	*network_key = g_key_file_get_string(file, GROUP, "network", NULL);
	*room_network = g_key_file_get_string(file, GROUP, "room-network", NULL);
	*room_name = g_key_file_get_string(file, GROUP, "room", NULL);
}


void save_sidebar_width(int width) {
	g_autoptr(GKeyFile) file = load_state();
	g_key_file_set_integer(file, GROUP, "sidebar-width", width);
	save_state(file);
}


int load_sidebar_width(int fallback) {
	g_autoptr(GKeyFile) file = load_state();
	int width = g_key_file_get_integer(file, GROUP, "sidebar-width", NULL);
	return width > 0 ? width : fallback;
}


#define DISMISSED_GROUP "registration-dismissed"


void save_registration_dismissed(const char *network_key) {
	g_autoptr(GKeyFile) file = load_state();
	g_key_file_set_boolean(file, DISMISSED_GROUP, network_key, TRUE);
	save_state(file);
}


gboolean is_registration_dismissed(const char *network_key) {
	g_autoptr(GKeyFile) file = load_state();
	return g_key_file_get_boolean(file, DISMISSED_GROUP, network_key, NULL);
}
