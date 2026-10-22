#include <string.h>

#include "bouncer.h"
#include "setup.h"


static void free_bouncer_user(BouncerUser *user) {
	g_ptr_array_unref(user->upstreams);
	g_free(user->name);
	g_free(user->password_hash);
	g_free(user);
}


static void load_user(Bouncer *bouncer, const UserRecord *record) {
	BouncerUser *user = g_new0(BouncerUser, 1);
	user->bouncer = bouncer;
	user->id = record->id;
	user->name = g_strdup(record->name);
	user->password_hash = g_strdup(record->password_hash);
	user->is_admin = record->is_admin;
	user->upstreams = g_ptr_array_new_with_free_func((GDestroyNotify)free_upstream);
	g_ptr_array_add(bouncer->users, user);
	g_autoptr(GPtrArray) networks = load_networks(bouncer->database, user->id);
	for (guint index = 0; index < networks->len; index++)
		g_ptr_array_add(user->upstreams, create_upstream(user, g_ptr_array_index(networks, index)));
}


Bouncer *create_bouncer(Database *database, GTlsCertificate *certificate, const char *name) {
	Bouncer *bouncer = g_new0(Bouncer, 1);
	bouncer->database = database;
	bouncer->certificate = g_object_ref(certificate);
	bouncer->name = g_strdup(name);
	g_autofree char *hash = hash_certificate(certificate);
	bouncer->fingerprint = format_certificate_hash(hash);
	bouncer->web_sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	bouncer->users = g_ptr_array_new_with_free_func((GDestroyNotify)free_bouncer_user);
	g_autoptr(GPtrArray) users = load_users(database);
	for (guint index = 0; index < users->len; index++)
		load_user(bouncer, g_ptr_array_index(users, index));
	return bouncer;
}


#define SNIFF_BYTES 4096

/* A browser and an IRC client reach the same port, and the first words of the first line tell them apart */
static gboolean is_web_request(GBytes *bytes) {
	static const char *const METHODS[] = {"GET ", "POST ", "HEAD ", NULL};
	gsize length = 0;
	const char *data = g_bytes_get_data(bytes, &length);
	for (int index = 0; METHODS[index]; index++) {
		gsize size = strlen(METHODS[index]);
		if (length >= size && memcmp(data, METHODS[index], size) == 0)
			return TRUE;
	}
	return FALSE;
}


typedef struct {
	Bouncer *bouncer;
	GIOStream *connection;
	char *address;
} Arrival;


static void finish_first_read(GObject *source, GAsyncResult *result, gpointer data) {
	Arrival *arrival = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
	if (bytes && g_bytes_get_size(bytes)) {
		if (is_web_request(bytes))
			serve_web(arrival->bouncer, arrival->connection, bytes);
		else
			accept_downstream(arrival->bouncer, arrival->connection, arrival->address, bytes);
	} else {
		g_io_stream_close_async(arrival->connection, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	}
	g_object_unref(arrival->connection);
	g_free(arrival->address);
	g_free(arrival);
}


static gboolean take_connection(GSocketService *service, GSocketConnection *connection, GObject *source,
	gpointer data) {
	Bouncer *bouncer = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress) remote = g_socket_connection_get_remote_address(connection, NULL);
	g_autofree char *address = remote ? g_socket_connectable_to_string(G_SOCKET_CONNECTABLE(remote))
		: g_strdup("unknown");
	GIOStream *secured = g_tls_server_connection_new(G_IO_STREAM(connection), bouncer->certificate, &error);
	if (!secured) {
		g_warning("TLS failed for %s: %s", address, error->message);
		return TRUE;
	}
	Arrival *arrival = g_new0(Arrival, 1);
	arrival->bouncer = bouncer;
	arrival->connection = secured;
	arrival->address = g_steal_pointer(&address);
	g_input_stream_read_bytes_async(g_io_stream_get_input_stream(secured), SNIFF_BYTES, G_PRIORITY_DEFAULT, NULL,
		finish_first_read, arrival);
	return TRUE;
}


/* Takes a port alone for every interface, or an address and port such as 127.0.0.1:6697 or [::1]:6697 */
static gboolean add_listen_address(GSocketService *service, const char *text, GError **error) {
	const char *colon = strrchr(text, ':');
	if (!colon)
		return g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), (guint16)g_ascii_strtoull(text, NULL,
			10), NULL, error);
	g_autofree char *host = g_strndup(text, (gsize)(colon - text));
	if (*host == '[' && host[strlen(host) - 1] == ']') {
		memmove(host, host + 1, strlen(host));
		host[strlen(host) - 1] = '\0';
	}
	g_autoptr(GInetAddress) inet = g_inet_address_new_from_string(host);
	if (!inet) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s is not an IP address", host);
		return FALSE;
	}
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new(inet, (guint16)g_ascii_strtoull(colon + 1,
		NULL, 10));
	return g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_TCP, NULL, NULL, error);
}


gboolean listen_bouncer(Bouncer *bouncer, const char *address, GError **error) {
	bouncer->service = g_socket_service_new();
	if (!add_listen_address(bouncer->service, address, error))
		return FALSE;
	g_signal_connect(bouncer->service, "incoming", G_CALLBACK(take_connection), bouncer);
	g_socket_service_start(bouncer->service);
	for (guint index = 0; index < bouncer->users->len; index++) {
		BouncerUser *user = g_ptr_array_index(bouncer->users, index);
		for (guint network = 0; network < user->upstreams->len; network++)
			restart_upstream(g_ptr_array_index(user->upstreams, network));
	}
	return TRUE;
}


BouncerUser *log_in_user(Bouncer *bouncer, const char *name, const char *password) {
	for (guint index = 0; index < bouncer->users->len; index++) {
		BouncerUser *user = g_ptr_array_index(bouncer->users, index);
		if (g_strcmp0(user->name, name) == 0)
			return check_password(password, user->password_hash) ? user : NULL;
	}
	/* Hashing anyway keeps an unknown name from answering faster than a wrong password */
	static char *stand_in = NULL;
	if (!stand_in)
		stand_in = hash_password(BOUNCER_NAME);
	check_password(password, stand_in);
	return NULL;
}


gboolean is_valid_user_name(const char *name) {
	gsize length = name ? strlen(name) : 0;
	if (!length || length > 32)
		return FALSE;
	for (const char *character = name; *character; character++) {
		if (!g_ascii_isalnum(*character) && !strchr("_-.", *character))
			return FALSE;
	}
	return TRUE;
}


BouncerUser *find_user(Bouncer *bouncer, gint64 id) {
	for (guint index = 0; index < bouncer->users->len; index++) {
		BouncerUser *user = g_ptr_array_index(bouncer->users, index);
		if (user->id == id)
			return user;
	}
	return NULL;
}


BouncerUser *register_user(Bouncer *bouncer, const char *name, const char *password) {
	if (!is_valid_user_name(name))
		return NULL;
	for (guint index = 0; index < bouncer->users->len; index++) {
		if (g_ascii_strcasecmp(((BouncerUser *)g_ptr_array_index(bouncer->users, index))->name, name) == 0)
			return NULL;
	}
	g_autofree char *hash = hash_password(password);
	UserRecord record = {.name = (char *)name, .password_hash = hash};
	record.id = hash ? add_user(bouncer->database, name, hash, FALSE) : 0;
	if (record.id <= 0)
		return NULL;
	load_user(bouncer, &record);
	g_message("%s registered", name);
	return g_ptr_array_index(bouncer->users, bouncer->users->len - 1);
}


char *create_invite(Bouncer *bouncer, BouncerUser *creator, int uses) {
	g_autofree char *first = create_irc_identifier(), *second = create_irc_identifier();
	char *token = g_strconcat(first, second, NULL);
	add_invite(bouncer->database, token, uses, creator->id);
	return token;
}


Upstream *find_upstream(BouncerUser *user, const char *network_id) {
	if (!network_id)
		return NULL;
	gint64 id = g_ascii_strtoll(network_id, NULL, 10);
	for (guint index = 0; index < user->upstreams->len; index++) {
		Upstream *upstream = g_ptr_array_index(user->upstreams, index);
		if (upstream->record.id == id)
			return upstream;
	}
	return NULL;
}


void add_upstream(BouncerUser *user, const NetworkRecord *record) {
	Upstream *upstream = create_upstream(user, record);
	g_ptr_array_add(user->upstreams, upstream);
	announce_network(upstream, describe_upstream(upstream));
	restart_upstream(upstream);
}


void remove_upstream(Upstream *upstream) {
	BouncerUser *user = upstream->user;
	Bouncer *bouncer = user->bouncer;
	announce_network(upstream, NULL);
	for (GList *item = bouncer->downstreams; item;) {
		Downstream *downstream = item->data;
		item = item->next;
		if (downstream->upstream == upstream)
			close_downstream(downstream);
	}
	delete_network(bouncer->database, upstream->record.id);
	g_ptr_array_remove(user->upstreams, upstream);
}


void announce_network(Upstream *upstream, GHashTable *attributes) {
	g_autoptr(GHashTable) owned = attributes;
	g_autofree char *network_id = g_strdup_printf("%" G_GINT64_FORMAT, upstream->record.id);
	g_autofree char *text = attributes ? format_irc_attributes(attributes) : g_strdup("*");
	g_autoptr(IrcMessage) message = create_irc_message("BOUNCER", "NETWORK", network_id, text, NULL);
	message->source = g_strdup(BOUNCER_NAME);
	for (GList *item = upstream->user->bouncer->downstreams; item; item = item->next) {
		Downstream *downstream = item->data;
		if (downstream->user == upstream->user && downstream->is_registered
			&& has_downstream_cap(downstream, "soju.im/bouncer-networks-notify"))
			send_to_downstream(downstream, message);
	}
}


void forget_downstream(Bouncer *bouncer, Downstream *downstream) {
	bouncer->downstreams = g_list_remove(bouncer->downstreams, downstream);
	for (guint index = 0; index < bouncer->users->len; index++) {
		BouncerUser *user = g_ptr_array_index(bouncer->users, index);
		for (guint network = 0; network < user->upstreams->len; network++) {
			Upstream *upstream = g_ptr_array_index(user->upstreams, network);
			for (GList *item = upstream->pending_echoes.head; item; item = item->next) {
				PendingEcho *echo = item->data;
				/* An echo awaited for a client that left goes to everyone */
				if (echo->origin == downstream)
					echo->origin = NULL;
			}
		}
	}
}
