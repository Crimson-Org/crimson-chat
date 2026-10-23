#pragma once

#include "casemap.h"
#include "message.h"

/* The keys of the IRCv3 metadata registry followed for every person and room, which the bouncer subscribes to
 * before it joins anything so the server sends them with each join */
#define FOLLOWED_PROFILE_KEYS "avatar", "display-name", "color", "status", "pronouns", "homepage", "bot"

/* Someone seen in a shared channel, as far as the server has told */
typedef struct {
	char *nick, *user, *host, *account, *realname;
	gboolean is_away, is_bot;
	/* Keys such as avatar and display-name from the metadata extension */
	GHashTable *metadata;
} IrcUser;

/* A channel the connection has joined */
typedef struct {
	char *name;
	char *topic, *topic_setter;
	gint64 topic_time;
	/* Folded nicks to their status symbols, highest first, such as "@+" */
	GHashTable *members;
	GHashTable *metadata;
	/* A new NAMES listing replaces the members once the previous one has ended */
	gboolean has_complete_names;
} IrcChannel;

/* What a connection knows about its network, kept current from each message the server sends */
typedef struct {
	char *nick, *user, *host, *account;
	gboolean is_registered, is_away;
	IrcCasemap casemap;
	/* RPL_ISUPPORT tokens, with an empty string for a token without a value */
	GHashTable *support;
	char *prefix_modes, *prefix_symbols, *channel_types;
	/* The four CHANMODES groups, which decide whether a mode letter takes a parameter */
	char *list_modes, *setting_modes, *parameter_modes;
	GHashTable *available_caps;
	GHashTable *enabled_caps;
	/* Folded channel names to IrcChannel and folded nicks to IrcUser */
	GHashTable *channels;
	GHashTable *users;
} IrcState;

IrcState *create_irc_state(void);
void free_irc_state(IrcState *state);
/* Forgets everything a connection learned, for a fresh connection */
void reset_irc_state(IrcState *state);

/* Fills affected with the names of the channels a QUIT or NICK touched, before the change removes that link */
void update_irc_state(IrcState *state, const IrcMessage *message, GPtrArray *affected);
/* Reads one RPL_ISUPPORT token, which the bouncer also uses to describe itself */
void apply_irc_support(IrcState *state, const char *token);

const char *get_irc_support(IrcState *state, const char *name);
gboolean has_irc_cap(IrcState *state, const char *name);
char *fold_state_name(IrcState *state, const char *name);
gboolean is_irc_channel_name(IrcState *state, const char *name);
gboolean is_own_nick(IrcState *state, const char *nick);
IrcChannel *find_irc_channel(IrcState *state, const char *name);
IrcUser *find_irc_user(IrcState *state, const char *nick);
/* How senior a status symbol is, from zero for the highest, or -1 for no status */
int rank_irc_prefix(IrcState *state, const char *symbols);
/* The highest status symbol, or an empty string */
char get_highest_prefix(const char *symbols);
