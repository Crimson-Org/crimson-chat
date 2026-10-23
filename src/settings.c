#include <adwaita.h>

#include "common/key-store.h"
#include "common/themes.h"
#include "settings.h"

#define SETTINGS_FILE "settings.ini"
#define GROUP "chat"

static Settings settings = {
	.style = STYLE_SYSTEM,
	.shows_images = TRUE,
	.shows_typing = TRUE,
	.sends_typing = TRUE,
	.sends_read_markers = TRUE,
	.notifies_mentions = TRUE,
	.plays_sounds = TRUE,
	.notifies_private = TRUE,
	.closes_to_tray = TRUE,
	.uses_24_hours = TRUE,
};


Settings *get_settings(void) {
	return &settings;
}


static gboolean read_switch(GKeyFile *file, const char *key, gboolean fallback) {
	g_autoptr(GError) error = NULL;
	gboolean value = g_key_file_get_boolean(file, GROUP, key, &error);
	return error ? fallback : value;
}


void load_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	settings.style = (Style)read_key_integer(file, GROUP, "style", STYLE_SYSTEM, STYLE_SYSTEM, STYLE_DARK);
	g_free(settings.light_theme);
	settings.light_theme = g_key_file_get_string(file, GROUP, "light-theme", NULL);
	g_free(settings.dark_theme);
	settings.dark_theme = g_key_file_get_string(file, GROUP, "dark-theme", NULL);
	settings.shows_events = read_switch(file, "show-events", settings.shows_events);
	settings.shows_images = read_switch(file, "show-images", settings.shows_images);
	settings.shows_typing = read_switch(file, "show-typing", settings.shows_typing);
	settings.sends_typing = read_switch(file, "send-typing", settings.sends_typing);
	settings.sends_read_markers = read_switch(file, "send-read-markers", settings.sends_read_markers);
	settings.notifies_mentions = read_switch(file, "notify-mentions", settings.notifies_mentions);
	settings.plays_sounds = read_switch(file, "play-sounds", settings.plays_sounds);
	settings.notifies_private = read_switch(file, "notify-private", settings.notifies_private);
	settings.closes_to_tray = read_switch(file, "close-to-tray", settings.closes_to_tray);
	settings.uses_24_hours = read_switch(file, "24-hour-clock", settings.uses_24_hours);
	g_free(settings.highlight_words);
	settings.highlight_words = g_key_file_get_string(file, GROUP, "highlight-words", NULL);
}


void save_settings(void) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_integer(file, GROUP, "style", settings.style);
	g_key_file_set_string(file, GROUP, "light-theme", settings.light_theme ? settings.light_theme
		: DEFAULT_LIGHT_THEME);
	g_key_file_set_string(file, GROUP, "dark-theme", settings.dark_theme ? settings.dark_theme : DEFAULT_DARK_THEME);
	g_key_file_set_boolean(file, GROUP, "show-events", settings.shows_events);
	g_key_file_set_boolean(file, GROUP, "show-images", settings.shows_images);
	g_key_file_set_boolean(file, GROUP, "show-typing", settings.shows_typing);
	g_key_file_set_boolean(file, GROUP, "send-typing", settings.sends_typing);
	g_key_file_set_boolean(file, GROUP, "send-read-markers", settings.sends_read_markers);
	g_key_file_set_boolean(file, GROUP, "notify-mentions", settings.notifies_mentions);
	g_key_file_set_boolean(file, GROUP, "play-sounds", settings.plays_sounds);
	g_key_file_set_boolean(file, GROUP, "notify-private", settings.notifies_private);
	g_key_file_set_boolean(file, GROUP, "close-to-tray", settings.closes_to_tray);
	g_key_file_set_boolean(file, GROUP, "24-hour-clock", settings.uses_24_hours);
	g_key_file_set_string(file, GROUP, "highlight-words", settings.highlight_words ? settings.highlight_words : "");
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	save_key_file(file, path);
}


void apply_style(void) {
	static const AdwColorScheme SCHEMES[] = {
		[STYLE_SYSTEM] = ADW_COLOR_SCHEME_DEFAULT,
		[STYLE_LIGHT] = ADW_COLOR_SCHEME_FORCE_LIGHT,
		[STYLE_DARK] = ADW_COLOR_SCHEME_FORCE_DARK,
	};
	adw_style_manager_set_color_scheme(adw_style_manager_get_default(), SCHEMES[settings.style]);
	use_themes(settings.light_theme, settings.dark_theme, THEME_WINDOW | THEME_TITLE_BAR);
}


Settings *copy_settings(const Settings *source) {
	Settings *copy = g_new(Settings, 1);
	*copy = *source;
	copy->light_theme = g_strdup(source->light_theme);
	copy->dark_theme = g_strdup(source->dark_theme);
	copy->highlight_words = g_strdup(source->highlight_words);
	return copy;
}


void free_settings(Settings *copy) {
	g_free(copy->light_theme);
	g_free(copy->dark_theme);
	g_free(copy->highlight_words);
	g_free(copy);
}


void use_settings(const Settings *chosen) {
	Settings *copy = copy_settings(chosen);
	g_free(settings.light_theme);
	g_free(settings.dark_theme);
	g_free(settings.highlight_words);
	settings = *copy;
	g_free(copy);
	save_settings();
	apply_style();
}
