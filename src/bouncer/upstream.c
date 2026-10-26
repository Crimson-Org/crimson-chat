#include <string.h>

#include "bouncer.h"

#define JOIN_LINE_BYTES 400
#define CTCP_MARK '\001'

/* Everything the bouncer can pass on or use, which the server grants where it supports it */
static const char *const WANTED_CAPS[] = {
	"message-tags", "server-time", "batch", "echo-message", "labeled-response", "sasl", "cap-notify", "account-notify",
	"away-notify", "extended-join", "multi-prefix", "userhost-in-names", "chghost", "setname", "invite-notify",
	"account-tag", "draft/message-redaction", "draft/metadata-2", "draft/channel-rename", "extended-monitor",
	"sts", "draft/account-registration", NULL,
};

/* The server's welcome, which the bouncer answers itself when an app logs in, so the server's own copies stay with
 * the bouncer until the welcome ends, while later ones such as an asked for message of the day pass on */
static const char *const WELCOME_COMMANDS[] = {
	"001", "002", "003", "004", "005", "250", "251", "252", "253", "254", "255", "265",
	"266", "372", "375", "376", "422", "902", "903", "904", "905", "906", "907", "908", NULL,
};


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


static const char *describe_failure(IrcClient *client) {
	switch (get_irc_client_failure(client)) {
	case IRC_FAILURE_LOGIN: return "login";
	case IRC_FAILURE_CERTIFICATE: return "certificate";
	case IRC_FAILURE_NETWORK: return "network";
	default: return "";
	}
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
	if (upstream->client && get_irc_client_status(upstream->client) == IRC_CLIENT_DISCONNECTED) {
		add_attribute(attributes, "error", upstream->error);
		add_attribute(attributes, RETRY_ATTRIBUTE, will_irc_client_retry(upstream->client) ? "1" : "0");
		add_attribute(attributes, FAILURE_ATTRIBUTE, describe_failure(upstream->client));
	}
	return attributes;
}


static void for_each_downstream(Upstream *upstream, void (*function)(Downstream *, gpointer), gpointer data) {
	for (GList *item = upstream->user->bouncer->downstreams; item;) {
		Downstream *downstream = item->data;
		item = item->next;
		if (downstream->user == upstream->user && downstream->is_registered)
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


/* Servers forget the user's display name, picture and the like when the connection ends, so the bouncer sets what
 * the user last chose again on each connection */
static void restore_own_profile(Upstream *upstream) {
	g_autoptr(GHashTable) values = load_own_profile(upstream->user->bouncer->database, upstream->record.id);
	GHashTableIter iterator;
	gpointer key, value;
	g_hash_table_iter_init(&iterator, values);
	while (g_hash_table_iter_next(&iterator, &key, &value))
		send_irc_client_command(upstream->client, "METADATA", "*", "SET", key, value, NULL);
}


static void handle_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	Upstream *upstream = data;
	if (upstream->is_removed)
		return;
	upstream->is_welcomed = FALSE;
	g_free(upstream->error);
	g_free(upstream->registering_account);
	g_free(upstream->registering_password);
	upstream->error = status == IRC_CLIENT_DISCONNECTED && reason ? g_strdup(reason) : NULL;
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	add_attribute(attributes, "state", describe_upstream_state(upstream));
	g_hash_table_insert(attributes, g_strdup("error"), g_strdup(status == IRC_CLIENT_DISCONNECTED && reason
		? reason : ""));
	/* Whether the bouncer will try again by itself, so clients only say they reconnect when that is true */
	g_hash_table_insert(attributes, g_strdup(RETRY_ATTRIBUTE), g_strdup(status != IRC_CLIENT_DISCONNECTED ? ""
		: will_irc_client_retry(client) ? "1" : "0"));
	g_hash_table_insert(attributes, g_strdup(FAILURE_ATTRIBUTE), g_strdup(status != IRC_CLIENT_DISCONNECTED ? ""
		: describe_failure(client)));
	announce_network(upstream, attributes);
	gboolean is_final = status == IRC_CLIENT_DISCONNECTED && !will_irc_client_retry(client);
	g_message("%s on %s is %s%s%s%s", upstream->user->name, upstream->record.host,
		describe_upstream_state(upstream), reason ? ": " : "", reason ? reason : "",
		is_final ? ", not retrying" : status == IRC_CLIENT_DISCONNECTED ? ", retrying" : "");
	if (status == IRC_CLIENT_REGISTERED) {
		/* The server sends a room's picture and its people's profiles on join only to a connection that follows
		 * them, so the bouncer follows them itself before any client is there to ask */
		if (has_irc_cap(get_irc_client_state(client), "draft/metadata-2")) {
			send_irc_client_command(client, "METADATA", "*", "SUB", FOLLOWED_PROFILE_KEYS, NULL);
			restore_own_profile(upstream);
		}
		join_saved_channels(upstream);
		upstream->is_away_for_absence = FALSE;
		upstream->own_away_answers = 0;
		follow_user_presence(upstream->user);
	} else if (status == IRC_CLIENT_DISCONNECTED) {
		g_hash_table_remove_all(upstream->answer_batches);
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
	/* A channel just joined is asked who its people are, which learns their accounts and who is away */
	if (g_str_equal(message->command, "JOIN") && get_irc_support(state, "WHOX"))
		send_irc_client_command(upstream->client, "WHO", channel, WHO_FIELDS, NULL);
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


/* Where a labeled line goes, the device that asked with its own label, or none for a line for everyone */
typedef struct {
	Downstream *origin;
	char *label;
} Answer;


static Downstream *find_downstream(Upstream *upstream, guint id) {
	for (GList *item = upstream->user->bouncer->downstreams; item; item = item->next) {
		Downstream *downstream = item->data;
		if (downstream->id == id && downstream->user == upstream->user && downstream->is_registered)
			return downstream;
	}
	return NULL;
}


/* A label the bouncer passed on starts with the asking device's number, as in 12.abc, which leads the answer back */
static Answer read_answer(Upstream *upstream, const IrcMessage *message) {
	const char *label = get_irc_tag(message, "label");
	const char *batch = get_irc_tag(message, "batch");
	if (!label && batch)
		label = g_hash_table_lookup(upstream->answer_batches, batch);
	const char *dot = label ? strchr(label, '.') : NULL;
	if (!dot)
		return (Answer){0};
	Downstream *origin = find_downstream(upstream, (guint)g_ascii_strtoull(label, NULL, 10));
	return (Answer){origin, origin && get_irc_tag(message, "label") ? g_strdup(dot + 1) : NULL};
}


/* A batch the server opens in answer to a labeled line carries the label for every line inside it */
static void follow_answer_batch(Upstream *upstream, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	const char *label = get_irc_tag(message, "label");
	if (!g_str_equal(message->command, "BATCH") || !reference)
		return;
	if (*reference == '+' && label)
		g_hash_table_insert(upstream->answer_batches, g_strdup(reference + 1), g_strdup(label));
}


static void forget_answer_batch(Upstream *upstream, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	if (g_str_equal(message->command, "BATCH") && reference && *reference == '-')
		g_hash_table_remove(upstream->answer_batches, reference + 1);
}


/* An answer goes only to the device that asked, with its own label back, while an echo of what that device sent
 * reaches the user's other devices too, without the label */
static void relay_to_clients(Upstream *upstream, IrcMessage *message, const Answer *answer) {
	g_autofree char *nick = get_source_nick(message->source);
	gboolean is_echo = is_echoable(message->command) && is_own_nick(get_irc_client_state(upstream->client), nick);
	if (answer->origin) {
		g_autoptr(IrcMessage) labeled = copy_irc_message(message);
		if (answer->label)
			set_irc_tag(labeled, "label", answer->label);
		send_to_downstream(answer->origin, upstream, labeled);
	}
	if (answer->origin && !is_echo)
		return;
	for (GList *item = upstream->user->bouncer->downstreams; item; item = item->next) {
		Downstream *downstream = item->data;
		if (downstream->user == upstream->user && downstream->is_registered && downstream != answer->origin)
			send_to_downstream(downstream, upstream, message);
	}
}


/* A registration the server completed becomes the network's login, so the bouncer logs in by itself next time,
 * without reconnecting now since the server already counts the connection as logged in */
static void keep_new_login(Upstream *upstream, const IrcMessage *message) {
	const char *command = message->command;
	gboolean is_done = (g_str_equal(command, "REGISTER") || g_str_equal(command, "VERIFY"))
		&& g_strcmp0(get_irc_parameter(message, 0), "SUCCESS") == 0;
	if (!is_done || !upstream->registering_password)
		return;
	const char *account = get_irc_parameter(message, 1);
	g_free(upstream->record.sasl_username);
	g_free(upstream->record.sasl_password);
	upstream->record.sasl_username = g_strdup(account ? account : upstream->registering_account);
	upstream->record.sasl_password = g_steal_pointer(&upstream->registering_password);
	g_clear_pointer(&upstream->registering_account, g_free);
	save_network(upstream->user->bouncer->database, &upstream->record);
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(attributes, g_strdup("sasl-username"), g_strdup(upstream->record.sasl_username));
	announce_network(upstream, attributes);
}


/* Whether the message is a client query or its answer, such as VERSION, rather than a /me action */
static gboolean is_ctcp_query(const IrcMessage *message) {
	const char *text = get_last_irc_parameter(message);
	gboolean is_text = g_str_equal(message->command, "PRIVMSG") || g_str_equal(message->command, "NOTICE");
	return is_text && text && *text == CTCP_MARK && g_ascii_strncasecmp(text + 1, "ACTION ", 7) != 0;
}


/* The bouncer answers client queries itself, once for all of the user's devices and while none is connected */
static void answer_ctcp(Upstream *upstream, const IrcMessage *message) {
	g_autofree char *sender = get_source_nick(message->source);
	g_autofree char *request = g_strdup(get_last_irc_parameter(message) + 1);
	char *end = strchr(request, CTCP_MARK);
	if (end)
		*end = '\0';
	g_autofree char *answer = NULL;
	if (g_ascii_strncasecmp(request, "VERSION", 7) == 0)
		answer = g_strdup_printf("VERSION Crimson Chat %s", APPLICATION_VERSION);
	else if (g_ascii_strncasecmp(request, "PING", 4) == 0)
		answer = g_strdup(request);
	else if (g_ascii_strncasecmp(request, "TIME", 4) == 0) {
		g_autoptr(GDateTime) now = g_date_time_new_now_local();
		g_autofree char *moment = g_date_time_format(now, "%a %b %d %H:%M:%S %Y");
		answer = g_strdup_printf("TIME %s", moment);
	}
	else if (g_ascii_strncasecmp(request, "CLIENTINFO", 10) == 0)
		answer = g_strdup("CLIENTINFO ACTION CLIENTINFO PING TIME VERSION");
	if (!answer || !g_str_equal(message->command, "PRIVMSG"))
		return;
	g_autofree char *line = g_strdup_printf("%c%s%c", CTCP_MARK, answer, CTCP_MARK);
	send_irc_client_command(upstream->client, "NOTICE", sender, line, NULL);
}


/* Each message of the day, the one of a new connection and one asked for alike, goes into the server's chat as a
 * single line holding all of it, so a page of history never cuts it apart */
static IrcMessage *keep_welcome_message(Upstream *upstream, const IrcMessage *message) {
	const char *command = message->command;
	if (g_str_equal(command, "375")) {
		g_clear_pointer(&upstream->welcome_message, g_free);
		upstream->welcome_message = g_strdup("");
	} else if (g_str_equal(command, "372") && upstream->welcome_message) {
		char *longer = g_strconcat(upstream->welcome_message, *upstream->welcome_message ? "\n" : "",
			get_last_irc_parameter(message), NULL);
		g_free(upstream->welcome_message);
		upstream->welcome_message = longer;
	} else if (g_str_equal(command, "376") && upstream->welcome_message) {
		g_autoptr(IrcMessage) kept = create_irc_message("CRIMSON", "MOTD", NULL);
		kept->source = g_strdup(message->source);
		g_autofree char *now = format_irc_time_now();
		set_irc_tag(kept, "time", get_irc_tag(message, "time") ? get_irc_tag(message, "time") : now);
		set_irc_tag(kept, WELCOME_MESSAGE_TAG, upstream->welcome_message);
		g_clear_pointer(&upstream->welcome_message, g_free);
		store_server_line(upstream, kept);
		return g_steal_pointer(&kept);
	}
	return NULL;
}


static void handle_message(IrcClient *client, const IrcMessage *message, GPtrArray *affected, gpointer data) {
	Upstream *upstream = data;
	if (is_ctcp_query(message)) {
		answer_ctcp(upstream, message);
		return;
	}
	/* Capability and login lines belong to the bouncer's own connection, though the apps hear when the network's
	 * capabilities change later */
	const char *change = g_str_equal(message->command, "CAP") ? get_irc_parameter(message, 1) : NULL;
	if (upstream->is_welcomed && (g_strcmp0(change, "ACK") == 0 || g_strcmp0(change, "DEL") == 0))
		for_each_downstream(upstream, send_network_caps, upstream);
	if (g_str_equal(message->command, "CAP") || g_str_equal(message->command, "AUTHENTICATE"))
		return;
	/* An asked for message of the day reaches the device that asked as the same single line, in place of its parts */
	g_autoptr(IrcMessage) welcome_message = keep_welcome_message(upstream, message);
	gboolean is_motd = g_str_equal(message->command, "375") || g_str_equal(message->command, "372")
		|| g_str_equal(message->command, "376");
	if (upstream->is_welcomed && is_motd) {
		if (welcome_message) {
			if (get_irc_tag(message, "batch"))
				set_irc_tag(welcome_message, "batch", get_irc_tag(message, "batch"));
			Answer answer = read_answer(upstream, message);
			relay_to_clients(upstream, welcome_message, &answer);
			g_free(answer.label);
		}
		return;
	}
	/* The end of the message of the day closes the server's welcome, after its details have all arrived */
	gboolean is_welcome_end = g_str_equal(message->command, "376") || g_str_equal(message->command, "422");
	if (!upstream->is_welcomed && is_welcome_end) {
		upstream->is_welcomed = TRUE;
		for_each_downstream(upstream, welcome_network, upstream);
		return;
	}
	if (!upstream->is_welcomed && g_strv_contains(WELCOME_COMMANDS, message->command))
		return;
	gboolean is_away_answer = g_str_equal(message->command, "305") || g_str_equal(message->command, "306");
	if (is_away_answer && upstream->own_away_answers) {
		upstream->own_away_answers--;
		return;
	}
	const char *command = message->command;
	keep_new_login(upstream, message);
	if (g_str_equal(command, "JOIN") || g_str_equal(command, "PART") || g_str_equal(command, "KICK"))
		remember_channel(upstream, message);
	g_autoptr(IrcMessage) copy = copy_irc_message(message);
	if (!get_irc_tag(copy, "time")) {
		g_autofree char *now = format_irc_time_now();
		set_irc_tag(copy, "time", now);
	}
	follow_answer_batch(upstream, copy);
	Answer answer = read_answer(upstream, copy);
	g_hash_table_remove(copy->tags, "label");
	if (upstream->is_welcomed && is_server_line(copy, answer.origin != NULL))
		store_server_line(upstream, copy);
	else
		store_upstream_message(upstream, copy, affected);
	relay_to_clients(upstream, copy, &answer);
	forget_answer_batch(upstream, copy);
	g_free(answer.label);
}


Upstream *create_upstream(BouncerUser *user, const NetworkRecord *record) {
	Upstream *upstream = g_new0(Upstream, 1);
	upstream->user = user;
	copy_network_record(&upstream->record, record);
	upstream->answer_batches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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
	g_hash_table_unref(upstream->answer_batches);
	g_hash_table_unref(upstream->channel_keys);
	g_free(upstream->welcome_message);
	g_free(upstream->error);
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


/* A server without echo-message never repeats what was sent, so the bouncer makes the copy every device sees, with
 * the label of the device that sent it */
static void echo_locally(Upstream *upstream, Downstream *origin, const IrcMessage *sent, const char *label) {
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
	Answer answer = {origin, g_strdup(label)};
	relay_to_clients(upstream, echo, &answer);
	g_free(answer.label);
}


void forward_to_upstream(Upstream *upstream, Downstream *origin, const IrcMessage *message) {
	IrcState *state = get_irc_client_state(upstream->client);
	const char *command = message->command;
	if (g_str_equal(command, "TAGMSG") && !has_irc_cap(state, "message-tags"))
		return;
	g_autoptr(IrcMessage) copy = copy_irc_message(message);
	g_clear_pointer(&copy->source, g_free);
	keep_client_tags(state, copy);
	/* The label goes up with the device's number before it, so the answer finds its way back to that device */
	const char *label = get_irc_tag(message, "label");
	if (label && has_irc_cap(state, "labeled-response")) {
		g_autofree char *routed = g_strdup_printf("%u.%s", origin->id, label);
		set_irc_tag(copy, "label", routed);
	}
	if (g_str_equal(command, "REGISTER") && get_irc_parameter(copy, 2)) {
		const char *account = get_irc_parameter(copy, 0);
		g_free(upstream->registering_account);
		g_free(upstream->registering_password);
		upstream->registering_account = g_str_equal(account, "*") ? get_upstream_nick(upstream) : g_strdup(account);
		upstream->registering_password = g_strdup(get_irc_parameter(copy, 2));
	}
	if (g_str_equal(command, "METADATA") && g_strcmp0(get_irc_parameter(copy, 0), "*") == 0
		&& g_strcmp0(get_irc_parameter(copy, 1), "SET") == 0 && get_irc_parameter(copy, 2))
		save_own_profile_value(upstream->user->bouncer->database, upstream->record.id, get_irc_parameter(copy, 2),
			get_irc_parameter(copy, 3));
	if (g_str_equal(command, "JOIN"))
		note_requested_join(upstream, copy);
	else if (g_str_equal(command, "NICK"))
		save_requested_nick(upstream, get_irc_parameter(copy, 0));
	send_irc_client_message(upstream->client, copy);
	if (!is_echoable(command) || !get_irc_parameter(copy, 0) || get_irc_client_status(upstream->client)
		!= IRC_CLIENT_REGISTERED)
		return;
	if (!has_irc_cap(state, "echo-message")) {
		g_hash_table_remove(copy->tags, "label");
		echo_locally(upstream, origin, copy, label);
	}
}
