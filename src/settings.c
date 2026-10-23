#include <adwaita.h>

#include "common/key-store.h"
#include "common/themes.h"
#include "settings.h"

#define SETTINGS_FILE "settings.ini"
#define GROUP "chat"
#define NOTIFICATION_OVERRIDES_GROUP "notification-overrides"
#define BADGE_OVERRIDES_GROUP "badge-overrides"
#define OVERRIDE_ON "on"
#define OVERRIDE_OFF "off"

static Settings settings = {
	.style = STYLE_SYSTEM,
	.shows_images = TRUE,
	.shows_typing = TRUE,
	.sends_typing = TRUE,
	.sends_read_markers = TRUE,
	.notifies_mentions = TRUE,
	.plays_sounds = TRUE,
	.notifies_private = TRUE,
	.notifies_room_messages = FALSE,
	.shows_mention_badges = TRUE,
	.shows_private_badges = TRUE,
	.shows_room_badges = TRUE,
	.tray_badge = TRAY_BADGE_MENTIONS,
	.closes_to_tray = TRUE,
	.uses_24_hours = TRUE,
};


Settings *get_settings(void) {
	return &settings;
}


static void free_override(RoomOverride *override) {
	g_free(override->network_key);
	g_free(override->room_name);
	g_free(override);
}


static GPtrArray *create_overrides(void) {
	return g_ptr_array_new_with_free_func((GDestroyNotify)free_override);
}


static void add_override(GPtrArray *overrides, const char *network_key, const char *room_name, gboolean is_enabled,
	gint64 until) {
	RoomOverride *override = g_new(RoomOverride, 1);
	override->network_key = g_strdup(network_key);
	override->room_name = g_strdup(room_name);
	override->is_enabled = is_enabled;
	override->until = until;
	g_ptr_array_add(overrides, override);
}


static gboolean has_override_ended(const RoomOverride *override) {
	return override->until && override->until <= g_get_real_time();
}


static gboolean is_override_for(const RoomOverride *override, const char *network_key, const char *room_name) {
	return g_str_equal(override->network_key, network_key) && g_ascii_strcasecmp(override->room_name, room_name) == 0;
}


const RoomOverride *find_room_override(GPtrArray *overrides, const char *network_key, const char *room_name) {
	for (guint index = 0; index < overrides->len; index++) {
		const RoomOverride *override = g_ptr_array_index(overrides, index);
		if (is_override_for(override, network_key, room_name))
			return has_override_ended(override) ? NULL : override;
	}
	return NULL;
}


/* Takes away the room's override, along with any that have ended */
static void remove_override(GPtrArray *overrides, const char *network_key, const char *room_name) {
	for (guint index = overrides->len; index > 0; index--) {
		const RoomOverride *override = g_ptr_array_index(overrides, index - 1);
		gboolean is_room = network_key && is_override_for(override, network_key, room_name);
		if (is_room || has_override_ended(override))
			g_ptr_array_remove_index(overrides, index - 1);
	}
}


/* Each override is a line of its network, room, on or off and the moment it ends */
static GPtrArray *load_overrides(GKeyFile *file, const char *group) {
	GPtrArray *overrides = create_overrides();
	g_auto(GStrv) keys = g_key_file_get_keys(file, group, NULL, NULL);
	for (guint index = 0; keys && keys[index]; index++) {
		gsize count = 0;
		g_auto(GStrv) parts = g_key_file_get_string_list(file, group, keys[index], &count, NULL);
		if (count != 4)
			continue;
		gint64 until = g_ascii_strtoll(parts[3], NULL, 10);
		add_override(overrides, parts[0], parts[1], g_str_equal(parts[2], OVERRIDE_ON), until);
	}
	remove_override(overrides, NULL, NULL);
	return overrides;
}


static void save_overrides(GKeyFile *file, const char *group, GPtrArray *overrides) {
	for (guint index = 0; index < overrides->len; index++) {
		const RoomOverride *override = g_ptr_array_index(overrides, index);
		g_autofree char *key = g_strdup_printf("%u", index + 1);
		g_autofree char *until = g_strdup_printf("%" G_GINT64_FORMAT, override->until);
		const char *parts[] = {override->network_key, override->room_name,
			override->is_enabled ? OVERRIDE_ON : OVERRIDE_OFF, until};
		g_key_file_set_string_list(file, group, key, parts, G_N_ELEMENTS(parts));
	}
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
	settings.notifies_room_messages = read_switch(file, "notify-room-messages", settings.notifies_room_messages);
	settings.shows_mention_badges = read_switch(file, "badge-mentions", settings.shows_mention_badges);
	settings.shows_private_badges = read_switch(file, "badge-private", settings.shows_private_badges);
	settings.shows_room_badges = read_switch(file, "badge-room-messages", settings.shows_room_badges);
	settings.tray_badge = (TrayBadge)read_key_integer(file, GROUP, "tray-badge", TRAY_BADGE_MENTIONS, TRAY_BADGE_NOTHING,
		TRAY_BADGE_ANY);
	settings.closes_to_tray = read_switch(file, "close-to-tray", settings.closes_to_tray);
	settings.uses_24_hours = read_switch(file, "24-hour-clock", settings.uses_24_hours);
	g_free(settings.highlight_words);
	settings.highlight_words = g_key_file_get_string(file, GROUP, "highlight-words", NULL);
	g_clear_pointer(&settings.notification_overrides, g_ptr_array_unref);
	settings.notification_overrides = load_overrides(file, NOTIFICATION_OVERRIDES_GROUP);
	g_clear_pointer(&settings.badge_overrides, g_ptr_array_unref);
	settings.badge_overrides = load_overrides(file, BADGE_OVERRIDES_GROUP);
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
	g_key_file_set_boolean(file, GROUP, "notify-room-messages", settings.notifies_room_messages);
	g_key_file_set_boolean(file, GROUP, "badge-mentions", settings.shows_mention_badges);
	g_key_file_set_boolean(file, GROUP, "badge-private", settings.shows_private_badges);
	g_key_file_set_boolean(file, GROUP, "badge-room-messages", settings.shows_room_badges);
	g_key_file_set_integer(file, GROUP, "tray-badge", settings.tray_badge);
	g_key_file_set_boolean(file, GROUP, "close-to-tray", settings.closes_to_tray);
	g_key_file_set_boolean(file, GROUP, "24-hour-clock", settings.uses_24_hours);
	g_key_file_set_string(file, GROUP, "highlight-words", settings.highlight_words ? settings.highlight_words : "");
	save_overrides(file, NOTIFICATION_OVERRIDES_GROUP, settings.notification_overrides);
	save_overrides(file, BADGE_OVERRIDES_GROUP, settings.badge_overrides);
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


static GPtrArray *copy_overrides(GPtrArray *source) {
	GPtrArray *copy = create_overrides();
	for (guint index = 0; index < source->len; index++) {
		const RoomOverride *override = g_ptr_array_index(source, index);
		if (!has_override_ended(override))
			add_override(copy, override->network_key, override->room_name, override->is_enabled, override->until);
	}
	return copy;
}


Settings *copy_settings(const Settings *source) {
	Settings *copy = g_new(Settings, 1);
	*copy = *source;
	copy->light_theme = g_strdup(source->light_theme);
	copy->dark_theme = g_strdup(source->dark_theme);
	copy->highlight_words = g_strdup(source->highlight_words);
	copy->notification_overrides = copy_overrides(source->notification_overrides);
	copy->badge_overrides = copy_overrides(source->badge_overrides);
	return copy;
}


void free_settings(Settings *copy) {
	g_free(copy->light_theme);
	g_free(copy->dark_theme);
	g_free(copy->highlight_words);
	g_ptr_array_unref(copy->notification_overrides);
	g_ptr_array_unref(copy->badge_overrides);
	g_free(copy);
}


void use_settings(const Settings *chosen) {
	Settings *copy = copy_settings(chosen);
	g_free(settings.light_theme);
	g_free(settings.dark_theme);
	g_free(settings.highlight_words);
	g_ptr_array_unref(settings.notification_overrides);
	g_ptr_array_unref(settings.badge_overrides);
	settings = *copy;
	g_free(copy);
	save_settings();
	apply_style();
}


void set_room_override(GPtrArray *overrides, const char *network_key, const char *room_name, gboolean is_enabled,
	gint64 until) {
	remove_override(overrides, network_key, room_name);
	add_override(overrides, network_key, room_name, is_enabled, until);
	save_settings();
}


void clear_room_override(GPtrArray *overrides, const char *network_key, const char *room_name) {
	remove_override(overrides, network_key, room_name);
	save_settings();
}
