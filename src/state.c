#include "common/key-store.h"
#include "state.h"

#define STATE_FILE "window.ini"
#define GROUP "choice"
#define DEFAULT_WIDTH 1080
#define DEFAULT_HEIGHT 760
/* Saving waits this long so a run of choices is written once */
#define SAVE_DELAY_MS 1000

/* The file is read once and kept, and one worker writes it so the writes stay in order */
static GKeyFile *state;
static GThreadPool *writer;
static guint save_source;


static GKeyFile *load_state(void) {
	if (!state) {
		g_autofree char *path = get_state_file_path(STATE_FILE);
		state = load_key_file(path, G_KEY_FILE_NONE);
	}
	return g_key_file_ref(state);
}


static void write_state_text(gpointer text, gpointer data) {
	g_autofree char *contents = text;
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autofree char *folder = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;
	if (g_mkdir_with_parents(folder, 0700) != 0 || !g_file_set_contents(path, contents, -1, &error))
		g_warning("%s was not saved", path);
}


static gboolean save_state_now(gpointer data) {
	save_source = 0;
	if (!writer)
		writer = g_thread_pool_new(write_state_text, NULL, 1, FALSE, NULL);
	g_thread_pool_push(writer, g_key_file_to_data(state, NULL, NULL), NULL);
	return G_SOURCE_REMOVE;
}


/* Choosing a room never waits for the disk, since the file is written a moment later on the worker */
static void save_state(GKeyFile *file) {
	if (!save_source)
		save_source = g_timeout_add(SAVE_DELAY_MS, save_state_now, NULL);
}


void finish_saving_state(void) {
	if (save_source) {
		g_source_remove(save_source);
		save_state_now(NULL);
	}
	if (writer)
		g_thread_pool_free(g_steal_pointer(&writer), FALSE, TRUE);
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


void save_away_message(const char *message) {
	g_autoptr(GKeyFile) file = load_state();
	if (message)
		g_key_file_set_string(file, GROUP, "away", message);
	else
		g_key_file_remove_key(file, GROUP, "away", NULL);
	save_state(file);
}


char *load_away_message(void) {
	g_autoptr(GKeyFile) file = load_state();
	return g_key_file_get_string(file, GROUP, "away", NULL);
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
