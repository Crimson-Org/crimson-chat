#include <string.h>

#include "bouncer.h"

#define JOIN_LINE_BYTES 400

/* Everything the bouncer can pass on or use, which the server grants where it supports it */
static const char *const WANTED_CAPS[] = {
	"message-tags", "server-time", "batch", "echo-message", "sasl", "cap-notify", "account-notify",
	"away-notify", "extended-join", "multi-prefix", "userhost-in-names", "chghost", "setname", "invite-notify",
	"account-tag", "draft/message-redaction", "draft/metadata-2", "draft/channel-rename", "extended-monitor",
	"sts", NULL,
};

/* Replies the bouncer answers itself when a client binds, so the server's own copies stay with the bouncer */
static const char *const KEPT_COMMANDS[] = {
	"CAP", "AUTHENTICATE", "001", "002", "003", "004", "005", "250", "251", "252", "253", "254", "255", "265",
	"266", "372", "375", "376", "422", "900", "901", "902", "903", "904", "905", "906", "907", "908", NULL,
};


static void free_pending_echo(PendingEcho *echo) {
	g_free(echo->command);
	g_free(echo->target);
	g_free(echo);
}


static void fill_client_settings(IrcClientSettings *settings, const NetworkRecord *record, const char *user_name) {
	const char *nick = record->nickname && *record->nickname ? record->nickname : user_name;
	*settings = (IrcClientSettings){
		.host = record->host,
		.port = record->port,
		.uses_tls = record->uses_tls,
		.nick = (char *)nick,
		.username = record->username && *record->username ? record->username : (char *)nick,
		.realname = record->realname && *record->realname ? record->realname : (char *)nick,
		.password = record->password,
		.sasl_username = record->sasl_username,
		.sasl_password = record->sasl_password,
	};
}


char *get_upstream_nick(Upstream *upstream) {
	IrcState *state = get_irc_client_state(upstream->client);
	if (state->nick)
		return g_strdup(state->nick);
	const char *nick = upstream->record.nickname;
	return g_strdup(nick && *nick ? nick : upstream->user->name);
}


const char *describe_upstream_state(Upstream *upstream) {
	switch (get_irc_client_status(upstream->client)) {
	case IRC_CLIENT_REGISTERED: return "connected";
	case IRC_CLIENT_CONNECTING: return "connecting";
	default: return "disconnected";
	}
}


static void add_attribute(GHashTable *attributes, const char *name, const char *value) {
	if (value && *value)
		g_hash_table_insert(attributes, g_strdup(name), g_strdup(value));
}


GHashTable *describe_upstream(Upstream *upstream) {
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const NetworkRecord *record = &upstream->record;
	g_autofree char *port = g_strdup_printf("%d", record->port);
	add_attribute(attributes, "name", record->name && *record->name ? record->name : record->host);
	add_attribute(attributes, "state", describe_upstream_state(upstream));
	add_attribute(attributes, "host", record->host);
	add_attribute(attributes, "port", port);
	add_attribute(attributes, "tls", record->uses_tls ? "1" : "0");
	add_attribute(attributes, "nickname", record->nickname);
	add_attribute(attributes, "username", record->username);
	add_attribute(attributes, "realname", record->realname);
	add_attribute(attributes, "sasl-username", record->sasl_username);
	return attributes;
}


static void for_each_bound_downstream(Upstream *upstream, void (*function)(Downstream *, gpointer), gpointer data) {
	for (GList *item = upstream->user->bouncer->downstreams; item;) {
		Downstream *downstream = item->data;
		item = item->next;
		if (downstream->upstream == upstream && downstream->is_registered)
			function(downstream, data);
	}
}


static void send_join(Upstream *upstream, GString *keyed, GString *plain, GString *keys) {
	if (!keyed->len && !plain->len)
		return;
	/* Keys pair with the names in order, so the channels with keys go first */
	g_autofree char *names = g_strconcat(keyed->str, keyed->len && plain->len ? "," : "", plain->str, NULL);
	send_irc_client_command(upstream->client, "JOIN", names, keys->len ? keys->str : NULL, NULL);
	g_string_truncate(keyed, 0);
	g_string_truncate(plain, 0);
	g_string_truncate(keys, 0);
}


/* Rejoins the saved channels, several to a line, with their keys */
static void join_saved_channels(Upstream *upstream) {
	g_autoptr(GPtrArray) channels = load_channels(upstream->user->bouncer->database, upstream->record.id);
	g_autoptr(GString) keyed = g_string_new(NULL), plain = g_string_new(NULL), keys = g_string_new(NULL);
	for (guint index = 0; index < channels->len; index++) {
		ChannelRecord *channel = g_ptr_array_index(channels, index);
		if (keyed->len + plain->len + keys->len + strlen(channel->name) > JOIN_LINE_BYTES)
			send_join(upstream, keyed, plain, keys);
		gboolean has_key = channel->key && *channel->key;
		GString *names = has_key ? keyed : plain;
		g_string_append_printf(names, "%s%s", names->len ? "," : "", channel->name);
		if (has_key)
			g_string_append_printf(keys, "%s%s", keys->len ? "," : "", channel->key);
	}
	send_join(upstream, keyed, plain, keys);
}


static void tell_disconnected(Downstream *downstream, gpointer reason) {
	g_autofree char *text = g_strdup_printf("Disconnected from the network: %s",
		reason ? (const char *)reason : "no reason given");
	g_autoptr(IrcMessage) notice = create_irc_message("NOTICE", downstream->nick, text, NULL);
	notice->source = g_strdup(BOUNCER_NAME);
	send_to_downstream(downstream, notice);
}


/* Clients keep the nick and network details they were told, so a reconnect brings both up to date */
static void tell_new_nick(Downstream *downstream, gpointer data) {
	g_autofree char *nick = get_upstream_nick(downstream->upstream);
	if (g_strcmp0(nick, downstream->nick) != 0) {
		g_autoptr(IrcMessage) message = create_irc_message("NICK", nick, NULL);
		message->source = g_strdup(downstream->nick);
		send_to_downstream(downstream, message);
		g_free(downstream->nick);
		downstream->nick = g_steal_pointer(&nick);
	}
	send_support_tokens(downstream);
}


static void handle_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	Upstream *upstream = data;
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	add_attribute(attributes, "state", describe_upstream_state(upstream));
	g_hash_table_insert(attributes, g_strdup("error"), g_strdup(status == IRC_CLIENT_DISCONNECTED && reason
		? reason : ""));
	announce_network(upstream, attributes);
	g_message("%s on %s is %s%s%s", upstream->user->name, upstream->record.host,
		describe_upstream_state(upstream), reason ? ": " : "", reason ? reason : "");
	if (status == IRC_CLIENT_REGISTERED) {
		join_saved_channels(upstream);
		for_each_bound_downstream(upstream, tell_new_nick, NULL);
	} else if (status == IRC_CLIENT_DISCONNECTED) {
		g_queue_clear_full(&upstream->pending_echoes, (GDestroyNotify)free_pending_echo);
		for_each_bound_downstream(upstream, tell_disconnected, (gpointer)reason);
	}
}


static void remember_channel(Upstream *upstream, const IrcMessage *message) {
	IrcState *state = get_irc_client_state(upstream->client);
	g_autofree char *nick = get_source_nick(message->source);
	const char *channel = get_irc_parameter(message, 0);
	gboolean is_kick = g_str_equal(message->command, "KICK");
	if (!channel || !is_own_nick(state, is_kick ? get_irc_parameter(message, 1) : nick))
		return;
	g_autofree char *folded = fold_state_name(state, channel);
	Database *database = upstream->user->bouncer->database;
	if (g_str_equal(message->command, "JOIN"))
		save_channel(database, upstream->record.id, channel, folded, g_hash_table_lookup(upstream->channel_keys,
			folded));
	else
		delete_channel(database, upstream->record.id, folded);
	g_hash_table_remove(upstream->channel_keys, folded);
}


static gboolean is_echoable(const char *command) {
	return g_str_equal(command, "PRIVMSG") || g_str_equal(command, "NOTICE") || g_str_equal(command, "TAGMSG");
}


/* The first awaited echo matching the message, which the server returns in the order it was sent */
static Downstream *take_echo_origin(Upstream *upstream, const IrcMessage *message, gboolean *is_found) {
	IrcState *state = get_irc_client_state(upstream->client);
	for (GList *item = upstream->pending_echoes.head; item; item = item->next) {
		PendingEcho *echo = item->data;
		if (g_str_equal(echo->command, message->command)
			&& are_irc_names_equal(state->casemap, echo->target, get_irc_parameter(message, 0))) {
			Downstream *origin = echo->origin;
			g_queue_delete_link(&upstream->pending_echoes, item);
			free_pending_echo(echo);
			*is_found = TRUE;
			return origin;
		}
	}
	*is_found = FALSE;
	return NULL;
}


typedef struct {
	IrcMessage *message;
	Downstream *origin;
	gboolean skips_origin;
} Delivery;


static void deliver(Downstream *downstream, gpointer data) {
	Delivery *delivery = data;
	/* A client without echo-message already shows what it sent */
	if (delivery->skips_origin && downstream == delivery->origin && !has_downstream_cap(downstream, "echo-message"))
		return;
	send_to_downstream(downstream, delivery->message);
}


static void relay_to_clients(Upstream *upstream, IrcMessage *message, Downstream *origin, gboolean skips_origin) {
	Delivery delivery = {message, origin, skips_origin};
	for_each_bound_downstream(upstream, deliver, &delivery);
}


static void handle_message(IrcClient *client, const IrcMessage *message, GPtrArray *affected, gpointer data) {
	Upstream *upstream = data;
	if (g_strv_contains(KEPT_COMMANDS, message->command))
		return;
	const char *command = message->command;
	if (g_str_equal(command, "JOIN") || g_str_equal(command, "PART") || g_str_equal(command, "KICK"))
		remember_channel(upstream, message);
	g_autoptr(IrcMessage) copy = copy_irc_message(message);
	if (!get_irc_tag(copy, "time")) {
		g_autofree char *now = format_irc_time_now();
		set_irc_tag(copy, "time", now);
	}
	Downstream *origin = NULL;
	gboolean is_echo = FALSE;
	g_autofree char *nick = get_source_nick(copy->source);
	if (is_echoable(command) && is_own_nick(get_irc_client_state(client), nick))
		origin = take_echo_origin(upstream, copy, &is_echo);
	store_upstream_message(upstream, copy, affected);
	relay_to_clients(upstream, copy, origin, is_echo);
}


Upstream *create_upstream(BouncerUser *user, const NetworkRecord *record) {
	Upstream *upstream = g_new0(Upstream, 1);
	upstream->user = user;
	copy_network_record(&upstream->record, record);
	g_queue_init(&upstream->pending_echoes);
	upstream->channel_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	IrcClientSettings settings;
	fill_client_settings(&settings, record, user->name);
	static const IrcClientEvents EVENTS = {handle_message, handle_status};
	upstream->client = create_irc_client(&settings, WANTED_CAPS, &EVENTS, upstream);
	return upstream;
}


void free_upstream(Upstream *upstream) {
	disconnect_irc_client(upstream->client, NULL);
	free_irc_client(upstream->client);
	clear_network_record(&upstream->record);
	g_queue_clear_full(&upstream->pending_echoes, (GDestroyNotify)free_pending_echo);
	g_hash_table_unref(upstream->channel_keys);
	g_free(upstream);
}


void restart_upstream(Upstream *upstream) {
	IrcClientSettings settings;
	fill_client_settings(&settings, &upstream->record, upstream->user->name);
	disconnect_irc_client(upstream->client, "Reconnecting");
	change_irc_client_settings(upstream->client, &settings);
	connect_irc_client(upstream->client);
}


/* Keeps only the client tags a server passes between users, and none for a server without message tags */
static void keep_client_tags(IrcState *state, IrcMessage *message) {
	if (!has_irc_cap(state, "message-tags")) {
		g_hash_table_remove_all(message->tags);
		return;
	}
	GHashTableIter iterator;
	gpointer name;
	g_hash_table_iter_init(&iterator, message->tags);
	while (g_hash_table_iter_next(&iterator, &name, NULL)) {
		if (*(const char *)name != '+')
			g_hash_table_iter_remove(&iterator);
	}
}


static void note_requested_join(Upstream *upstream, const IrcMessage *message) {
	IrcState *state = get_irc_client_state(upstream->client);
	const char *keys = get_irc_parameter(message, 1);
	if (!keys || !get_irc_parameter(message, 0))
		return;
	g_auto(GStrv) names = g_strsplit(get_irc_parameter(message, 0), ",", -1);
	g_auto(GStrv) values = g_strsplit(keys, ",", -1);
	for (int index = 0; names[index] && values[index]; index++)
		g_hash_table_insert(upstream->channel_keys, fold_state_name(state, names[index]), g_strdup(values[index]));
}


/* A nick the user picks becomes the one the bouncer registers with from then on */
static void save_requested_nick(Upstream *upstream, const char *nick) {
	if (!nick || !*nick)
		return;
	g_free(upstream->record.nickname);
	upstream->record.nickname = g_strdup(nick);
	save_network(upstream->user->bouncer->database, &upstream->record);
	IrcClientSettings settings;
	fill_client_settings(&settings, &upstream->record, upstream->user->name);
	change_irc_client_settings(upstream->client, &settings);
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	add_attribute(attributes, "nickname", nick);
	announce_network(upstream, attributes);
}


/* A server without echo-message never repeats what was sent, so the bouncer makes the copy other clients see */
static void echo_locally(Upstream *upstream, Downstream *origin, const IrcMessage *sent) {
	IrcState *state = get_irc_client_state(upstream->client);
	g_autoptr(IrcMessage) echo = copy_irc_message(sent);
	g_autofree char *nick = get_upstream_nick(upstream);
	IrcUser *self = find_irc_user(state, nick);
	echo->source = self && self->user && self->host ? g_strdup_printf("%s!%s@%s", nick, self->user, self->host)
		: g_strdup(nick);
	g_autofree char *now = format_irc_time_now();
	g_autofree char *message_id = create_irc_identifier();
	set_irc_tag(echo, "time", now);
	set_irc_tag(echo, "msgid", message_id);
	store_upstream_message(upstream, echo, NULL);
	relay_to_clients(upstream, echo, origin, TRUE);
}


void forward_to_upstream(Upstream *upstream, Downstream *origin, const IrcMessage *message) {
	IrcState *state = get_irc_client_state(upstream->client);
	const char *command = message->command;
	if (g_str_equal(command, "TAGMSG") && !has_irc_cap(state, "message-tags"))
		return;
	g_autoptr(IrcMessage) copy = copy_irc_message(message);
	g_clear_pointer(&copy->source, g_free);
	keep_client_tags(state, copy);
	if (g_str_equal(command, "JOIN"))
		note_requested_join(upstream, copy);
	else if (g_str_equal(command, "NICK"))
		save_requested_nick(upstream, get_irc_parameter(copy, 0));
	send_irc_client_message(upstream->client, copy);
	if (!is_echoable(command) || !get_irc_parameter(copy, 0) || get_irc_client_status(upstream->client)
		!= IRC_CLIENT_REGISTERED)
		return;
	if (!has_irc_cap(state, "echo-message")) {
		echo_locally(upstream, origin, copy);
		return;
	}
	PendingEcho *echo = g_new0(PendingEcho, 1);
	echo->origin = origin;
	echo->command = g_strdup(command);
	echo->target = g_strdup(get_irc_parameter(copy, 0));
	g_queue_push_tail(&upstream->pending_echoes, echo);
}
