#pragma once

#include <glib.h>

/* Pango markup for a message, with bold, italics, underline, strikethrough, monospace, colours and links */
char *format_irc_markup(const char *text);
/* Markdown, **bold** or __bold__, *italics* or _italics_, ~~struck~~, `code`, ```blocks``` and ||spoilers||, as the
 * IRC codes every client shows, with emphasis only at the edge of a word, so snake_case and addresses stay */
char *convert_markdown(const char *text);
/* Where a server strips reply tags, a reply starts with >"Name: the start of their message" and its own text, which
 * shows as the same quote a tagged reply gets */
typedef struct {
	char *sender, *excerpt;
	const char *rest;
} TextQuote;

gboolean read_text_quote(const char *text, TextQuote *quote);
void clear_text_quote(TextQuote *quote);
/* The text without formatting codes, for previews and notifications */
char *strip_irc_formatting(const char *text);
/* Addresses of pictures the text links to, which show as previews under the message */
GPtrArray *find_image_links(const char *text);
/* Addresses of web pages the text links to over HTTPS, which show a card with the page's title */
GPtrArray *find_page_links(const char *text);

/* A file someone shared, from a line such as "file: notes.txt (2.1 kB) https://example.com/notes.txt" */
typedef struct {
	char *name, *size, *url;
} SharedFile;

/* Reads a shared file's line into the file, returning FALSE for any other text */
gboolean read_shared_file(const char *text, SharedFile *file);
void clear_shared_file(SharedFile *file);

/* One of the eight name colours, the same for a nick every time */
int pick_name_color(const char *name);

char *format_clock_time(gint64 time);
/* A time for the room list, as the clock time today, the weekday this week and the date before that */
char *format_list_time(gint64 time);
/* A heading for a day in a conversation, such as Today or Monday 19 October */
char *format_day_heading(gint64 time);
gboolean is_same_day(gint64 first, gint64 second);

typedef void (*DayChangedFunction)(gpointer data);
/* Runs the function each time the local date changes, so words such as Today and Yesterday move on */
void watch_day_change(DayChangedFunction on_changed, gpointer data);
