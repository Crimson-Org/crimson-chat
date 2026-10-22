#include <string.h>

#include "../lang/en.h"
#include "session.h"

#define CACHE_DELAY_SECONDS 2
#define WORK_KEY "work"

/* Every capability Crimson Chat understands, which servers grant where they support it */
static const char *const CLIENT_CAPS[] = {
	"message-tags", "server-time", "batch", "echo-message", "sasl", "cap-notify", "account-notify", "away-notify",
	"extended-join", "multi-prefix", "userhost-in-names", "chghost", "setname", "invite-notify", "account-tag",
	"labeled-response", "draft/chathistory", "draft/event-playback", "draft/read-marker", "draft/message-redaction",
	"draft/metadata-2", "draft/channel-rename", "draft/multiline", "extended-monitor", "sts",
	"soju.im/bouncer-networks", "draft/pre-away", NULL,
};

/* The managing connection to a bouncer only needs the network list and its notifications */
static const char *const CONTROL_CAPS[] = {
	"sasl", "batch", "message-tags", "server-time", "cap-notify", "soju.im/bouncer-networks",
	"soju.im/bouncer-networks-notify", "crimson-chat/invites", NULL,
};

static void connect_network(CrimsonNetwork *network);


static void free_network_work(NetworkWork *work) {
	g_hash_table_unref(work->history);
	g_hash_table_unref(work->multiline);
	g_hash_table_unref(work->pending);
	g_free(work->opening_room);
	g_clear_handle_id(&work->typing_source, g_source_remove);
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
	g_object_set_data_full(G_OBJECT(network), WORK_KEY, work, (GDestroyNotify)free_network_work);
	return work;
}


gboolean can_network(CrimsonNetwork *network, const char *cap) {
	IrcState *state = get_network_state(network);
	return state && has_irc_cap(state, cap);
}


Chat *create_chat(const ChatEvents *events, gpointer data) {
	Chat *chat = g_new0(Chat, 1);
	chat->networks = g_list_store_new(CRIMSON_TYPE_NETWORK);
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


static void refresh_network_state(CrimsonNetwork *network) {
	network->state = read_network_state(network);
	announce_network_change(network);
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
	if (network->bouncer_id || !name || network->session->record->name)
		return;
	g_free(network->name);
	network->name = g_strdup(name);
	g_free(network->server_room->name);
	network->server_room->name = g_strdup(name);
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


static void start_registered_network(CrimsonNetwork *network) {
	adopt_network_name(network);
	if (can_network(network, "draft/metadata-2"))
		send_irc_client_command(network->client, "METADATA", "*", "SUB", "avatar", "display-name", "color", NULL);
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


static void handle_network_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	CrimsonNetwork *network = data;
	g_free(network->error);
	network->error = g_strdup(reason);
	if (status == IRC_CLIENT_DISCONNECTED) {
		guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
		for (guint index = 0; index < count; index++) {
			g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
			room->is_joined = FALSE;
			room->is_loading_history = FALSE;
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


static void handle_network_received(IrcClient *client, const IrcMessage *message, GPtrArray *affected,
	gpointer data) {
	CrimsonNetwork *network = data;
	handle_network_message(network, message, affected);
	if (g_str_equal(message->command, "005"))
		adopt_network_name(network);
}


static CrimsonNetwork *add_network(Session *session, const char *key, const char *name) {
	CrimsonNetwork *network = create_network(session, key, name);
	load_room_cache(network);
	g_ptr_array_add(session->networks, network);
	g_list_store_append(session->chat->networks, network);
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
	if (network->client)
		disconnect_irc_client(network->client, NULL);
	guint position = 0;
	if (g_list_store_find(session->chat->networks, network, &position))
		g_list_store_remove(session->chat->networks, position);
	g_ptr_array_remove(session->networks, network);
	g_object_unref(network);
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


static void handle_control_received(IrcClient *client, const IrcMessage *message, GPtrArray *affected,
	gpointer data) {
	Session *session = data;
	const char *subcommand = get_irc_parameter(message, 0);
	if (g_str_equal(message->command, "BOUNCER") && g_strcmp0(subcommand, "NETWORK") == 0)
		update_bouncer_network(session, message);
	else if (g_str_equal(message->command, "CRIMSON") && g_strcmp0(subcommand, "INVITE") == 0
		&& get_irc_parameter(message, 3))
		session->chat->events.invite_ready(session, get_irc_parameter(message, 1), get_irc_parameter(message, 2),
			get_irc_parameter(message, 3), session->chat->data);
	else if (g_str_equal(message->command, "FAIL"))
		session->chat->events.show_problem(get_irc_parameter(message, count_irc_parameters(message) - 1),
			session->chat->data);
}


static void handle_control_status(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data) {
	Session *session = data;
	if (status == IRC_CLIENT_REGISTERED && !has_irc_cap(get_irc_client_state(client), "soju.im/bouncer-networks"))
		session->chat->events.show_problem(TEXT_NOT_A_BOUNCER, session->chat->data);
	if (status == IRC_CLIENT_REGISTERED && !has_irc_cap(get_irc_client_state(client), "soju.im/bouncer-networks-notify"))
		send_irc_client_command(client, "BOUNCER", "LISTNETWORKS", NULL);
	if (status == IRC_CLIENT_DISCONNECTED && get_irc_client_failure(client) == IRC_FAILURE_CERTIFICATE)
		session->chat->events.certificate_refused(session, get_refused_certificate(client), session->chat->data);
	for (guint index = 0; status != IRC_CLIENT_REGISTERED && index < session->networks->len; index++)
		refresh_network_state(g_ptr_array_index(session->networks, index));
}


static void connect_session(Session *session) {
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
	g_ptr_array_add(chat->sessions, session);
	if (record->is_bouncer)
		restore_cached_networks(session);
	else
		add_network(session, record->id, record->name ? record->name : record->host);
	connect_session(session);
	return session;
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
	if (network->session->record->is_bouncer && network->session->control
		&& get_irc_client_status(network->session->control) == IRC_CLIENT_DISCONNECTED)
		connect_session(network->session);
	else
		connect_network(network);
}
