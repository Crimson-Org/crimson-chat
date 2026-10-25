#include <stdlib.h>
#include <string.h>

#include "state.h"

#define DEFAULT_PREFIX "(qaohv)~&@%+"
#define DEFAULT_CHANNEL_TYPES "#&"
#define DEFAULT_CHANNEL_MODES "beI,k,l,imnpst"


static GHashTable *create_value_table(void) {
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}


static void free_user(IrcUser *user) {
	g_free(user->nick);
	g_free(user->user);
	g_free(user->host);
	g_free(user->account);
	g_free(user->realname);
	g_hash_table_unref(user->metadata);
	g_free(user);
}


static void free_channel(IrcChannel *channel) {
	g_free(channel->name);
	g_free(channel->topic);
	g_free(channel->topic_setter);
	g_hash_table_unref(channel->members);
	g_hash_table_unref(channel->metadata);
	g_hash_table_unref(channel->modes);
	g_hash_table_unref(channel->lists);
	g_string_free(channel->listing, TRUE);
	g_free(channel);
}


static void apply_prefix(IrcState *state, const char *value) {
	const char *close = value ? strchr(value, ')') : NULL;
	if (!value || *value != '(' || !close) {
		value = DEFAULT_PREFIX;
		close = strchr(value, ')');
	}
	g_free(state->prefix_modes);
	g_free(state->prefix_symbols);
	state->prefix_modes = g_strndup(value + 1, (gsize)(close - value - 1));
	state->prefix_symbols = g_strdup(close + 1);
}


static void apply_channel_modes(IrcState *state, const char *value) {
	g_auto(GStrv) groups = g_strsplit(value ? value : DEFAULT_CHANNEL_MODES, ",", 4);
	g_free(state->list_modes);
	g_free(state->setting_modes);
	g_free(state->parameter_modes);
	guint count = g_strv_length(groups);
	state->list_modes = g_strdup(count > 0 ? groups[0] : "");
	state->setting_modes = g_strdup(count > 1 ? groups[1] : "");
	state->parameter_modes = g_strdup(count > 2 ? groups[2] : "");
}


void reset_irc_state(IrcState *state) {
	g_clear_pointer(&state->nick, g_free);
	g_clear_pointer(&state->user, g_free);
	g_clear_pointer(&state->host, g_free);
	g_clear_pointer(&state->account, g_free);
	state->is_registered = FALSE;
	state->is_away = FALSE;
	state->casemap = CASEMAP_RFC1459;
	g_hash_table_remove_all(state->support);
	g_hash_table_remove_all(state->available_caps);
	g_hash_table_remove_all(state->enabled_caps);
	g_hash_table_remove_all(state->channels);
	g_hash_table_remove_all(state->users);
	apply_prefix(state, NULL);
	apply_channel_modes(state, NULL);
	g_free(state->channel_types);
	state->channel_types = g_strdup(DEFAULT_CHANNEL_TYPES);
}


IrcState *create_irc_state(void) {
	IrcState *state = g_new0(IrcState, 1);
	state->support = create_value_table();
	state->available_caps = create_value_table();
	state->enabled_caps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	state->channels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_channel);
	state->users = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_user);
	reset_irc_state(state);
	return state;
}


void free_irc_state(IrcState *state) {
	if (!state)
		return;
	g_free(state->nick);
	g_free(state->user);
	g_free(state->host);
	g_free(state->account);
	g_hash_table_unref(state->support);
	g_hash_table_unref(state->available_caps);
	g_hash_table_unref(state->enabled_caps);
	g_hash_table_unref(state->channels);
	g_hash_table_unref(state->users);
	g_free(state->prefix_modes);
	g_free(state->prefix_symbols);
	g_free(state->channel_types);
	g_free(state->list_modes);
	g_free(state->setting_modes);
	g_free(state->parameter_modes);
	g_free(state);
}


char *fold_state_name(IrcState *state, const char *name) {
	return fold_irc_name(state->casemap, name);
}


const char *get_irc_support(IrcState *state, const char *name) {
	return g_hash_table_lookup(state->support, name);
}


gboolean has_irc_cap(IrcState *state, const char *name) {
	return g_hash_table_contains(state->enabled_caps, name);
}


gboolean is_irc_channel_name(IrcState *state, const char *name) {
	return name && *name && strchr(state->channel_types, *name);
}


gboolean is_own_nick(IrcState *state, const char *nick) {
	return state->nick && are_irc_names_equal(state->casemap, state->nick, nick);
}


IrcChannel *find_irc_channel(IrcState *state, const char *name) {
	g_autofree char *folded = fold_state_name(state, name);
	return g_hash_table_lookup(state->channels, folded);
}


IrcUser *find_irc_user(IrcState *state, const char *nick) {
	g_autofree char *folded = fold_state_name(state, nick);
	return g_hash_table_lookup(state->users, folded);
}


int rank_irc_prefix(IrcState *state, const char *symbols) {
	if (!symbols || !*symbols)
		return -1;
	const char *found = strchr(state->prefix_symbols, *symbols);
	return found ? (int)(found - state->prefix_symbols) : -1;
}


char get_highest_prefix(const char *symbols) {
	return symbols ? *symbols : '\0';
}


/* Values escape spaces and other bytes as \xHH */
static char *unescape_support_value(const char *value) {
	GString *text = g_string_new(NULL);
	for (const char *character = value; *character; character++) {
		if (character[0] == '\\' && character[1] == 'x' && g_ascii_isxdigit(character[2])
			&& g_ascii_isxdigit(character[3])) {
			g_string_append_c(text, (char)(g_ascii_xdigit_value(character[2]) * 16
				+ g_ascii_xdigit_value(character[3])));
			character += 3;
		} else {
			g_string_append_c(text, *character);
		}
	}
	return g_string_free(text, FALSE);
}


void apply_irc_support(IrcState *state, const char *token) {
	if (*token == '-') {
		g_hash_table_remove(state->support, token + 1);
		return;
	}
	const char *equals = strchr(token, '=');
	char *name = equals ? g_strndup(token, (gsize)(equals - token)) : g_strdup(token);
	g_hash_table_insert(state->support, name, equals ? unescape_support_value(equals + 1) : g_strdup(""));
	if (g_str_equal(name, "CASEMAPPING"))
		state->casemap = parse_irc_casemap(get_irc_support(state, name));
	else if (g_str_equal(name, "PREFIX"))
		apply_prefix(state, get_irc_support(state, name));
	else if (g_str_equal(name, "CHANMODES"))
		apply_channel_modes(state, get_irc_support(state, name));
	else if (g_str_equal(name, "CHANTYPES")) {
		g_free(state->channel_types);
		state->channel_types = g_strdup(get_irc_support(state, name));
	}
}


static IrcUser *ensure_user(IrcState *state, const char *nick) {
	g_autofree char *folded = fold_state_name(state, nick);
	IrcUser *user = g_hash_table_lookup(state->users, folded);
	if (user)
		return user;
	user = g_new0(IrcUser, 1);
	user->nick = g_strdup(nick);
	user->metadata = create_value_table();
	g_hash_table_insert(state->users, g_steal_pointer(&folded), user);
	return user;
}


/* Fills in what the message's source and tags tell about its sender */
static IrcUser *note_sender(IrcState *state, const IrcMessage *message) {
	if (!message->source || !strchr(message->source, '!'))
		return NULL;
	g_auto(IrcSource) parts;
	parse_irc_source(message->source, &parts);
	IrcUser *user = ensure_user(state, parts.nick);
	if (parts.user) {
		g_free(user->user);
		user->user = g_strdup(parts.user);
	}
	if (parts.host) {
		g_free(user->host);
		user->host = g_strdup(parts.host);
	}
	const char *account = get_irc_tag(message, "account");
	if (account) {
		g_free(user->account);
		user->account = g_strdup(account);
	}
	if (has_irc_tag(message, "bot"))
		user->is_bot = TRUE;
	return user;
}


static gboolean is_in_any_channel(IrcState *state, const char *folded) {
	GHashTableIter iterator;
	gpointer channel;
	g_hash_table_iter_init(&iterator, state->channels);
	while (g_hash_table_iter_next(&iterator, NULL, &channel)) {
		if (g_hash_table_contains(((IrcChannel *)channel)->members, folded))
			return TRUE;
	}
	return FALSE;
}


/* A user who shares no channel any more drops out, except the connection's own user */
static void forget_user_if_alone(IrcState *state, const char *nick) {
	g_autofree char *folded = fold_state_name(state, nick);
	if (!is_own_nick(state, nick) && !is_in_any_channel(state, folded))
		g_hash_table_remove(state->users, folded);
}


static void add_member(IrcState *state, IrcChannel *channel, const char *nick, const char *symbols) {
	ensure_user(state, nick);
	g_hash_table_insert(channel->members, fold_state_name(state, nick), g_strdup(symbols ? symbols : ""));
}


static void remove_member(IrcState *state, IrcChannel *channel, const char *nick) {
	g_autofree char *folded = fold_state_name(state, nick);
	g_hash_table_remove(channel->members, folded);
	forget_user_if_alone(state, nick);
}


static void join_channel(IrcState *state, const IrcMessage *message, IrcUser *user) {
	const char *name = get_irc_parameter(message, 0);
	if (!name || !user)
		return;
	g_autofree char *folded = fold_state_name(state, name);
	IrcChannel *channel = g_hash_table_lookup(state->channels, folded);
	if (!channel && is_own_nick(state, user->nick)) {
		channel = g_new0(IrcChannel, 1);
		channel->name = g_strdup(name);
		channel->members = create_value_table();
		channel->metadata = create_value_table();
		channel->modes = create_value_table();
		channel->lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
		channel->listing = g_string_new(NULL);
		channel->has_complete_names = TRUE;
		g_hash_table_insert(state->channels, g_strdup(folded), channel);
	}
	if (!channel)
		return;
	/* The extended JOIN form adds the account, or *, and the real name */
	const char *account = get_irc_parameter(message, 1);
	if (account) {
		g_free(user->account);
		user->account = g_str_equal(account, "*") ? NULL : g_strdup(account);
	}
	const char *realname = get_irc_parameter(message, 2);
	if (realname) {
		g_free(user->realname);
		user->realname = g_strdup(realname);
	}
	add_member(state, channel, user->nick, "");
}


static void leave_channel(IrcState *state, const char *name, const char *nick) {
	g_autofree char *folded = fold_state_name(state, name);
	IrcChannel *channel = g_hash_table_lookup(state->channels, folded);
	if (!channel || !nick)
		return;
	if (!is_own_nick(state, nick)) {
		remove_member(state, channel, nick);
		return;
	}
	g_autoptr(GPtrArray) members = g_hash_table_get_keys_as_ptr_array(channel->members);
	g_ptr_array_set_free_func(members, g_free);
	for (guint index = 0; index < members->len; index++)
		g_ptr_array_index(members, index) = g_strdup(g_ptr_array_index(members, index));
	g_hash_table_remove(state->channels, folded);
	for (guint index = 0; index < members->len; index++) {
		IrcUser *user = g_hash_table_lookup(state->users, g_ptr_array_index(members, index));
		if (user)
			forget_user_if_alone(state, user->nick);
	}
}


static void list_shared_channels(IrcState *state, const char *folded, GPtrArray *affected) {
	if (!affected)
		return;
	GHashTableIter iterator;
	gpointer channel;
	g_hash_table_iter_init(&iterator, state->channels);
	while (g_hash_table_iter_next(&iterator, NULL, &channel)) {
		if (g_hash_table_contains(((IrcChannel *)channel)->members, folded))
			g_ptr_array_add(affected, g_strdup(((IrcChannel *)channel)->name));
	}
}


static void quit_user(IrcState *state, const char *nick, GPtrArray *affected) {
	g_autofree char *folded = fold_state_name(state, nick);
	list_shared_channels(state, folded, affected);
	GHashTableIter iterator;
	gpointer channel;
	g_hash_table_iter_init(&iterator, state->channels);
	while (g_hash_table_iter_next(&iterator, NULL, &channel))
		g_hash_table_remove(((IrcChannel *)channel)->members, folded);
	if (!is_own_nick(state, nick))
		g_hash_table_remove(state->users, folded);
}


static void rename_user(IrcState *state, const char *old_nick, const char *new_nick, GPtrArray *affected) {
	g_autofree char *old_folded = fold_state_name(state, old_nick);
	g_autofree char *new_folded = fold_state_name(state, new_nick);
	list_shared_channels(state, old_folded, affected);
	GHashTableIter iterator;
	gpointer channel;
	g_hash_table_iter_init(&iterator, state->channels);
	while (g_hash_table_iter_next(&iterator, NULL, &channel)) {
		GHashTable *members = ((IrcChannel *)channel)->members;
		char *symbols = NULL;
		if (g_hash_table_steal_extended(members, old_folded, NULL, (gpointer *)&symbols))
			g_hash_table_insert(members, g_strdup(new_folded), symbols);
	}
	IrcUser *user = NULL;
	char *key = NULL;
	if (g_hash_table_steal_extended(state->users, old_folded, (gpointer *)&key, (gpointer *)&user)) {
		g_free(key);
		g_free(user->nick);
		user->nick = g_strdup(new_nick);
		g_hash_table_insert(state->users, g_strdup(new_folded), user);
	}
	if (is_own_nick(state, old_nick)) {
		g_free(state->nick);
		state->nick = g_strdup(new_nick);
	}
}


/* Splits a NAMES entry such as @+nick!user@host into its status symbols and source */
static void add_names_entry(IrcState *state, IrcChannel *channel, const char *entry) {
	const char *source = entry;
	while (*source && strchr(state->prefix_symbols, *source))
		source++;
	if (!*source)
		return;
	g_autofree char *symbols = g_strndup(entry, (gsize)(source - entry));
	g_auto(IrcSource) parts;
	parse_irc_source(source, &parts);
	add_member(state, channel, parts.nick, symbols);
	IrcUser *user = find_irc_user(state, parts.nick);
	if (parts.user && user) {
		g_free(user->user);
		user->user = g_strdup(parts.user);
		g_free(user->host);
		user->host = g_strdup(parts.host);
	}
}


static void read_names(IrcState *state, const IrcMessage *message) {
	IrcChannel *channel = find_irc_channel(state, get_irc_parameter(message, 2));
	const char *names = get_irc_parameter(message, 3);
	if (!channel || !names)
		return;
	if (channel->has_complete_names) {
		g_hash_table_remove_all(channel->members);
		channel->has_complete_names = FALSE;
	}
	g_auto(GStrv) entries = g_strsplit(names, " ", -1);
	for (int index = 0; entries[index]; index++) {
		if (*entries[index])
			add_names_entry(state, channel, entries[index]);
	}
}


/* Keeps a member's symbols in order of seniority when a status mode changes */
static void change_member_status(IrcState *state, IrcChannel *channel, const char *nick, char mode,
	gboolean is_adding) {
	g_autofree char *folded = fold_state_name(state, nick);
	const char *symbols = g_hash_table_lookup(channel->members, folded);
	const char *position = strchr(state->prefix_modes, mode);
	if (!symbols || !position)
		return;
	char symbol = state->prefix_symbols[position - state->prefix_modes];
	GString *updated = g_string_new(NULL);
	for (const char *candidate = state->prefix_symbols; *candidate; candidate++) {
		gboolean is_held = strchr(symbols, *candidate) != NULL;
		if (*candidate == symbol)
			is_held = is_adding;
		if (is_held)
			g_string_append_c(updated, *candidate);
	}
	g_hash_table_insert(channel->members, g_steal_pointer(&folded), g_string_free(updated, FALSE));
}


static gboolean takes_mode_parameter(IrcState *state, char mode, gboolean is_adding) {
	if (strchr(state->prefix_modes, mode) || strchr(state->list_modes, mode) || strchr(state->setting_modes, mode))
		return TRUE;
	return is_adding && strchr(state->parameter_modes, mode);
}


gboolean is_irc_channel_moderator(IrcState *state, IrcChannel *channel) {
	g_autofree char *own = fold_state_name(state, state->nick);
	const char *symbols = g_hash_table_lookup(channel->members, own);
	return symbols && strpbrk(symbols, "~&@%");
}


static void free_list_entry(IrcListEntry *entry) {
	g_free(entry->mask);
	g_free(entry->setter);
	g_free(entry);
}


static GPtrArray *find_list(IrcChannel *channel, char letter, gboolean is_created) {
	char key[2] = {letter, '\0'};
	GPtrArray *entries = g_hash_table_lookup(channel->lists, key);
	if (!entries && is_created) {
		entries = g_ptr_array_new_with_free_func((GDestroyNotify)free_list_entry);
		g_hash_table_insert(channel->lists, g_strdup(key), entries);
	}
	return entries;
}


static void add_list_entry(GPtrArray *entries, const char *mask, const char *setter, gint64 time) {
	IrcListEntry *entry = g_new0(IrcListEntry, 1);
	entry->mask = g_strdup(mask);
	entry->setter = g_strdup(setter);
	entry->time = time;
	g_ptr_array_add(entries, entry);
}


/* A mask set or lifted live changes a list the app already has, which the server answered before */
static void change_list(IrcChannel *channel, const IrcModeChange *change, const char *setter) {
	GPtrArray *entries = find_list(channel, change->letter, FALSE);
	if (!entries || !change->parameter)
		return;
	for (guint index = 0; index < entries->len; index++) {
		if (g_strcmp0(((IrcListEntry *)g_ptr_array_index(entries, index))->mask, change->parameter) == 0) {
			g_ptr_array_remove_index(entries, index);
			break;
		}
	}
	if (change->is_adding)
		add_list_entry(entries, change->parameter, setter, g_get_real_time() / G_USEC_PER_SEC);
}


/* Ban, invite and exception lists arrive an entry at a time, and the first one replaces the list the app had */
static void read_list_reply(IrcState *state, const IrcMessage *message, char letter, gboolean is_end) {
	IrcChannel *channel = find_irc_channel(state, get_irc_parameter(message, 1));
	if (!channel)
		return;
	gboolean is_arriving = strchr(channel->listing->str, letter) != NULL;
	if (is_end) {
		find_list(channel, letter, TRUE);
		if (is_arriving)
			g_string_erase(channel->listing, strchr(channel->listing->str, letter) - channel->listing->str, 1);
		return;
	}
	GPtrArray *entries = find_list(channel, letter, TRUE);
	if (!is_arriving) {
		g_ptr_array_set_size(entries, 0);
		g_string_append_c(channel->listing, letter);
	}
	const char *time = get_irc_parameter(message, 4);
	add_list_entry(entries, get_irc_parameter(message, 2), get_irc_parameter(message, 3),
		time ? g_ascii_strtoll(time, NULL, 10) : 0);
}


static void free_mode_change(IrcModeChange *change) {
	g_free(change->parameter);
	g_free(change);
}


GPtrArray *read_irc_mode_changes(IrcState *state, const IrcMessage *message, guint first) {
	GPtrArray *changes = g_ptr_array_new_with_free_func((GDestroyNotify)free_mode_change);
	const char *modes = get_irc_parameter(message, first);
	guint next_parameter = first + 1;
	gboolean is_adding = TRUE;
	for (const char *mode = modes ? modes : ""; *mode; mode++) {
		if (*mode == '+' || *mode == '-') {
			is_adding = *mode == '+';
			continue;
		}
		IrcModeChange *change = g_new0(IrcModeChange, 1);
		change->letter = *mode;
		change->is_adding = is_adding;
		if (takes_mode_parameter(state, *mode, is_adding))
			change->parameter = g_strdup(get_irc_parameter(message, next_parameter++));
		g_ptr_array_add(changes, change);
	}
	return changes;
}


gboolean is_irc_list_mode(IrcState *state, char letter) {
	return strchr(state->list_modes, letter) || strchr(state->prefix_modes, letter);
}


/* MODE changes settings and member status, and RPL_CHANNELMODEIS lists every setting the channel has */
static void apply_channel_mode(IrcState *state, const IrcMessage *message) {
	gboolean is_listing = g_str_equal(message->command, "324");
	guint first = is_listing ? 2 : 1;
	IrcChannel *channel = find_irc_channel(state, get_irc_parameter(message, first - 1));
	if (!channel)
		return;
	if (is_listing)
		g_hash_table_remove_all(channel->modes);
	g_autoptr(GPtrArray) changes = read_irc_mode_changes(state, message, first);
	for (guint index = 0; index < changes->len; index++) {
		IrcModeChange *change = g_ptr_array_index(changes, index);
		if (strchr(state->prefix_modes, change->letter) && change->parameter)
			change_member_status(state, channel, change->parameter, change->letter, change->is_adding);
		if (strchr(state->list_modes, change->letter) && !is_listing) {
			g_autofree char *setter = get_source_nick(message->source);
			change_list(channel, change, setter);
		}
		if (is_irc_list_mode(state, change->letter))
			continue;
		char key[2] = {change->letter, '\0'};
		if (change->is_adding)
			g_hash_table_insert(channel->modes, g_strdup(key), g_strdup(change->parameter ? change->parameter : ""));
		else
			g_hash_table_remove(channel->modes, key);
	}
}


static GHashTable *find_metadata_table(IrcState *state, const char *target) {
	if (!target)
		return NULL;
	if (g_str_equal(target, "*"))
		target = state->nick;
	if (is_irc_channel_name(state, target)) {
		IrcChannel *channel = find_irc_channel(state, target);
		return channel ? channel->metadata : NULL;
	}
	return target ? ensure_user(state, target)->metadata : NULL;
}


static void store_metadata(IrcState *state, const char *target, const char *key, const char *value) {
	GHashTable *metadata = find_metadata_table(state, target);
	if (!metadata || !key)
		return;
	if (value)
		g_hash_table_insert(metadata, g_strdup(key), g_strdup(value));
	else
		g_hash_table_remove(metadata, key);
}


static void rename_channel(IrcState *state, const char *old_name, const char *new_name) {
	g_autofree char *old_folded = fold_state_name(state, old_name);
	IrcChannel *channel = NULL;
	char *key = NULL;
	if (!new_name || !g_hash_table_steal_extended(state->channels, old_folded, (gpointer *)&key, (gpointer *)&channel))
		return;
	g_free(key);
	g_free(channel->name);
	channel->name = g_strdup(new_name);
	g_hash_table_insert(state->channels, fold_state_name(state, new_name), channel);
}


static void set_text(char **field, const char *value) {
	g_free(*field);
	*field = g_strdup(value);
}


static void update_topic(IrcState *state, const IrcMessage *message, IrcUser *sender) {
	gboolean is_change = g_str_equal(message->command, "TOPIC");
	IrcChannel *channel = find_irc_channel(state, get_irc_parameter(message, is_change ? 0 : 1));
	if (!channel)
		return;
	if (g_str_equal(message->command, "331")) {
		g_clear_pointer(&channel->topic, g_free);
	} else if (g_str_equal(message->command, "333")) {
		g_autofree char *setter = get_source_nick(get_irc_parameter(message, 2));
		set_text(&channel->topic_setter, setter);
		const char *time = get_irc_parameter(message, 3);
		channel->topic_time = time ? g_ascii_strtoll(time, NULL, 10) : 0;
	} else {
		const char *topic = get_irc_parameter(message, is_change ? 1 : 2);
		set_text(&channel->topic, topic && *topic ? topic : NULL);
		if (is_change && sender) {
			set_text(&channel->topic_setter, sender->nick);
			channel->topic_time = g_get_real_time() / G_USEC_PER_SEC;
		}
	}
}


static void update_registration(IrcState *state, const IrcMessage *message) {
	const char *command = message->command;
	if (g_str_equal(command, "001")) {
		set_text(&state->nick, get_irc_parameter(message, 0));
		state->is_registered = TRUE;
		ensure_user(state, state->nick);
	} else if (g_str_equal(command, "005")) {
		for (guint index = 1; index + 1 < count_irc_parameters(message); index++)
			apply_irc_support(state, get_irc_parameter(message, index));
	} else if (g_str_equal(command, "900")) {
		set_text(&state->account, get_irc_parameter(message, 2));
	} else if (g_str_equal(command, "901")) {
		g_clear_pointer(&state->account, g_free);
	} else if (g_str_equal(command, "396")) {
		set_text(&state->host, get_irc_parameter(message, 1));
	} else if (g_str_equal(command, "305") || g_str_equal(command, "306")) {
		state->is_away = g_str_equal(command, "306");
	}
}


/* WHO answers what a channel's people are, the extended form with their account, and the flags say who is away */
static void read_who_reply(IrcState *state, const IrcMessage *message) {
	gboolean is_extended = g_str_equal(message->command, "354");
	if (is_extended && g_strcmp0(get_irc_parameter(message, 1), WHO_TOKEN) != 0)
		return;
	/* Both forms put the nick and flags at the same place, and the extended one has the token before the user */
	const char *user_name = get_irc_parameter(message, is_extended ? 3 : 2);
	const char *host = get_irc_parameter(message, is_extended ? 4 : 3);
	const char *nick = get_irc_parameter(message, 5);
	const char *flags = get_irc_parameter(message, 6);
	if (!nick || !flags)
		return;
	IrcUser *user = ensure_user(state, nick);
	g_free(user->user);
	user->user = g_strdup(user_name);
	g_free(user->host);
	user->host = g_strdup(host);
	user->is_away = strchr(flags, 'G') != NULL;
	const char *account = is_extended ? get_irc_parameter(message, 7) : NULL;
	if (account) {
		g_free(user->account);
		user->account = g_str_equal(account, "0") ? NULL : g_strdup(account);
	}
}


static void update_user_details(IrcState *state, const IrcMessage *message, IrcUser *sender) {
	const char *command = message->command;
	const char *first = get_irc_parameter(message, 0);
	if (!sender)
		return;
	if (g_str_equal(command, "AWAY"))
		sender->is_away = first != NULL;
	else if (g_str_equal(command, "ACCOUNT"))
		set_text(&sender->account, first && !g_str_equal(first, "*") ? first : NULL);
	else if (g_str_equal(command, "CHGHOST")) {
		set_text(&sender->user, first);
		set_text(&sender->host, get_irc_parameter(message, 1));
	} else if (g_str_equal(command, "SETNAME"))
		set_text(&sender->realname, first);
	if (g_str_equal(command, "AWAY") && is_own_nick(state, sender->nick))
		state->is_away = sender->is_away;
}


static void update_metadata(IrcState *state, const IrcMessage *message) {
	const char *command = message->command;
	if (g_str_equal(command, "METADATA"))
		store_metadata(state, get_irc_parameter(message, 0), get_irc_parameter(message, 1),
			get_irc_parameter(message, 3));
	else if (g_str_equal(command, "761"))
		store_metadata(state, get_irc_parameter(message, 1), get_irc_parameter(message, 2),
			get_irc_parameter(message, 4));
	else if (g_str_equal(command, "766"))
		store_metadata(state, get_irc_parameter(message, 1), get_irc_parameter(message, 2), NULL);
}


static void update_membership(IrcState *state, const IrcMessage *message, IrcUser *sender, GPtrArray *affected) {
	const char *command = message->command;
	if (g_str_equal(command, "JOIN"))
		join_channel(state, message, sender);
	else if (g_str_equal(command, "PART") && sender)
		leave_channel(state, get_irc_parameter(message, 0), sender->nick);
	else if (g_str_equal(command, "KICK"))
		leave_channel(state, get_irc_parameter(message, 0), get_irc_parameter(message, 1));
	else if (g_str_equal(command, "QUIT") && sender)
		quit_user(state, sender->nick, affected);
	else if (g_str_equal(command, "NICK") && sender && get_irc_parameter(message, 0))
		rename_user(state, sender->nick, get_irc_parameter(message, 0), affected);
	else if (g_str_equal(command, "353"))
		read_names(state, message);
	else if (g_str_equal(command, "366")) {
		IrcChannel *channel = find_irc_channel(state, get_irc_parameter(message, 1));
		if (channel)
			channel->has_complete_names = TRUE;
	} else if (g_str_equal(command, "MODE") || g_str_equal(command, "324"))
		apply_channel_mode(state, message);
	else if (g_str_equal(command, "367") || g_str_equal(command, "368"))
		read_list_reply(state, message, 'b', g_str_equal(command, "368"));
	else if (g_str_equal(command, "348") || g_str_equal(command, "349"))
		read_list_reply(state, message, 'e', g_str_equal(command, "349"));
	else if (g_str_equal(command, "346") || g_str_equal(command, "347"))
		read_list_reply(state, message, 'I', g_str_equal(command, "347"));
	else if (g_str_equal(command, "RENAME"))
		rename_channel(state, get_irc_parameter(message, 0), get_irc_parameter(message, 1));
}


void update_irc_state(IrcState *state, const IrcMessage *message, GPtrArray *affected) {
	IrcUser *sender = note_sender(state, message);
	const char *command = message->command;
	if (g_str_equal(command, "TOPIC") || g_str_equal(command, "331") || g_str_equal(command, "332")
		|| g_str_equal(command, "333"))
		update_topic(state, message, sender);
	update_registration(state, message);
	update_user_details(state, message, sender);
	update_metadata(state, message);
	update_membership(state, message, sender, affected);
	if (g_str_equal(command, "352") || g_str_equal(command, "354"))
		read_who_reply(state, message);
}
