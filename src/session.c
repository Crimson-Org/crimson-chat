#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "questions.h"

#define CACHE_DELAY_SECONDS 2
#define WORK_KEY "work"
/* How long what the server said about a person or room is trusted before the app asks again, both that it cannot
 * reach them and what their profile holds */
#define ANSWER_KEEP_SECONDS 120
#define NICKSERV "NickServ"
/* The bouncer's word on whether it reconnects a network by itself */
#define RETRY_ATTRIBUTE "crimson-chat/retrying"
/* The name a bouncer announces when its owner gave it none */
#define BOUNCER_DEFAULT_NAME "crimson-chat-bouncer"
/* A server that can filter first lists only channels above this size, and lists all of them when that
 * finds fewer than the count after it, as on a small network */
#define BUSY_ROOM_FILTER ">9"
#define FEW_BUSY_ROOMS 20

/* Every capability Crimson Chat understands, which servers grant where they support it */
static const char *const CLIENT_CAPS[] = {
	"message-tags", "server-time", "batch", "echo-message", "sasl", "cap-notify", "account-notify", "away-notify",
	"extended-join", "multi-prefix", "userhost-in-names", "chghost", "setname", "invite-notify", "account-tag",
	"labeled-response", "draft/chathistory", "draft/event-playback", "draft/read-marker", "draft/message-redaction",
	"draft/metadata-2", "draft/channel-rename", "draft/multiline", "extended-monitor", "sts",
	"soju.im/bouncer-networks", "draft/pre-away", "draft/account-registration", NULL,
};

/* The managing connection to a bouncer only needs the network list and its notifications */
static const char *const CONTROL_CAPS[] = {
	"sasl", "batch", "message-tags", "server-time", "cap-notify", "soju.im/bouncer-networks",
	"soju.im/bouncer-networks-notify", "crimson-chat/invites", "crimson-chat/reconnect", NULL,
};

static void connect_network(CrimsonNetwork *network);


static void free_listed_room(ListedRoom *room) {
	g_free(room->name);
	g_free(room->topic);
	g_free(room);
}


/* A WHOIS answer and when it came */
typedef struct {
	Profile *profile;
	gint64 time;
} KeptProfile;


static void free_profile(Profile *profile);


static void free_kept_profile(KeptProfile *kept) {
	free_profile(kept->profile);
	g_free(kept);
}


static void free_profile(Profile *profile) {
	g_free(profile->nick);
	g_free(profile->user);
	g_free(profile->host);
	g_free(profile->realname);
	g_free(profile->server);
	g_free(profile->account);
	g_free(profile->away);
	g_free(profile->channels);
	g_free(profile);
}


static void free_network_work(NetworkWork *work) {
	if (work->listing_ready)
		work->listing_ready(NULL, work->listing_data);
	if (work->profile_ready)
		work->profile_ready(NULL, work->profile_data);
	g_clear_pointer(&work->profile, free_profile);
	g_clear_pointer(&work->listing, g_ptr_array_unref);
	g_clear_handle_id(&work->listing_retry_source, g_source_remove);
	g_hash_table_unref(work->history);
	g_hash_table_unref(work->multiline);
	g_hash_table_unref(work->pending);
	g_hash_table_unref(work->splits);
	g_hash_table_unref(work->refused_targets);
	g_hash_table_unref(work->profiles);
	g_ptr_array_unref(work->questions);
	g_hash_table_unref(work->answer_batches);
	g_free(work->opening_room);
	g_clear_handle_id(&work->typing_source, g_source_remove);
	if (work->registration_reply)
		work->registration_reply(REGISTRATION_FAILED, NULL, work->registration_data);
	g_free(work->registering_account);
	g_free(work->registering_password);
	g_free(work->code_command);
	g_free(work);
}


NetworkWork *get_network_work(CrimsonNetwork *network) {
	NetworkWork *work = g_object_get_data(G_OBJECT(network), WORK_KEY);
	if (work)
		return work;
	work = g_new0(NetworkWork, 1);
	work->history = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_history_load);
	work->multiline = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_irc_message);
	work->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	work->splits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
	work->refused_targets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	work->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_kept_profile);
	work->questions = g_ptr_array_new_with_free_func(free_question);
	work->answer_batches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_object_set_data_full(G_OBJECT(network), WORK_KEY, work, (GDestroyNotify)free_network_work);
	return work;
}


char *fold_network_name(CrimsonNetwork *network, const char *name) {
	IrcState *state = get_network_state(network);
	return state ? fold_state_name(state, name) : g_ascii_strdown(name, -1);
}


void refuse_target(CrimsonNetwork *network, const char *target) {
	NetworkWork *work = get_network_work(network);
	gint64 until = g_get_monotonic_time() + ANSWER_KEEP_SECONDS * G_USEC_PER_SEC;
	g_hash_table_replace(work->refused_targets, fold_network_name(network, target), GSIZE_TO_POINTER((gsize)until));
}


void forgive_target(CrimsonNetwork *network, const char *target) {
	NetworkWork *work = get_network_work(network);
	g_autofree char *folded = fold_network_name(network, target);
	g_hash_table_remove(work->refused_targets, folded);
}


gboolean is_target_refused(CrimsonNetwork *network, const char *target) {
	NetworkWork *work = get_network_work(network);
	g_autofree char *folded = fold_network_name(network, target);
	gint64 until = GPOINTER_TO_SIZE(g_hash_table_lookup(work->refused_targets, folded));
	return until && g_get_monotonic_time() < until;
}


gboolean can_network(CrimsonNetwork *network, const char *cap) {
	IrcState *state = get_network_state(network);
	return state && has_irc_cap(state, cap);
}


Chat *create_chat(const ChatEvents *events, gpointer data) {
	Chat *chat = g_new0(Chat, 1);
	chat->networks = g_list_store_new(CRIMSON_TYPE_NETWORK);
	chat->accounts = g_list_store_new(CRIMSON_TYPE_ACCOUNT);
	chat->sessions = g_ptr_array_new();
	chat->events = *events;
	chat->data = data;
	return chat;
}


static gboolean save_cache_now(gpointer data) {
	Chat *chat = data;
	chat->cache_source = 0;
	save_room_cache(G_LIST_MODEL(chat->networks));
	return G_SOURCE_REMOVE;
}


void save_chat_cache_later(Chat *chat) {
	if (!chat->cache_source)
		chat->cache_source = g_timeout_add_seconds(CACHE_DELAY_SECONDS, save_cache_now, chat);
}


void save_chat_accounts(Chat *chat) {
	g_autoptr(GPtrArray) records = g_ptr_array_new();
	for (guint index = 0; index < chat->sessions->len; index++)
		g_ptr_array_add(records, ((Session *)g_ptr_array_index(chat->sessions, index))->record);
	save_accounts(records);
}


static void fill_settings(IrcClientSettings *settings, const AccountRecord *record) {
	const char *nick = record->nick && *record->nick ? record->nick : g_get_user_name();
	*settings = (IrcClientSettings){
		.host = record->host,
		.port = record->port,
		.uses_tls = record->uses_tls,
		.nick = (char *)nick,
		.username = record->username,
		.realname = record->realname ? record->realname : (char *)nick,
		.password = record->password,
		.sasl_username = record->sasl_username,
		.sasl_password = record->sasl_password,
		.trusted_certificate = record->trusted_certificate,
		.is_bouncer = record->is_bouncer,
	};
}


static NetworkState read_network_state(CrimsonNetwork *network) {
	IrcClientStatus status = network->client ? get_irc_client_status(network->client) : IRC_CLIENT_DISCONNECTED;
	if (status != IRC_CLIENT_REGISTERED)
		return status == IRC_CLIENT_CONNECTING ? NETWORK_CONNECTING : NETWORK_DISCONNECTED;
	/* Through a bouncer the link that matters is the bouncer's own one to the network */
	const char *upstream = g_hash_table_lookup(network->attributes, "state");
	if (!network->bouncer_id || !upstream || g_str_equal(upstream, "connected"))
		return NETWORK_CONNECTED;
	return g_str_equal(upstream, "connecting") ? NETWORK_CONNECTING : NETWORK_DISCONNECTED;
}


/* The server room shows its network's state, so it is drawn again along with the rail and the header */
/* Servers without the IRCv3 commands register through NickServ, which a quiet WHOIS finds once connected */
static void probe_nickserv(CrimsonNetwork *network) {
	if (!network->client || can_network(network, "draft/account-registration"))
		return;
	get_network_work(network)->is_probing_nickserv = TRUE;
	send_irc_client_command(network->client, "WHOIS", NICKSERV, NULL);
}


static void refresh_network_state(CrimsonNetwork *network) {
	NetworkState previous = network->state;
	network->state = read_network_state(network);
	if (network->state == NETWORK_CONNECTED)
		network->has_connected = TRUE;
	if (network->state == NETWORK_CONNECTED && previous != NETWORK_CONNECTED) {
		network->connected_since = g_get_monotonic_time();
		probe_nickserv(network);
	}
	announce_network_change(network);
	announce_account_change(network->session->item);
	announce_room_change(network->server_room);
}


/* A direct server names itself in RPL_ISUPPORT, which is nicer than its host name */
static void adopt_network_name(CrimsonNetwork *network) {
	IrcState *state = get_network_state(network);
	const char *name = state ? get_irc_support(state, "NETWORK") : NULL;
	const char *icon = state ? get_irc_support(state, "draft/ICON") : NULL;
	if (icon && g_strcmp0(icon, network->icon_url) != 0) {
		g_free(network->icon_url);
		network->icon_url = g_strdup(icon);
	}
	/* A name the user gave wins, and only a bare address gives way to the name the network announces */
	const char *given = network->bouncer_id ? g_hash_table_lookup(network->attributes, "name")
		: network->session->record->name;
	const char *host = network->bouncer_id ? g_hash_table_lookup(network->attributes, "host")
		: network->session->record->host;
	if (!name || g_strcmp0(name, network->name) == 0 || (given && g_strcmp0(given, host) != 0))
		return;
	g_free(network->name);
	network->name = g_strdup(name);
	g_free(network->server_room->name);
	network->server_room->name = g_strdup(name);
	announce_network_change(network);
	save_chat_cache_later(network->session->chat);
}


static void join_saved_channels(CrimsonNetwork *network) {
	GStrv channels = network->session->record->channels;
	for (int index = 0; channels && channels[index]; index++)
		send_irc_client_command(network->client, "JOIN", channels[index], NULL);
}


/* Private chats from before survive a reconnect, and MONITOR reports whether their people are online */
static void watch_private_rooms(CrimsonNetwork *network) {
	if (!get_irc_support(get_network_state(network), "MONITOR"))
		return;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (room->kind == ROOM_PRIVATE)
			send_irc_client_command(network->client, "MONITOR", "+", room->name, NULL);
	}
}


/* Servers forget a display name and picture when the connection ends, so each connection sets them again */
static void restore_own_profile(CrimsonNetwork *network) {
	static const char *const KEYS[] = {OWN_PROFILE_KEYS, NULL};
	for (int index = 0; KEYS[index]; index++) {
		g_autofree char *value = load_profile_value(network->key, KEYS[index]);
		if (value && *value)
			send_irc_client_command(network->client, "METADATA", "*", "SET", KEYS[index], value, NULL);
	}
}


static void start_registered_network(CrimsonNetwork *network) {
	adopt_network_name(network);
	if (can_network(network, "draft/metadata-2")) {
		send_irc_client_command(network->client, "METADATA", "*", "SUB", FOLLOWED_PROFILE_KEYS, NULL);
		restore_own_profile(network);
	}
	if (!network->bouncer_id)
		join_saved_channels(network);
	watch_private_rooms(network);
	request_private_targets(network);
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (room->kind == ROOM_PRIVATE)
			request_latest_history(room);
	}
}


/* A failed login or a first connection that fails says so once, until the account connects again */
static void report_failure(Session *session, IrcClient *client, IrcClientStatus status, const char *reason) {
	IrcFailure failure = get_irc_client_failure(client);
	if (status == IRC_CLIENT_REGISTERED) {
		session->has_reported_failure = FALSE;
		session->has_connected = TRUE;
	}
	/* A refused login needs the user, and so does a server never reached, while a dropped connection
	 * reconnects by itself */
	gboolean needs_user = failure == IRC_FAILURE_LOGIN || (failure == IRC_FAILURE_NETWORK && !session->has_connected);
	if (status != IRC_CLIENT_DISCONNECTED || !needs_user || session->has_reported_failure || !reason)
		return;
	session->has_reported_failure = TRUE;
	g_autofree char *text = g_strdup_printf(TEXT_ACCOUNT_PROBLEM, describe_account_name(session), reason);
	session->chat->events.show_message(text, session->chat->data);
}


static void handle_network_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	CrimsonNetwork *network = data;
	if (!network->bouncer_id)
		report_failure(network->session, client, status, reason);
	g_free(network->error);
	network->error = g_strdup(reason);
	if (status == IRC_CLIENT_DISCONNECTED) {
		guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
		for (guint index = 0; index < count; index++) {
			g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
			room->is_joined = FALSE;
			room->history_request = HISTORY_NONE;
			g_hash_table_remove_all(room->typing);
		}
		if (get_irc_client_failure(client) == IRC_FAILURE_CERTIFICATE)
			network->session->chat->events.certificate_refused(network->session, get_refused_certificate(client),
				network->session->chat->data);
	}
	if (status == IRC_CLIENT_REGISTERED)
		start_registered_network(network);
	refresh_network_state(network);
}


static void drop_network(Session *session, CrimsonNetwork *network);
static void note_listing_delay(CrimsonNetwork *network, const IrcMessage *message);


static void drop_gone_network(gpointer data) {
	g_autoptr(CrimsonNetwork) network = data;
	Session *session = network->session;
	if (g_ptr_array_find(session->networks, network, NULL)) {
		drop_network(session, network);
		save_chat_cache_later(session->chat);
	}
}


/* A bouncer refuses a network it no longer has, which only means it was removed elsewhere */
static gboolean is_gone_network(const IrcMessage *message) {
	return g_str_equal(message->command, "FAIL") && g_strcmp0(get_irc_parameter(message, 0), "BOUNCER") == 0
		&& g_strcmp0(get_irc_parameter(message, 1), "INVALID_NETID") == 0;
}


static void handle_network_received(IrcClient *client, const IrcMessage *message, GPtrArray *affected,
	gpointer data) {
	CrimsonNetwork *network = data;
	if (network->bouncer_id && is_gone_network(message)) {
		g_idle_add_once(drop_gone_network, g_object_ref(network));
		return;
	}
	note_listing_delay(network, message);
	handle_network_message(network, message, affected);
	if (g_str_equal(message->command, "005"))
		adopt_network_name(network);
}


static CrimsonNetwork *add_network(Session *session, const char *key, const char *name) {
	CrimsonNetwork *network = create_network(session, key, name);
	load_room_cache(network);
	g_ptr_array_add(session->networks, network);
	/* A server takes its place in the order the user chose, after those placed before it */
	GListModel *networks = G_LIST_MODEL(session->chat->networks);
	int rank = find_network_rank(key);
	guint position = g_list_model_get_n_items(networks);
	for (guint index = 0; index < g_list_model_get_n_items(networks); index++) {
		g_autoptr(CrimsonNetwork) other = g_list_model_get_item(networks, index);
		if (find_network_rank(other->key) > rank) {
			position = index;
			break;
		}
	}
	g_list_store_insert(session->chat->networks, position, network);
	if (session->item)
		announce_account_change(session->item);
	return network;
}


static void connect_network(CrimsonNetwork *network) {
	IrcClientSettings settings;
	fill_settings(&settings, network->session->record);
	settings.bound_network = network->bouncer_id;
	if (!network->client) {
		static const IrcClientEvents EVENTS = {handle_network_received, handle_network_status};
		network->client = create_irc_client(&settings, CLIENT_CAPS, &EVENTS, network);
	} else {
		change_irc_client_settings(network->client, &settings);
	}
	connect_irc_client(network->client);
}


static CrimsonNetwork *find_bouncer_network(Session *session, const char *network_id) {
	for (guint index = 0; index < session->networks->len; index++) {
		CrimsonNetwork *network = g_ptr_array_index(session->networks, index);
		if (g_strcmp0(network->bouncer_id, network_id) == 0)
			return network;
	}
	return NULL;
}


static void drop_network(Session *session, CrimsonNetwork *network) {
	session->chat->events.network_removed(network, session->chat->data);
	if (network->client)
		disconnect_irc_client(network->client, NULL);
	guint position = 0;
	if (g_list_store_find(session->chat->networks, network, &position))
		g_list_store_remove(session->chat->networks, position);
	g_ptr_array_remove(session->networks, network);
	g_object_unref(network);
	announce_account_change(session->item);
}


/* BOUNCER NETWORK adds, changes or with * removes one of the bouncer's networks */
static void update_bouncer_network(Session *session, const IrcMessage *message) {
	const char *network_id = get_irc_parameter(message, 1);
	const char *text = get_irc_parameter(message, 2);
	if (!network_id || !text)
		return;
	CrimsonNetwork *network = find_bouncer_network(session, network_id);
	if (g_str_equal(text, "*")) {
		if (network)
			drop_network(session, network);
		save_chat_cache_later(session->chat);
		return;
	}
	g_autoptr(GHashTable) attributes = parse_irc_attributes(text);
	/* Only a full description adds a network, so a late state change for a removed one cannot bring it back */
	if (!network && !g_hash_table_contains(attributes, "host"))
		return;
	gboolean is_new = !network;
	if (is_new) {
		g_autofree char *key = g_strconcat(session->record->id, "/", network_id, NULL);
		network = add_network(session, key, network_id);
		network->bouncer_id = g_strdup(network_id);
	}
	GHashTableIter iterator;
	gpointer name, value;
	g_hash_table_iter_init(&iterator, attributes);
	while (g_hash_table_iter_next(&iterator, &name, &value)) {
		if (*(const char *)value)
			g_hash_table_insert(network->attributes, g_strdup(name), g_strdup(value));
		else
			g_hash_table_remove(network->attributes, name);
	}
	const char *label = g_hash_table_lookup(network->attributes, "name");
	if (!label)
		label = g_hash_table_lookup(network->attributes, "host");
	if (label && g_strcmp0(label, network->name) != 0) {
		g_free(network->name);
		network->name = g_strdup(label);
		g_free(network->server_room->name);
		network->server_room->name = g_strdup(label);
		save_chat_cache_later(session->chat);
	}
	if (!network->client)
		connect_network(network);
	refresh_network_state(network);
}


/* A bouncer added by its address takes the name its owner gave it */
static void adopt_bouncer_name(Session *session, IrcClient *client) {
	const char *name = get_irc_support(get_irc_client_state(client), "NETWORK");
	const char *given = session->record->name;
	if (!name || g_str_equal(name, BOUNCER_DEFAULT_NAME) || (given && g_strcmp0(given, session->record->host) != 0))
		return;
	g_free(session->record->name);
	session->record->name = g_strdup(name);
	save_chat_accounts(session->chat);
}


/* The full network list the bouncer sends on login is the truth, so networks remembered from before that
 * it no longer has are dropped instead of being retried */
static void follow_network_listing(Session *session, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	if (!reference || strlen(reference) < 2)
		return;
	if (*reference == '+' && g_strcmp0(get_irc_parameter(message, 1), "soju.im/bouncer-networks") == 0) {
		g_free(session->listing_batch);
		session->listing_batch = g_strdup(reference + 1);
		g_clear_pointer(&session->listed_ids, g_hash_table_unref);
		session->listed_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		return;
	}
	if (*reference != '-' || g_strcmp0(reference + 1, session->listing_batch) != 0 || !session->listed_ids)
		return;
	for (guint index = session->networks->len; index-- > 0;) {
		CrimsonNetwork *network = g_ptr_array_index(session->networks, index);
		if (network->bouncer_id && !g_hash_table_contains(session->listed_ids, network->bouncer_id))
			drop_network(session, network);
	}
	g_clear_pointer(&session->listed_ids, g_hash_table_unref);
	g_clear_pointer(&session->listing_batch, g_free);
	save_chat_cache_later(session->chat);
}


static void handle_control_received(IrcClient *client, const IrcMessage *message, GPtrArray *affected,
	gpointer data) {
	Session *session = data;
	const char *subcommand = get_irc_parameter(message, 0);
	/* The name comes with the server's details, which follow the welcome */
	if (g_str_equal(message->command, "005")) {
		adopt_bouncer_name(session, client);
		announce_account_change(session->item);
	} else if (g_str_equal(message->command, "BATCH")) {
		follow_network_listing(session, message);
	} else if (g_str_equal(message->command, "BOUNCER") && g_strcmp0(subcommand, "NETWORK") == 0) {
		const char *batch = get_irc_tag(message, "batch");
		if (session->listed_ids && g_strcmp0(batch, session->listing_batch) == 0 && get_irc_parameter(message, 1))
			g_hash_table_add(session->listed_ids, g_strdup(get_irc_parameter(message, 1)));
		update_bouncer_network(session, message);
	}
	else if (g_str_equal(message->command, "CRIMSON") && g_strcmp0(subcommand, "INVITE") == 0
		&& get_irc_parameter(message, 3))
		session->chat->events.invite_ready(session, get_irc_parameter(message, 1), get_irc_parameter(message, 2),
			get_irc_parameter(message, 3), session->chat->data);
	else if (g_str_equal(message->command, "CRIMSON") && g_strcmp0(subcommand, "ADMIN") == 0) {
		session->is_admin = TRUE;
		g_free(session->web_port);
		session->web_port = g_strdup(get_irc_parameter(message, 1));
		announce_account_change(session->item);
	} else if (g_str_equal(message->command, "FAIL"))
		session->chat->events.show_message(get_irc_parameter(message, count_irc_parameters(message) - 1),
			session->chat->data);
}


static void handle_control_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	Session *session = data;
	g_free(session->control_error);
	session->control_error = g_strdup(reason);
	report_failure(session, client, status, reason);
	announce_account_change(session->item);
	if (status == IRC_CLIENT_REGISTERED && !has_irc_cap(get_irc_client_state(client), "soju.im/bouncer-networks"))
		session->chat->events.show_message(TEXT_NOT_A_BOUNCER, session->chat->data);
	if (status == IRC_CLIENT_REGISTERED && !has_irc_cap(get_irc_client_state(client), "soju.im/bouncer-networks-notify"))
		send_irc_client_command(client, "BOUNCER", "LISTNETWORKS", NULL);
	if (status == IRC_CLIENT_DISCONNECTED && get_irc_client_failure(client) == IRC_FAILURE_CERTIFICATE)
		session->chat->events.certificate_refused(session, get_refused_certificate(client), session->chat->data);
	for (guint index = 0; status != IRC_CLIENT_REGISTERED && index < session->networks->len; index++)
		refresh_network_state(g_ptr_array_index(session->networks, index));
}


void connect_session(Session *session) {
	if (!session->record->is_bouncer) {
		CrimsonNetwork *network = session->networks->len ? g_ptr_array_index(session->networks, 0) : NULL;
		if (!network)
			network = add_network(session, session->record->id, session->record->name ? session->record->name
				: session->record->host);
		connect_network(network);
		return;
	}
	IrcClientSettings settings;
	fill_settings(&settings, session->record);
	static const IrcClientEvents EVENTS = {handle_control_received, handle_control_status};
	if (!session->control)
		session->control = create_irc_client(&settings, CONTROL_CAPS, &EVENTS, session);
	else
		change_irc_client_settings(session->control, &settings);
	connect_irc_client(session->control);
	for (guint index = 0; index < session->networks->len; index++)
		connect_network(g_ptr_array_index(session->networks, index));
}


/* A bouncer's networks from the last run show at once, before the bouncer lists them again */
static void restore_cached_networks(Session *session) {
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GPtrArray) keys = list_cached_networks(session->record->id, &names);
	for (guint index = 0; index < keys->len; index++) {
		const char *key = g_ptr_array_index(keys, index);
		CrimsonNetwork *network = add_network(session, key, g_ptr_array_index(names, index));
		network->bouncer_id = g_strdup(strchr(key, '/') + 1);
	}
}


Session *add_account(Chat *chat, AccountRecord *record) {
	Session *session = g_new0(Session, 1);
	session->chat = chat;
	session->record = record;
	session->networks = g_ptr_array_new();
	session->item = create_account_item(session);
	g_ptr_array_add(chat->sessions, session);
	g_list_store_append(chat->accounts, session->item);
	if (record->is_bouncer)
		restore_cached_networks(session);
	else
		add_network(session, record->id, record->name ? record->name : record->host);
	connect_session(session);
	return session;
}


const char *describe_account_name(Session *session) {
	return session->record->name && *session->record->name ? session->record->name : session->record->host;
}


/* A bouncer is as connected as the link to it, and a direct account as its one network */
NetworkState read_account_state(Session *session) {
	if (!session->record->is_bouncer)
		return session->networks->len ? ((CrimsonNetwork *)g_ptr_array_index(session->networks, 0))->state
			: NETWORK_DISCONNECTED;
	IrcClientStatus status = session->control ? get_irc_client_status(session->control) : IRC_CLIENT_DISCONNECTED;
	return status == IRC_CLIENT_REGISTERED ? NETWORK_CONNECTED : status == IRC_CLIENT_CONNECTING ? NETWORK_CONNECTING
		: NETWORK_DISCONNECTED;
}


const char *read_account_error(Session *session) {
	if (session->record->is_bouncer)
		return session->control_error;
	return session->networks->len ? ((CrimsonNetwork *)g_ptr_array_index(session->networks, 0))->error : NULL;
}


void start_chat(Chat *chat) {
	chat->has_started = TRUE;
	g_autoptr(GPtrArray) records = load_accounts();
	for (guint index = 0; index < records->len; index++)
		add_account(chat, copy_account_record(g_ptr_array_index(records, index)));
}


void stop_chat(Chat *chat) {
	g_clear_handle_id(&chat->cache_source, g_source_remove);
	save_room_cache(G_LIST_MODEL(chat->networks));
	for (guint index = 0; index < chat->sessions->len; index++) {
		Session *session = g_ptr_array_index(chat->sessions, index);
		for (guint network = 0; network < session->networks->len; network++) {
			CrimsonNetwork *item = g_ptr_array_index(session->networks, network);
			if (item->client)
				disconnect_irc_client(item->client, TEXT_QUIT_MESSAGE);
		}
		if (session->control)
			disconnect_irc_client(session->control, NULL);
	}
}


void change_account(Session *session, const AccountRecord *record) {
	AccountRecord *updated = copy_account_record(record);
	g_strfreev(updated->channels);
	updated->channels = g_strdupv(session->record->channels);
	free_account_record(session->record);
	session->record = updated;
	save_chat_accounts(session->chat);
	if (!record->is_bouncer && session->networks->len) {
		CrimsonNetwork *network = g_ptr_array_index(session->networks, 0);
		g_free(network->name);
		network->name = g_strdup(record->name ? record->name : record->host);
		announce_network_change(network);
	}
	announce_account_change(session->item);
	connect_session(session);
}


void remove_account(Session *session) {
	Chat *chat = session->chat;
	while (session->networks->len)
		drop_network(session, g_ptr_array_index(session->networks, 0));
	if (session->control) {
		disconnect_irc_client(session->control, NULL);
		free_irc_client(session->control);
	}
	g_ptr_array_remove(chat->sessions, session);
	guint position = 0;
	if (g_list_store_find(chat->accounts, session->item, &position))
		g_list_store_remove(chat->accounts, position);
	g_object_unref(session->item);
	g_free(session->control_error);
	g_free(session->web_port);
	g_free(session->listing_batch);
	g_clear_pointer(&session->listed_ids, g_hash_table_unref);
	free_account_record(session->record);
	g_ptr_array_unref(session->networks);
	g_free(session);
	save_chat_accounts(chat);
	save_chat_cache_later(chat);
}


void trust_certificate(Session *session, const char *hash) {
	g_free(session->record->trusted_certificate);
	session->record->trusted_certificate = g_strdup(hash);
	save_chat_accounts(session->chat);
	connect_session(session);
}


void add_bouncer_network(Session *session, GHashTable *attributes) {
	g_autofree char *text = format_irc_attributes(attributes);
	if (session->control)
		send_irc_client_command(session->control, "BOUNCER", "ADDNETWORK", text, NULL);
}


void change_bouncer_network(CrimsonNetwork *network, GHashTable *attributes) {
	g_autofree char *text = format_irc_attributes(attributes);
	if (network->session->control)
		send_irc_client_command(network->session->control, "BOUNCER", "CHANGENETWORK", network->bouncer_id, text,
			NULL);
}


void delete_bouncer_network(CrimsonNetwork *network) {
	if (network->session->control)
		send_irc_client_command(network->session->control, "BOUNCER", "DELNETWORK", network->bouncer_id, NULL);
}


void reconnect_network(CrimsonNetwork *network) {
	IrcClient *control = network->session->control;
	if (network->session->record->is_bouncer && control
		&& get_irc_client_status(control) == IRC_CLIENT_DISCONNECTED)
		connect_session(network->session);
	/* Crimson Chat's own bouncer reconnects to the server itself, where other bouncers only reconnect the app */
	else if (network->bouncer_id && control && has_irc_cap(get_irc_client_state(control), "crimson-chat/reconnect"))
		send_irc_client_command(control, "BOUNCER", "RECONNECTNETWORK", network->bouncer_id, NULL);
	else
		connect_network(network);
}


Session *find_bouncer(Chat *chat) {
	for (guint index = 0; index < chat->sessions->len; index++) {
		Session *session = g_ptr_array_index(chat->sessions, index);
		if (session->record->is_bouncer)
			return session;
	}
	return NULL;
}


void cancel_room_listing(CrimsonNetwork *network) {
	NetworkWork *work = get_network_work(network);
	g_clear_handle_id(&work->listing_retry_source, g_source_remove);
	work->listing_wait = NULL;
	work->listing_ready = NULL;
	work->listing_data = NULL;
	g_clear_pointer(&work->listing, g_ptr_array_unref);
}


static void send_room_listing(CrimsonNetwork *network) {
	NetworkWork *work = get_network_work(network);
	g_clear_pointer(&work->listing, g_ptr_array_unref);
	work->listing = g_ptr_array_new_with_free_func((GDestroyNotify)free_listed_room);
	work->listing_delay = 0;
	IrcState *state = get_network_state(network);
	const char *filters = state ? get_irc_support(state, "ELIST") : NULL;
	/* Big networks list tens of thousands of channels, so a server that filters by size first sends busy ones */
	work->is_listing_busy = filters && strpbrk(filters, "Uu");
	if (work->is_listing_busy)
		send_irc_client_command(network->client, "LIST", BUSY_ROOM_FILTER, NULL);
	else
		send_irc_client_command(network->client, "LIST", NULL);
}


static gboolean retry_room_listing(gpointer data) {
	CrimsonNetwork *network = data;
	get_network_work(network)->listing_retry_source = 0;
	send_room_listing(network);
	return G_SOURCE_REMOVE;
}


void request_room_listing(CrimsonNetwork *network, RoomListingReady ready, RoomListingWait wait, gpointer data) {
	cancel_room_listing(network);
	NetworkWork *work = get_network_work(network);
	work->listing_ready = ready;
	work->listing_wait = wait;
	work->listing_data = data;
	send_room_listing(network);
}


/* Some servers refuse to list rooms for a while after connecting, which they say in a notice with the seconds left */
static void note_listing_delay(CrimsonNetwork *network, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	if (!work->listing || !g_str_equal(message->command, "NOTICE") || (message->source && strchr(message->source, '!')))
		return;
	g_autoptr(GRegex) delay = g_regex_new("try again in (\\d+)", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	if (!g_regex_match(delay, get_irc_parameter(message, count_irc_parameters(message) - 1), 0, &match))
		return;
	g_autofree char *seconds = g_match_info_fetch(match, 1);
	work->listing_delay = (guint)g_ascii_strtoull(seconds, NULL, 10);
}


static int order_listed_rooms(gconstpointer first, gconstpointer second) {
	const ListedRoom *one = *(ListedRoom *const *)first, *other = *(ListedRoom *const *)second;
	if (one->users != other->users)
		return one->users > other->users ? -1 : 1;
	return g_ascii_strcasecmp(one->name, other->name);
}


gboolean collect_room_listing(CrimsonNetwork *network, int number, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	if (number != 321 && number != 322 && number != 323)
		return FALSE;
	if (!work->listing)
		return TRUE;
	if (number == 322 && get_irc_parameter(message, 1)) {
		ListedRoom *room = g_new0(ListedRoom, 1);
		room->name = g_strdup(get_irc_parameter(message, 1));
		room->users = (guint)g_ascii_strtoull(get_irc_parameter(message, 2) ? get_irc_parameter(message, 2) : "0",
			NULL, 10);
		/* Some servers put the room's modes in front of its topic, such as "[+nt]", which the picker leaves out */
		const char *topic = get_irc_parameter(message, 3);
		const char *modes_end = topic && g_str_has_prefix(topic, "[+") ? strstr(topic, "] ") : NULL;
		room->topic = strip_irc_formatting(modes_end ? modes_end + 2 : topic);
		g_ptr_array_add(work->listing, room);
	} else if (number == 323 && !work->listing->len && work->listing_delay) {
		/* The list waits for the time the server named, and the picker counts it down meanwhile */
		guint delay = work->listing_delay;
		g_clear_pointer(&work->listing, g_ptr_array_unref);
		work->listing = g_ptr_array_new_with_free_func((GDestroyNotify)free_listed_room);
		work->listing_retry_source = g_timeout_add_seconds(delay + 1, retry_room_listing, network);
		if (work->listing_wait)
			work->listing_wait(delay + 1, work->listing_data);
	} else if (number == 323 && work->is_listing_busy && work->listing->len < FEW_BUSY_ROOMS) {
		work->is_listing_busy = FALSE;
		g_ptr_array_set_size(work->listing, 0);
		send_irc_client_command(network->client, "LIST", NULL);
	} else if (number == 323) {
		g_autoptr(GPtrArray) rooms = g_steal_pointer(&work->listing);
		g_ptr_array_sort(rooms, order_listed_rooms);
		RoomListingReady ready = work->listing_ready;
		gpointer data = work->listing_data;
		work->listing_ready = NULL;
		work->listing_data = NULL;
		if (ready)
			ready(rooms, data);
	}
	return TRUE;
}


void cancel_profile(CrimsonNetwork *network, gpointer data) {
	NetworkWork *work = get_network_work(network);
	if (work->profile_data != data)
		return;
	work->profile_ready = NULL;
	work->profile_data = NULL;
	g_clear_pointer(&work->profile, free_profile);
}


void request_profile(CrimsonNetwork *network, const char *nick, ProfileReady ready, gpointer data) {
	NetworkWork *work = get_network_work(network);
	cancel_profile(network, work->profile_data);
	g_autofree char *folded = fold_network_name(network, nick);
	KeptProfile *kept = g_hash_table_lookup(work->profiles, folded);
	if (kept && g_get_monotonic_time() - kept->time < ANSWER_KEEP_SECONDS * G_USEC_PER_SEC) {
		ready(kept->profile, data);
		return;
	}
	work->profile = g_new0(Profile, 1);
	work->profile->nick = g_strdup(nick);
	work->profile->is_found = TRUE;
	work->profile_ready = ready;
	work->profile_data = data;
	/* Asking with the nick twice reaches the person's own server, which knows how long they have been idle */
	send_irc_client_command(network->client, "WHOIS", nick, nick, NULL);
}


static void set_profile_text(char **field, const char *text) {
	g_free(*field);
	*field = text && *text ? g_strdup(text) : NULL;
}


/* Hands the finished profile to whoever asked and keeps it for a while, so opening it again asks nothing */
static void finish_profile(CrimsonNetwork *network, Profile *profile) {
	NetworkWork *work = get_network_work(network);
	ProfileReady ready = work->profile_ready;
	gpointer data = work->profile_data;
	work->profile = NULL;
	work->profile_ready = NULL;
	work->profile_data = NULL;
	if (ready)
		ready(profile, data);
	KeptProfile *kept = g_new0(KeptProfile, 1);
	*kept = (KeptProfile){profile, g_get_monotonic_time()};
	g_hash_table_replace(work->profiles, fold_network_name(network, profile->nick), kept);
}


gboolean collect_profile(CrimsonNetwork *network, int number, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	Profile *profile = work->profile;
	const char *nick = get_irc_parameter(message, 1);
	if (!profile || !nick || g_ascii_strcasecmp(nick, profile->nick) != 0)
		return FALSE;
	const char *last = get_irc_parameter(message, count_irc_parameters(message) - 1);
	switch (number) {
	case 311:
		set_profile_text(&profile->nick, nick);
		set_profile_text(&profile->user, get_irc_parameter(message, 2));
		set_profile_text(&profile->host, get_irc_parameter(message, 3));
		set_profile_text(&profile->realname, last);
		break;
	case 312:
		set_profile_text(&profile->server, get_irc_parameter(message, 2));
		break;
	case 317:
		profile->idle_seconds = g_ascii_strtoll(get_irc_parameter(message, 2), NULL, 10);
		break;
	case 319:
		set_profile_text(&profile->channels, last);
		break;
	case 330:
		set_profile_text(&profile->account, get_irc_parameter(message, 2));
		break;
	case 301:
		set_profile_text(&profile->away, last);
		break;
	case 671:
		profile->is_secure = TRUE;
		break;
	case 401:
		profile->is_found = FALSE;
		break;
	/* The nick asked a second time names the person's server, which no server knows while they are away, and that
	 * answer ends the question without a closing reply */
	case 402:
		profile->is_found = FALSE;
		finish_profile(network, profile);
		break;
	case 318:
		finish_profile(network, profile);
		break;
	default:
		break;
	}
	return TRUE;
}


/* Through a bouncer that is linked, what went wrong is the bouncer's own link to the network */
static gboolean is_bouncer_link_up(CrimsonNetwork *network) {
	return network->bouncer_id && network->client && get_irc_client_status(network->client) == IRC_CLIENT_REGISTERED;
}


const char *read_network_error(CrimsonNetwork *network) {
	if (is_bouncer_link_up(network))
		return g_hash_table_lookup(network->attributes, "error");
	return network->error;
}


gboolean is_network_retrying(CrimsonNetwork *network) {
	if (network->state == NETWORK_CONNECTING)
		return TRUE;
	if (is_bouncer_link_up(network))
		return g_strcmp0(g_hash_table_lookup(network->attributes, RETRY_ATTRIBUTE), "1") == 0;
	return network->client && will_irc_client_retry(network->client);
}


gboolean is_logged_in(CrimsonNetwork *network) {
	IrcState *state = get_network_state(network);
	return state && state->account && *state->account;
}


gboolean can_register_account(CrimsonNetwork *network) {
	return can_network(network, "draft/account-registration") || network->has_nickserv;
}


/* The keys the server gives with the registration capability, such as email-required, where it gives them */
static gboolean has_registration_key(CrimsonNetwork *network, const char *key) {
	IrcState *state = get_network_state(network);
	const char *value = state ? g_hash_table_lookup(state->available_caps, "draft/account-registration") : NULL;
	if (!value)
		return FALSE;
	g_auto(GStrv) keys = g_strsplit(value, ",", -1);
	for (int index = 0; keys[index]; index++) {
		g_autofree char *name = g_strndup(keys[index], strcspn(keys[index], "="));
		if (g_str_equal(name, key))
			return TRUE;
	}
	return FALSE;
}


gboolean is_email_required(CrimsonNetwork *network) {
	return has_registration_key(network, "email-required");
}


gboolean allows_custom_account_name(CrimsonNetwork *network) {
	return can_network(network, "draft/account-registration") && has_registration_key(network,
		"custom-account-name");
}


void request_registration(CrimsonNetwork *network, const char *account, const char *email, const char *password,
	RegistrationReply reply, gpointer data) {
	NetworkWork *work = get_network_work(network);
	cancel_registration(network);
	work->registration_reply = reply;
	work->registration_data = data;
	work->registering_account = g_strdup(account);
	work->registering_password = g_strdup(password);
	work->is_nickserv_registration = !can_network(network, "draft/account-registration");
	if (!work->is_nickserv_registration) {
		send_irc_client_command(network->client, "REGISTER", account, email && *email ? email : "*", password,
			NULL);
		return;
	}
	g_autofree char *command = email && *email ? g_strdup_printf("REGISTER %s %s", password, email)
		: g_strdup_printf("REGISTER %s", password);
	send_irc_client_command(network->client, "PRIVMSG", NICKSERV, command, NULL);
}


void send_registration_code(CrimsonNetwork *network, const char *code) {
	NetworkWork *work = get_network_work(network);
	if (!work->is_nickserv_registration) {
		send_irc_client_command(network->client, "VERIFY", work->registering_account, code, NULL);
		return;
	}
	g_autofree char *command = g_strdup_printf("%s %s", work->code_command ? work->code_command : "CONFIRM", code);
	send_irc_client_command(network->client, "PRIVMSG", NICKSERV, command, NULL);
}


void cancel_registration(CrimsonNetwork *network) {
	NetworkWork *work = get_network_work(network);
	work->registration_reply = NULL;
	work->registration_data = NULL;
	g_clear_pointer(&work->registering_account, g_free);
	g_clear_pointer(&work->registering_password, g_free);
	g_clear_pointer(&work->code_command, g_free);
}


/* A finished registration becomes the saved login, kept by the bouncer for its networks, which then logs in by
 * itself, and in the account otherwise */
static void keep_registered_login(CrimsonNetwork *network, const char *account) {
	NetworkWork *work = get_network_work(network);
	const char *name = account ? account : work->registering_account;
	if (network->bouncer_id) {
		/* The bouncer saves a login registered with the IRCv3 commands itself, and NickServ's it learns here */
		if (work->is_nickserv_registration) {
			g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			g_hash_table_insert(attributes, g_strdup("sasl-username"), g_strdup(name));
			g_hash_table_insert(attributes, g_strdup("sasl-password"), g_strdup(work->registering_password));
			change_bouncer_network(network, attributes);
		}
	} else {
		AccountRecord *record = network->session->record;
		g_free(record->sasl_username);
		g_free(record->sasl_password);
		record->sasl_username = g_strdup(name);
		record->sasl_password = g_strdup(work->registering_password);
		save_chat_accounts(network->session->chat);
	}
	network->wants_login = FALSE;
}


static void answer_registration(CrimsonNetwork *network, RegistrationResult result, const char *message,
	const char *account) {
	NetworkWork *work = get_network_work(network);
	RegistrationReply reply = work->registration_reply;
	gpointer data = work->registration_data;
	if (result == REGISTRATION_DONE) {
		keep_registered_login(network, account);
		cancel_registration(network);
	}
	if (reply)
		reply(result, message, data);
	announce_network_change(network);
}


/* NickServ asks for a code with its own command, such as VERIFY REGISTER on Atheme or CONFIRM on Anope */
static void read_nickserv_reply(CrimsonNetwork *network, const char *text) {
	NetworkWork *work = get_network_work(network);
	g_autofree char *lower = g_ascii_strdown(text, -1);
	const char *verify = strstr(text, "VERIFY REGISTER");
	if (verify) {
		g_free(work->code_command);
		g_autofree char *rest = g_strdup(verify);
		/* The command names the account before the code, which is the part the user types */
		g_auto(GStrv) words = g_strsplit(rest, " ", 4);
		work->code_command = words[0] && words[1] && words[2] ? g_strdup_printf("%s %s %s", words[0], words[1],
			words[2]) : g_strdup("VERIFY REGISTER");
		answer_registration(network, REGISTRATION_NEEDS_CODE, text, NULL);
	} else if (strstr(lower, "confirm") && (strstr(lower, "code") || strstr(lower, "passcode"))) {
		g_free(work->code_command);
		work->code_command = g_strdup("CONFIRM");
		answer_registration(network, REGISTRATION_NEEDS_CODE, text, NULL);
	} else if (strstr(lower, "already registered")) {
		network->wants_login = TRUE;
		answer_registration(network, REGISTRATION_EXISTS, text, NULL);
	} else if (work->registration_reply) {
		/* Anything else NickServ says shows as it is */
		work->registration_reply(REGISTRATION_FAILED, text, work->registration_data);
	}
}


static gboolean collect_nickserv_probe(CrimsonNetwork *network, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	const char *nick = get_irc_parameter(message, 1);
	if (!work->is_probing_nickserv || !nick || g_ascii_strcasecmp(nick, NICKSERV) != 0)
		return FALSE;
	const char *command = message->command;
	if (g_str_equal(command, "311")) {
		network->has_nickserv = TRUE;
		announce_network_change(network);
	}
	if (g_str_equal(command, "318") || g_str_equal(command, "401"))
		work->is_probing_nickserv = FALSE;
	return TRUE;
}


/* A server can refuse the nickname for belonging to an account, as Ergo does, which is the same as NickServ asking
 * for a login */
static void notice_reserved_nick(CrimsonNetwork *network, const IrcMessage *message) {
	if (!g_str_equal(message->command, "433"))
		return;
	g_autofree char *reason = g_ascii_strdown(get_last_irc_parameter(message), -1);
	if (!strstr(reason, "reserved") && !strstr(reason, "registered"))
		return;
	network->wants_login = TRUE;
	announce_network_change(network);
}


gboolean collect_registration(CrimsonNetwork *network, const IrcMessage *message) {
	notice_reserved_nick(network, message);
	if (collect_nickserv_probe(network, message))
		return TRUE;
	NetworkWork *work = get_network_work(network);
	const char *command = message->command;
	const char *status = get_irc_parameter(message, 0);
	gboolean is_reply = g_str_equal(command, "REGISTER") || g_str_equal(command, "VERIFY");
	gboolean is_failure = g_str_equal(command, "FAIL") && (g_strcmp0(status, "REGISTER") == 0
		|| g_strcmp0(status, "VERIFY") == 0);
	if (is_reply && g_strcmp0(status, "SUCCESS") == 0) {
		answer_registration(network, REGISTRATION_DONE, get_last_irc_parameter(message), get_irc_parameter(message, 1));
		return TRUE;
	}
	if (is_reply && g_strcmp0(status, "VERIFICATION_REQUIRED") == 0) {
		answer_registration(network, REGISTRATION_NEEDS_CODE, get_last_irc_parameter(message), NULL);
		return TRUE;
	}
	if (is_failure) {
		gboolean exists = g_strcmp0(get_irc_parameter(message, 1), "ACCOUNT_EXISTS") == 0;
		if (exists)
			network->wants_login = TRUE;
		answer_registration(network, exists ? REGISTRATION_EXISTS : REGISTRATION_FAILED,
			get_last_irc_parameter(message), NULL);
		return TRUE;
	}
	if (!work->registration_reply || !work->is_nickserv_registration)
		return FALSE;
	/* NickServ logs the new account in once it is registered, which the server confirms with 900 */
	if (g_str_equal(command, "900") && !work->code_command) {
		answer_registration(network, REGISTRATION_DONE, NULL, get_irc_parameter(message, 2));
		return FALSE;
	}
	g_autofree char *sender = get_source_nick(message->source);
	if (g_str_equal(command, "NOTICE") && sender && g_ascii_strcasecmp(sender, NICKSERV) == 0)
		read_nickserv_reply(network, get_last_irc_parameter(message));
	return FALSE;
}


gboolean is_trusted_certificate(Chat *chat, GTlsCertificate *certificate) {
	g_autofree char *hash = hash_certificate(certificate);
	for (guint index = 0; hash && index < chat->sessions->len; index++) {
		Session *session = g_ptr_array_index(chat->sessions, index);
		if (g_strcmp0(session->record->trusted_certificate, hash) == 0)
			return TRUE;
	}
	return FALSE;
}
