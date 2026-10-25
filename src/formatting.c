#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "settings.h"

#define BOLD '\x02'
#define COLOR '\x03'
#define HEX_COLOR '\x04'
#define MONOSPACE '\x11'
#define REVERSE '\x16'
#define ITALIC '\x1d'
#define STRIKETHROUGH '\x1e'
#define UNDERLINE '\x1f'
#define RESET '\x0f'
/* A spoiler is black text on black, the way IRC has always hidden text until it is selected */
#define SPOILER_OPENING "\x03" "01,01"
#define NAME_COLORS 8
#define WEEK_DAYS 6
/* The timer to midnight stops while the computer sleeps, so it also checks the date this often to catch a missed
 * midnight soon after waking */
#define DAY_CHECK_SECONDS (10 * 60)
#define LINK_PATTERN "(?:https?|ftp)://[^\\s<>\"]+|www\\.[^\\s<>\"]+"
#define TRAILING_PUNCTUATION ".,;:!?'\""

/* The sixteen classic colours followed by the 83 extended ones, as servers and clients agree on them */
static const char *const PALETTE[] = {
	"#ffffff", "#000000", "#00007f", "#009300", "#ff0000", "#7f0000", "#9c009c", "#fc7f00", "#ffff00", "#00fc00",
	"#009393", "#00ffff", "#0000fc", "#ff00ff", "#7f7f7f", "#d2d2d2", "#470000", "#472100", "#474700", "#324700",
	"#004700", "#00472c", "#004747", "#002747", "#000047", "#2e0047", "#470047", "#47002a", "#740000", "#743a00",
	"#747400", "#517400", "#007400", "#007449", "#007474", "#004074", "#000074", "#4b0074", "#740074", "#740045",
	"#b50000", "#b56300", "#b5b500", "#7db500", "#00b500", "#00b571", "#00b5b5", "#0063b5", "#0000b5", "#7500b5",
	"#b500b5", "#b5006b", "#ff0000", "#ff8c00", "#ffff00", "#b2ff00", "#00ff00", "#00ffa0", "#00ffff", "#008cff",
	"#0000ff", "#a500ff", "#ff00ff", "#ff0098", "#ff5959", "#ffb459", "#ffff71", "#cfff60", "#6fff6f", "#65ffc9",
	"#6dffff", "#59b4ff", "#5959ff", "#c459ff", "#ff66ff", "#ff59bc", "#ff9c9c", "#ffd39c", "#ffff9c", "#e2ff9c",
	"#9cff9c", "#9cffdb", "#9cffff", "#9cd3ff", "#9c9cff", "#dc9cff", "#ff9cff", "#ff94d3", "#000000", "#131313",
	"#282828", "#363636", "#4d4d4d", "#656565", "#818181", "#9f9f9f", "#bcbcbc", "#e2e2e2", "#ffffff",
};

/* The look of a stretch of text between formatting codes */
typedef struct {
	gboolean is_bold, is_italic, is_underlined, is_struck, is_monospace;
	char foreground[8], background[8];
} Look;


static GRegex *get_link_pattern(void) {
	static GRegex *pattern = NULL;
	if (!pattern)
		pattern = g_regex_new(LINK_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
	return pattern;
}


/* Leaves off a full stop or bracket that ends a sentence rather than the address */
static gsize trim_link(const char *link, gsize length) {
	while (length > 0 && strchr(TRAILING_PUNCTUATION, link[length - 1]))
		length--;
	if (length > 0 && link[length - 1] == ')') {
		int depth = 0;
		for (gsize index = 0; index < length; index++)
			depth += link[index] == '(' ? 1 : link[index] == ')' ? -1 : 0;
		if (depth < 0)
			length--;
	}
	return length;
}


static void append_escaped(GString *markup, const char *text, gssize length) {
	g_autofree char *escaped = g_markup_escape_text(text, length);
	g_string_append(markup, escaped);
}


static void append_linked(GString *markup, const char *text) {
	GMatchInfo *match = NULL;
	const char *cursor = text;
	g_regex_match(get_link_pattern(), text, 0, &match);
	while (g_match_info_matches(match)) {
		int start = 0, end = 0;
		g_match_info_fetch_pos(match, 0, &start, &end);
		gsize length = trim_link(text + start, (gsize)(end - start));
		append_escaped(markup, cursor, text + start - cursor);
		g_autofree char *link = g_strndup(text + start, length);
		g_autofree char *target = g_str_has_prefix(link, "www.") ? g_strconcat("https://", link, NULL)
			: g_strdup(link);
		g_autofree char *escaped_target = g_markup_escape_text(target, -1);
		g_string_append_printf(markup, "<a href=\"%s\">", escaped_target);
		append_escaped(markup, link, -1);
		g_string_append(markup, "</a>");
		cursor = text + start + length;
		g_match_info_next(match, NULL);
	}
	g_match_info_free(match);
	append_escaped(markup, cursor, -1);
}


static gboolean is_plain_look(const Look *look) {
	return !look->is_bold && !look->is_italic && !look->is_underlined && !look->is_struck && !look->is_monospace
		&& !*look->foreground && !*look->background;
}


static void append_run(GString *markup, const char *text, const Look *look) {
	if (!*text)
		return;
	if (is_plain_look(look)) {
		append_linked(markup, text);
		return;
	}
	g_string_append(markup, "<span");
	if (look->is_bold)
		g_string_append(markup, " weight=\"bold\"");
	if (look->is_italic)
		g_string_append(markup, " style=\"italic\"");
	if (look->is_underlined)
		g_string_append(markup, " underline=\"single\"");
	if (look->is_struck)
		g_string_append(markup, " strikethrough=\"true\"");
	if (look->is_monospace)
		g_string_append(markup, " font_family=\"monospace\"");
	if (*look->foreground)
		g_string_append_printf(markup, " foreground=\"%s\"", look->foreground);
	if (*look->background)
		g_string_append_printf(markup, " background=\"%s\"", look->background);
	g_string_append_c(markup, '>');
	append_linked(markup, text);
	g_string_append(markup, "</span>");
}


/* Reads up to two digits of a palette colour, returning how many characters it used */
static int read_palette_color(const char *text, char *color) {
	int digits = 0, value = 0;
	while (digits < 2 && g_ascii_isdigit(text[digits]))
		value = value * 10 + (text[digits++] - '0');
	if (!digits)
		return 0;
	if (value < (int)G_N_ELEMENTS(PALETTE))
		g_strlcpy(color, PALETTE[value], 8);
	else
		*color = '\0';
	return digits;
}


static int read_hex_color(const char *text, char *color) {
	for (int index = 0; index < 6; index++) {
		if (!g_ascii_isxdigit(text[index]))
			return 0;
	}
	g_snprintf(color, 8, "#%.6s", text);
	return 6;
}


/* Moves past a colour code and its numbers, returning where the text continues */
static const char *read_color_code(const char *code, Look *look) {
	gboolean is_hex = *code == HEX_COLOR;
	int (*read)(const char *, char *) = is_hex ? read_hex_color : read_palette_color;
	const char *cursor = code + 1;
	int used = read(cursor, look->foreground);
	if (!used) {
		*look->foreground = '\0';
		*look->background = '\0';
		return cursor;
	}
	cursor += used;
	if (*cursor == ',') {
		int background = read(cursor + 1, look->background);
		if (background)
			cursor += 1 + background;
	}
	return cursor;
}


/* Applies one formatting code, returning where the text after it begins */
static const char *apply_code(const char *code, Look *look) {
	switch (*code) {
	case BOLD: look->is_bold = !look->is_bold; break;
	case ITALIC: look->is_italic = !look->is_italic; break;
	case UNDERLINE: look->is_underlined = !look->is_underlined; break;
	case STRIKETHROUGH: look->is_struck = !look->is_struck; break;
	case MONOSPACE: look->is_monospace = !look->is_monospace; break;
	case REVERSE: break;
	case RESET: *look = (Look){0}; break;
	case COLOR:
	case HEX_COLOR: return read_color_code(code, look);
	default: return NULL;
	}
	return code + 1;
}


char *format_irc_markup(const char *text) {
	GString *markup = g_string_new(NULL);
	GString *run = g_string_new(NULL);
	Look look = {0};
	for (const char *cursor = text; *cursor;) {
		Look next = look;
		const char *after = apply_code(cursor, &next);
		if (!after) {
			g_string_append_c(run, *cursor++);
			continue;
		}
		append_run(markup, run->str, &look);
		g_string_truncate(run, 0);
		look = next;
		cursor = after;
	}
	append_run(markup, run->str, &look);
	g_string_free(run, TRUE);
	return g_string_free(markup, FALSE);
}


char *strip_irc_formatting(const char *text) {
	GString *plain = g_string_new(NULL);
	for (const char *cursor = text ? text : ""; *cursor;) {
		Look ignored = {0};
		const char *after = apply_code(cursor, &ignored);
		if (after) {
			cursor = after;
			continue;
		}
		g_string_append_c(plain, *cursor == '\n' ? ' ' : *cursor);
		cursor++;
	}
	return g_string_free(plain, FALSE);
}


static gboolean is_image_link(const char *link) {
	static const char *const EXTENSIONS[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", NULL};
	g_autofree char *lower = g_ascii_strdown(link, -1);
	char *query = strpbrk(lower, "?#");
	if (query)
		*query = '\0';
	for (int index = 0; EXTENSIONS[index]; index++) {
		if (g_str_has_suffix(lower, EXTENSIONS[index]))
			return TRUE;
	}
	return FALSE;
}


static GPtrArray *find_links(const char *text, gboolean wants_images) {
	GPtrArray *links = g_ptr_array_new_with_free_func(g_free);
	g_autofree char *plain = strip_irc_formatting(text);
	GMatchInfo *match = NULL;
	g_regex_match(get_link_pattern(), plain, 0, &match);
	while (g_match_info_matches(match)) {
		int start = 0, end = 0;
		g_match_info_fetch_pos(match, 0, &start, &end);
		g_autofree char *link = g_strndup(plain + start, trim_link(plain + start, (gsize)(end - start)));
		if (g_str_has_prefix(link, "https://") && is_image_link(link) == wants_images)
			g_ptr_array_add(links, g_steal_pointer(&link));
		g_match_info_next(match, NULL);
	}
	g_match_info_free(match);
	return links;
}


GPtrArray *find_image_links(const char *text) {
	return find_links(text, TRUE);
}


GPtrArray *find_page_links(const char *text) {
	return find_links(text, FALSE);
}


gboolean read_shared_file(const char *text, SharedFile *file) {
	static GRegex *pattern;
	if (!pattern)
		pattern = g_regex_new("^file: (.+) \\((\\d+(?:[.,]\\d+)?\\h[^\\s()]+)\\) (https://\\S+)$", G_REGEX_OPTIMIZE, 0,
			NULL);
	g_autofree char *plain = strip_irc_formatting(text);
	g_autoptr(GMatchInfo) match = NULL;
	if (!g_regex_match(pattern, g_strstrip(plain), 0, &match))
		return FALSE;
	file->name = g_match_info_fetch(match, 1);
	file->size = g_match_info_fetch(match, 2);
	file->url = g_match_info_fetch(match, 3);
	return TRUE;
}


void clear_shared_file(SharedFile *file) {
	g_free(file->name);
	g_free(file->size);
	g_free(file->url);
}


int pick_name_color(const char *name) {
	g_autofree char *folded = g_utf8_casefold(name ? name : "", -1);
	return (int)(g_str_hash(folded) % NAME_COLORS);
}


static GDateTime *to_local_time(gint64 time) {
	g_autoptr(GDateTime) universal = g_date_time_new_from_unix_utc_usec(time);
	return g_date_time_to_local(universal);
}


char *format_clock_time(gint64 time) {
	g_autoptr(GDateTime) local = to_local_time(time);
	return g_date_time_format(local, get_settings()->uses_24_hours ? "%H:%M" : "%l:%M %p");
}


gboolean is_same_day(gint64 first, gint64 second) {
	g_autoptr(GDateTime) one = to_local_time(first);
	g_autoptr(GDateTime) other = to_local_time(second);
	return g_date_time_get_year(one) == g_date_time_get_year(other)
		&& g_date_time_get_day_of_year(one) == g_date_time_get_day_of_year(other);
}


static int count_days_ago(gint64 time) {
	g_autoptr(GDateTime) local = to_local_time(time);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autoptr(GDateTime) day = g_date_time_new_local(g_date_time_get_year(local), g_date_time_get_month(local),
		g_date_time_get_day_of_month(local), 0, 0, 0);
	g_autoptr(GDateTime) today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
		g_date_time_get_day_of_month(now), 0, 0, 0);
	return (int)(g_date_time_difference(today, day) / G_TIME_SPAN_DAY);
}


char *format_list_time(gint64 time) {
	if (!time)
		return g_strdup("");
	int days = count_days_ago(time);
	if (days <= 0)
		return format_clock_time(time);
	g_autoptr(GDateTime) local = to_local_time(time);
	return g_date_time_format(local, days <= WEEK_DAYS ? "%a" : "%Y-%m-%d");
}


char *format_day_heading(gint64 time) {
	int days = count_days_ago(time);
	if (days == 0)
		return g_strdup(TEXT_TODAY);
	if (days == 1)
		return g_strdup(TEXT_YESTERDAY);
	g_autoptr(GDateTime) local = to_local_time(time);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	gboolean is_this_year = g_date_time_get_year(local) == g_date_time_get_year(now);
	return g_date_time_format(local, is_this_year ? "%A %e %B" : "%A %e %B %Y");
}


/* A function to tell when the date changes */
typedef struct {
	DayChangedFunction on_changed;
	gpointer data;
} DayWatcher;

static GArray *day_watchers;
static int watched_day;


/* The local date as one number that changes exactly when the date does */
static int read_local_day(void) {
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	return g_date_time_get_year(now) * 1000 + g_date_time_get_day_of_year(now);
}


static guint count_seconds_to_midnight(void) {
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autoptr(GDateTime) start = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
		g_date_time_get_day_of_month(now), 0, 0, 0);
	g_autoptr(GDateTime) midnight = g_date_time_add_days(start, 1);
	return (guint)(g_date_time_difference(midnight, now) / G_USEC_PER_SEC) + 1;
}


static gboolean check_day(gpointer data);


static void schedule_day_check(void) {
	g_timeout_add_seconds(MIN(count_seconds_to_midnight(), DAY_CHECK_SECONDS), check_day, NULL);
}


static gboolean check_day(gpointer data) {
	int today = read_local_day();
	if (today != watched_day) {
		watched_day = today;
		for (guint index = 0; index < day_watchers->len; index++) {
			DayWatcher *watcher = &g_array_index(day_watchers, DayWatcher, index);
			watcher->on_changed(watcher->data);
		}
	}
	schedule_day_check();
	return G_SOURCE_REMOVE;
}


void watch_day_change(DayChangedFunction on_changed, gpointer data) {
	if (!day_watchers) {
		day_watchers = g_array_new(FALSE, FALSE, sizeof(DayWatcher));
		watched_day = read_local_day();
		schedule_day_check();
	}
	DayWatcher watcher = {on_changed, data};
	g_array_append_val(day_watchers, watcher);
}


static gboolean is_word_before(const char *text, const char *position) {
	if (position == text)
		return FALSE;
	const char *previous = g_utf8_find_prev_char(text, position);
	return previous && g_unichar_isalnum(g_utf8_get_char(previous));
}


static gboolean is_link_start(const char *position) {
	return g_str_has_prefix(position, "http://") || g_str_has_prefix(position, "https://")
		|| g_str_has_prefix(position, "ftp://") || g_str_has_prefix(position, "www.");
}


/* The closing marker ends a word: it follows a letter rather than a space and no letter follows it */
static const char *find_closing_marker(const char *inner, char marker, gsize width) {
	for (const char *position = inner + 1; *position; position++) {
		if (is_link_start(position))
			return NULL;
		if (*position != marker || (width == 2 && position[1] != marker))
			continue;
		if (width == 1 && position[1] == marker) {
			position++;
			continue;
		}
		const char *after = position + width;
		gboolean ends_word = !*after || !g_unichar_isalnum(g_utf8_get_char(after));
		if (position[-1] != ' ' && ends_word)
			return position;
	}
	return NULL;
}


/* Markers whose text goes out as written, with the codes that open and close it, longest marker first */
static const struct {
	const char *marker, *opening, *closing;
} LITERAL_SPANS[] = {
	{"```", "\x11", "\x11"},
	{"`", "\x11", "\x11"},
	{"||", SPOILER_OPENING, "\x03"},
};


/* Adds a code block, inline code or spoiler starting here, returning where the text after it begins or NULL */
static const char *convert_literal_span(GString *converted, const char *position) {
	for (guint index = 0; index < G_N_ELEMENTS(LITERAL_SPANS); index++) {
		const char *marker = LITERAL_SPANS[index].marker;
		if (!g_str_has_prefix(position, marker))
			continue;
		const char *inner = position + strlen(marker);
		const char *close = strstr(inner, marker);
		if (!close || close == inner)
			return NULL;
		g_string_append(converted, LITERAL_SPANS[index].opening);
		g_string_append_len(converted, inner, close - inner);
		g_string_append(converted, LITERAL_SPANS[index].closing);
		return close + strlen(marker);
	}
	return NULL;
}


char *convert_markdown(const char *text) {
	if (!text)
		return NULL;
	GString *converted = g_string_new(NULL);
	const char *position = text;
	while (*position) {
		/* Addresses keep their underscores and stars */
		if (is_link_start(position) && !is_word_before(text, position)) {
			const char *end = position;
			while (*end && *end != ' ')
				end++;
			g_string_append_len(converted, position, end - position);
			position = end;
			continue;
		}
		const char *after_span = convert_literal_span(converted, position);
		if (after_span) {
			position = after_span;
			continue;
		}
		char marker = *position;
		gboolean is_strike = marker == '~' && position[1] == '~';
		if ((marker == '*' || marker == '_' || is_strike) && !is_word_before(text, position)) {
			gsize width = position[1] == marker ? 2 : 1;
			const char *inner = position + width;
			const char *close = *inner && *inner != ' ' && *inner != marker
				? find_closing_marker(inner, marker, width) : NULL;
			if (close) {
				char code = is_strike ? STRIKETHROUGH : width == 2 ? BOLD : ITALIC;
				g_autofree char *middle = g_strndup(inner, (gsize)(close - inner));
				g_autofree char *nested = convert_markdown(middle);
				g_string_append_c(converted, code);
				g_string_append(converted, nested);
				g_string_append_c(converted, code);
				position = close + width;
				continue;
			}
		}
		g_string_append_c(converted, *position);
		position++;
	}
	return g_string_free(converted, FALSE);
}


gboolean read_text_quote(const char *text, TextQuote *quote) {
	if (!text || !g_str_has_prefix(text, ">\""))
		return FALSE;
	const char *start = text + 2;
	const char *colon = strstr(start, ": ");
	const char *close = colon ? strstr(colon + 2, "\" ") : NULL;
	if (!colon || !close || colon == start || memchr(start, ' ', (size_t)(colon - start)))
		return FALSE;
	quote->sender = g_strndup(start, (gsize)(colon - start));
	quote->excerpt = g_strndup(colon + 2, (gsize)(close - colon - 2));
	quote->rest = close + 2;
	return TRUE;
}


void clear_text_quote(TextQuote *quote) {
	g_free(quote->sender);
	g_free(quote->excerpt);
}
