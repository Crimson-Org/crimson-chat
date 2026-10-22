#pragma once

#include <glib.h>

typedef enum {
	STYLE_SYSTEM,
	STYLE_LIGHT,
	STYLE_DARK,
} Style;

/* The choices in Settings, kept in the user's config folder */
typedef struct {
	Style style;
	gboolean shows_events, shows_images, shows_typing, sends_typing, sends_read_markers;
	gboolean notifies_mentions, notifies_private, closes_to_tray, uses_24_hours;
	/* Comma separated words that count as a mention besides the user's own nick */
	char *highlight_words;
} Settings;

Settings *get_settings(void);
void load_settings(void);
void save_settings(void);
void apply_style(void);
