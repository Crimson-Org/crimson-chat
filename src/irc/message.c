#include <stdarg.h>
#include <string.h>

#include "message.h"

#define IDENTIFIER_BYTES 12


static GHashTable *create_tag_table(void) {
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}


static IrcMessage *create_empty_message(void) {
	IrcMessage *message = g_new0(IrcMessage, 1);
	message->tags = create_tag_table();
	message->parameters = g_ptr_array_new_with_free_func(g_free);
	return message;
}


IrcMessage *create_irc_message(const char *command, ...) {
	IrcMessage *message = create_empty_message();
	message->command = g_ascii_strup(command, -1);
	va_list parameters;
	va_start(parameters, command);
	for (const char *parameter = va_arg(parameters, const char *); parameter;
		parameter = va_arg(parameters, const char *))
		add_irc_parameter(message, parameter);
	va_end(parameters);
	return message;
}


IrcMessage *copy_irc_message(const IrcMessage *message) {
	IrcMessage *copy = create_empty_message();
	GHashTableIter iterator;
	gpointer name, value;
	g_hash_table_iter_init(&iterator, message->tags);
	while (g_hash_table_iter_next(&iterator, &name, &value))
		g_hash_table_insert(copy->tags, g_strdup(name), g_strdup(value));
	copy->source = g_strdup(message->source);
	copy->command = g_strdup(message->command);
	for (guint index = 0; index < message->parameters->len; index++)
		add_irc_parameter(copy, g_ptr_array_index(message->parameters, index));
	return copy;
}


void free_irc_message(IrcMessage *message) {
	if (!message)
		return;
	g_hash_table_unref(message->tags);
	g_free(message->source);
	g_free(message->command);
	g_ptr_array_unref(message->parameters);
	g_free(message);
}


static char *unescape_tag_value(const char *value) {
	GString *text = g_string_new(NULL);
	for (const char *character = value; *character; character++) {
		if (*character != '\\') {
			g_string_append_c(text, *character);
			continue;
		}
		character++;
		if (!*character)
			break;
		switch (*character) {
		case ':': g_string_append_c(text, ';'); break;
		case 's': g_string_append_c(text, ' '); break;
		case 'r': g_string_append_c(text, '\r'); break;
		case 'n': g_string_append_c(text, '\n'); break;
		default: g_string_append_c(text, *character); break;
		}
	}
	return g_string_free(text, FALSE);
}


static void escape_tag_value(GString *text, const char *value) {
	for (const char *character = value; *character; character++) {
		switch (*character) {
		case ';': g_string_append(text, "\\:"); break;
		case ' ': g_string_append(text, "\\s"); break;
		case '\\': g_string_append(text, "\\\\"); break;
		case '\r': g_string_append(text, "\\r"); break;
		case '\n': g_string_append(text, "\\n"); break;
		default: g_string_append_c(text, *character); break;
		}
	}
}


static void read_tags_into(GHashTable *tags, const char *text) {
	g_auto(GStrv) items = g_strsplit(text, ";", -1);
	for (int index = 0; items[index]; index++) {
		if (!*items[index])
			continue;
		char *equals = strchr(items[index], '=');
		if (equals)
			*equals = '\0';
		g_hash_table_insert(tags, g_strdup(items[index]), equals ? unescape_tag_value(equals + 1) : g_strdup(""));
	}
}


static void write_tags_into(GString *text, GHashTable *tags) {
	GHashTableIter iterator;
	gpointer name, value;
	gboolean is_first = TRUE;
	g_hash_table_iter_init(&iterator, tags);
	while (g_hash_table_iter_next(&iterator, &name, &value)) {
		g_string_append_c(text, is_first ? '@' : ';');
		g_string_append(text, name);
		if (*(const char *)value) {
			g_string_append_c(text, '=');
			escape_tag_value(text, value);
		}
		is_first = FALSE;
	}
}


/* Moves past the next word and the spaces after it, ending the word in place */
static char *take_word(char **cursor) {
	char *word = *cursor;
	char *space = strchr(word, ' ');
	if (!space) {
		*cursor = word + strlen(word);
		return word;
	}
	*space = '\0';
	for (space++; *space == ' '; space++)
		;
	*cursor = space;
	return word;
}


IrcMessage *parse_irc_line(const char *line) {
	g_autofree char *copy = g_strdup(line);
	g_strchomp(copy);
	char *cursor = copy;
	while (*cursor == ' ')
		cursor++;
	if (!*cursor)
		return NULL;
	IrcMessage *message = create_empty_message();
	if (*cursor == '@')
		read_tags_into(message->tags, take_word(&cursor) + 1);
	if (*cursor == ':')
		message->source = g_strdup(take_word(&cursor) + 1);
	if (!*cursor) {
		free_irc_message(message);
		return NULL;
	}
	message->command = g_ascii_strup(take_word(&cursor), -1);
	while (*cursor) {
		if (*cursor == ':') {
			add_irc_parameter(message, cursor + 1);
			break;
		}
		add_irc_parameter(message, take_word(&cursor));
	}
	return message;
}


static gboolean needs_trailing_mark(const char *parameter) {
	return !*parameter || *parameter == ':' || strchr(parameter, ' ');
}


char *format_irc_line(const IrcMessage *message) {
	GString *text = g_string_new(NULL);
	if (g_hash_table_size(message->tags)) {
		write_tags_into(text, message->tags);
		g_string_append_c(text, ' ');
	}
	if (message->source)
		g_string_append_printf(text, ":%s ", message->source);
	g_string_append(text, message->command);
	for (guint index = 0; index < message->parameters->len; index++) {
		const char *parameter = g_ptr_array_index(message->parameters, index);
		gboolean is_last = index + 1 == message->parameters->len;
		g_string_append_c(text, ' ');
		if (is_last && needs_trailing_mark(parameter))
			g_string_append_c(text, ':');
		g_string_append(text, parameter);
	}
	/* A line break inside a parameter would end the line early, so it becomes a space */
	g_strdelimit(text->str, "\r\n", ' ');
	return g_string_free(text, FALSE);
}


void add_irc_parameter(IrcMessage *message, const char *parameter) {
	g_ptr_array_add(message->parameters, g_strdup(parameter ? parameter : ""));
}


const char *get_irc_parameter(const IrcMessage *message, guint index) {
	return index < message->parameters->len ? g_ptr_array_index(message->parameters, index) : NULL;
}


guint count_irc_parameters(const IrcMessage *message) {
	return message->parameters->len;
}


const char *get_irc_tag(const IrcMessage *message, const char *name) {
	return g_hash_table_lookup(message->tags, name);
}


void set_irc_tag(IrcMessage *message, const char *name, const char *value) {
	g_hash_table_insert(message->tags, g_strdup(name), g_strdup(value ? value : ""));
}


void remove_irc_tag(IrcMessage *message, const char *name) {
	g_hash_table_remove(message->tags, name);
}


gboolean has_irc_tag(const IrcMessage *message, const char *name) {
	return g_hash_table_contains(message->tags, name);
}


void parse_irc_source(const char *source, IrcSource *parts) {
	*parts = (IrcSource){0};
	if (!source)
		return;
	const char *at = strchr(source, '@');
	const char *bang = strchr(source, '!');
	if (bang && at && bang > at)
		bang = NULL;
	const char *nick_end = bang ? bang : at ? at : source + strlen(source);
	parts->nick = g_strndup(source, (gsize)(nick_end - source));
	if (bang)
		parts->user = g_strndup(bang + 1, (gsize)((at ? at : source + strlen(source)) - bang - 1));
	if (at)
		parts->host = g_strdup(at + 1);
}


void clear_irc_source(IrcSource *parts) {
	g_clear_pointer(&parts->nick, g_free);
	g_clear_pointer(&parts->user, g_free);
	g_clear_pointer(&parts->host, g_free);
}


char *get_source_nick(const char *source) {
	g_auto(IrcSource) parts;
	parse_irc_source(source, &parts);
	return g_steal_pointer(&parts.nick);
}


char *format_irc_time(GDateTime *time) {
	g_autoptr(GDateTime) universal = g_date_time_to_utc(time);
	g_autofree char *seconds = g_date_time_format(universal, "%Y-%m-%dT%H:%M:%S");
	return g_strdup_printf("%s.%03dZ", seconds, g_date_time_get_microsecond(universal) / 1000);
}


GDateTime *parse_irc_time(const char *text) {
	if (!text || !*text)
		return NULL;
	return g_date_time_new_from_iso8601(text, NULL);
}


char *format_irc_time_now(void) {
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	return format_irc_time(now);
}


GHashTable *parse_irc_attributes(const char *text) {
	GHashTable *attributes = create_tag_table();
	if (text)
		read_tags_into(attributes, text);
	return attributes;
}


char *format_irc_attributes(GHashTable *attributes) {
	GString *text = g_string_new(NULL);
	write_tags_into(text, attributes);
	if (text->len)
		g_string_erase(text, 0, 1);
	return g_string_free(text, FALSE);
}


char *create_irc_identifier(void) {
	static const char LETTERS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	char identifier[IDENTIFIER_BYTES + 1];
	for (int index = 0; index < IDENTIFIER_BYTES; index++)
		identifier[index] = LETTERS[g_random_int_range(0, (gint32)sizeof LETTERS - 1)];
	identifier[IDENTIFIER_BYTES] = '\0';
	return g_strdup(identifier);
}


const char *get_last_irc_parameter(const IrcMessage *message) {
	guint count = count_irc_parameters(message);
	return count ? get_irc_parameter(message, count - 1) : "";
}
