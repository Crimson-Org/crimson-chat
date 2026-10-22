#pragma once

#include <glib.h>

/* One protocol line split into its tags, source, command and parameters */
typedef struct {
	GHashTable *tags;
	char *source;
	char *command;
	GPtrArray *parameters;
} IrcMessage;

/* Where a message came from, split out of nick!user@host */
typedef struct {
	char *nick;
	char *user;
	char *host;
} IrcSource;

IrcMessage *create_irc_message(const char *command, ...) G_GNUC_NULL_TERMINATED;
IrcMessage *copy_irc_message(const IrcMessage *message);
void free_irc_message(IrcMessage *message);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(IrcMessage, free_irc_message)

/* Returns NULL for an empty or malformed line */
IrcMessage *parse_irc_line(const char *line);
/* The line without its ending CR LF */
char *format_irc_line(const IrcMessage *message);

void add_irc_parameter(IrcMessage *message, const char *parameter);
/* NULL when the message has fewer parameters */
const char *get_irc_parameter(const IrcMessage *message, guint index);
guint count_irc_parameters(const IrcMessage *message);

/* A tag without a value reads as an empty string, and a missing tag as NULL */
const char *get_irc_tag(const IrcMessage *message, const char *name);
void set_irc_tag(IrcMessage *message, const char *name, const char *value);
void remove_irc_tag(IrcMessage *message, const char *name);
gboolean has_irc_tag(const IrcMessage *message, const char *name);

void parse_irc_source(const char *source, IrcSource *parts);
void clear_irc_source(IrcSource *parts);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(IrcSource, clear_irc_source)
/* The nick part of a source, or the whole source for a server */
char *get_source_nick(const char *source);

/* Times in the YYYY-MM-DDThh:mm:ss.sssZ form used by the time tag */
char *format_irc_time(GDateTime *time);
GDateTime *parse_irc_time(const char *text);
char *format_irc_time_now(void);

/* Attribute lists such as network attributes use the tag syntax without the leading @ */
GHashTable *parse_irc_attributes(const char *text);
char *format_irc_attributes(GHashTable *attributes);

/* A random identifier for message ids, labels and batch references */
char *create_irc_identifier(void);
