#pragma once

#include <glib.h>

typedef enum {
	STYLE_SYSTEM,
	STYLE_LIGHT,
	STYLE_DARK,
} Style;

/* What puts a dot on the tray icon, counting only rooms whose badges show */
typedef enum {
	TRAY_BADGE_NOTHING,
	TRAY_BADGE_MENTIONS,
	TRAY_BADGE_ANY,
} TrayBadge;

/* A room or private chat whose notifications or badges are switched on or off apart from the defaults */
typedef struct {
	char *network_key, *room_name;
	gboolean is_enabled;
	/* The real time in microseconds when the override ends, or 0 for one that never does */
	gint64 until;
} RoomOverride;

/* The choices in Settings, kept in the user's config folder */
typedef struct {
	Style style;
	/* Theme ids for the light and dark style */
	char *light_theme, *dark_theme;
	gboolean shows_events, shows_images, shows_typing, sends_typing, sends_read_markers;
	gboolean notifies_mentions, notifies_private, notifies_room_messages, plays_sounds, closes_to_tray, uses_24_hours;
	gboolean shows_mention_badges, shows_private_badges, shows_room_badges;
	TrayBadge tray_badge;
	/* Comma separated words that count as a mention besides the user's own nick */
	char *highlight_words;
	GPtrArray *notification_overrides, *badge_overrides;
} Settings;

Settings *get_settings(void);
void load_settings(void);
void save_settings(void);
void apply_style(void);
/* A copy for a settings form to change, which takes effect only once handed to use_settings */
Settings *copy_settings(const Settings *source);
void free_settings(Settings *copy);
void use_settings(const Settings *chosen);
/* The override still in force for a room, or NULL once it has ended or there is none */
const RoomOverride *find_room_override(GPtrArray *overrides, const char *network_key, const char *room_name);
/* Overrides take effect and are saved at once, outside the Settings form */
void set_room_override(GPtrArray *overrides, const char *network_key, const char *room_name, gboolean is_enabled,
	gint64 until);
void clear_room_override(GPtrArray *overrides, const char *network_key, const char *room_name);
