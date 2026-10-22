#pragma once

#include <glib.h>

/* Pango markup for a message, with bold, italics, underline, strikethrough, monospace, colours and links */
char *format_irc_markup(const char *text);
/* The text without formatting codes, for previews and notifications */
char *strip_irc_formatting(const char *text);
/* Addresses of pictures the text links to, which show as previews under the message */
GPtrArray *find_image_links(const char *text);

/* One of the eight name colours, the same for a nick every time */
int pick_name_color(const char *name);

char *format_clock_time(gint64 time);
/* A time for the room list, as the clock time today, the weekday this week and the date before that */
char *format_list_time(gint64 time);
/* A heading for a day in a conversation, such as Today or Monday 19 October */
char *format_day_heading(gint64 time);
gboolean is_same_day(gint64 first, gint64 second);
