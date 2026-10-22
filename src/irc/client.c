#include <stdarg.h>
#include <string.h>

#include "client.h"
#include "stream.h"

#define CONNECT_TIMEOUT_SECONDS 20
#define FIRST_RETRY_SECONDS 2
#define LONGEST_RETRY_SECONDS 300
/* CAP REQ and AUTHENTICATE lines stay well under the 512 byte line limit */
#define REQUEST_BYTES 400
#define AUTHENTICATE_CHUNK 400
#define TLS_PORT 6697

struct IrcClient {
	IrcClientSettings settings;
	GStrv wanted_caps;
	IrcClientEvents events;
	gpointer data;
	IrcState *state;
	IrcStream *stream;
	GSocketClient *connector;
	GCancellable *connecting;
	IrcClientStatus status;
	IrcFailure failure;
	char *refused_certificate;
	gboolean wants_connection, has_ended_caps;
	guint reconnect_source, retry_seconds, pending_requests;
	char *trying_nick;
	GHashTable *batches;
};

static void start_connecting(IrcClient *client);


void copy_irc_client_settings(IrcClientSettings *target, const IrcClientSettings *source) {
	*target = (IrcClientSettings){
		.host = g_strdup(source->host),
		.port = source->port,
		.uses_tls = source->uses_tls,
		.nick = g_strdup(source->nick),
		.username = g_strdup(source->username),
		.realname = g_strdup(source->realname),
		.password = g_strdup(source->password),
		.sasl_username = g_strdup(source->sasl_username),
		.sasl_password = g_strdup(source->sasl_password),
		.bound_network = g_strdup(source->bound_network),
		.trusted_certificate = g_strdup(source->trusted_certificate),
		.is_bouncer = source->is_bouncer,
	};
}


void clear_irc_client_settings(IrcClientSettings *settings) {
	g_free(settings->host);
	g_free(settings->nick);
	g_free(settings->username);
	g_free(settings->realname);
	g_free(settings->password);
	g_free(settings->sasl_username);
	g_free(settings->sasl_password);
	g_free(settings->bound_network);
	g_free(settings->trusted_certificate);
	*settings = (IrcClientSettings){0};
}


char *hash_certificate(GTlsCertificate *certificate) {
	g_autoptr(GByteArray) der = NULL;
	g_object_get(certificate, "certificate", &der, NULL);
	if (!der)
		return NULL;
	return g_compute_checksum_for_data(G_CHECKSUM_SHA256, der->data, der->len);
}


char *format_certificate_hash(const char *hash) {
	GString *text = g_string_new(NULL);
	for (const char *pair = hash; pair && pair[0] && pair[1]; pair += 2)
		g_string_append_printf(text, "%s%c%c", text->len ? ":" : "", g_ascii_toupper(pair[0]),
			g_ascii_toupper(pair[1]));
	return g_string_free(text, FALSE);
}


static void free_batch(IrcBatch *batch) {
	g_free(batch->reference);
	g_free(batch->type);
	g_ptr_array_unref(batch->parameters);
	g_free(batch->outer);
	g_free(batch);
}


IrcClient *create_irc_client(const IrcClientSettings *settings, const char *const *wanted_caps,
	const IrcClientEvents *events, gpointer data) {
	IrcClient *client = g_new0(IrcClient, 1);
	copy_irc_client_settings(&client->settings, settings);
	client->wanted_caps = g_strdupv((char **)wanted_caps);
	client->events = *events;
	client->data = data;
	client->state = create_irc_state();
	client->retry_seconds = FIRST_RETRY_SECONDS;
	client->batches = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)free_batch);
	return client;
}


static void set_status(IrcClient *client, IrcClientStatus status, const char *reason) {
	client->status = status;
	client->events.status_changed(client, status, reason, client->data);
}


static void drop_connection(IrcClient *client) {
	g_clear_handle_id(&client->reconnect_source, g_source_remove);
	if (client->connecting) {
		g_cancellable_cancel(client->connecting);
		g_clear_object(&client->connecting);
	}
	g_clear_pointer(&client->stream, close_irc_stream);
	g_hash_table_remove_all(client->batches);
}


void free_irc_client(IrcClient *client) {
	if (!client)
		return;
	drop_connection(client);
	g_clear_object(&client->connector);
	clear_irc_client_settings(&client->settings);
	g_strfreev(client->wanted_caps);
	free_irc_state(client->state);
	g_free(client->refused_certificate);
	g_free(client->trying_nick);
	g_hash_table_unref(client->batches);
	g_free(client);
}


static gboolean retry_connecting(gpointer data) {
	IrcClient *client = data;
	client->reconnect_source = 0;
	start_connecting(client);
	return G_SOURCE_REMOVE;
}


/* A refused certificate or login only changes when the user does something, so those never retry by themselves */
static void end_connection(IrcClient *client, const char *reason) {
	drop_connection(client);
	gboolean was_registered = client->status == IRC_CLIENT_REGISTERED;
	set_status(client, IRC_CLIENT_DISCONNECTED, reason);
	if (!client->wants_connection || client->failure == IRC_FAILURE_CERTIFICATE
		|| client->failure == IRC_FAILURE_LOGIN)
		return;
	if (was_registered)
		client->retry_seconds = FIRST_RETRY_SECONDS;
	client->reconnect_source = g_timeout_add_seconds(client->retry_seconds, retry_connecting, client);
	client->retry_seconds = MIN(client->retry_seconds * 2, LONGEST_RETRY_SECONDS);
}


static void fail_login(IrcClient *client, const char *reason) {
	client->failure = IRC_FAILURE_LOGIN;
	end_connection(client, reason);
}


static void send_registration(IrcClient *client) {
	const IrcClientSettings *settings = &client->settings;
	send_irc_command(client->stream, "CAP", "LS", "302", NULL);
	if (settings->password && *settings->password)
		send_irc_command(client->stream, "PASS", settings->password, NULL);
	g_free(client->trying_nick);
	client->trying_nick = g_strdup(settings->nick);
	send_irc_command(client->stream, "NICK", client->trying_nick, NULL);
	const char *username = settings->username && *settings->username ? settings->username : settings->nick;
	const char *realname = settings->realname && *settings->realname ? settings->realname : settings->nick;
	send_irc_command(client->stream, "USER", username, "0", "*", realname, NULL);
}


static void end_caps(IrcClient *client) {
	if (client->has_ended_caps)
		return;
	client->has_ended_caps = TRUE;
	if (client->settings.bound_network)
		send_irc_command(client->stream, "BOUNCER", "BIND", client->settings.bound_network, NULL);
	send_irc_command(client->stream, "CAP", "END", NULL);
}


static gboolean has_sasl_login(IrcClient *client) {
	return client->settings.sasl_username && *client->settings.sasl_username && client->settings.sasl_password;
}


static gboolean offers_plain_login(IrcClient *client) {
	const char *mechanisms = g_hash_table_lookup(client->state->available_caps, "sasl");
	if (!mechanisms || !*mechanisms)
		return TRUE;
	g_auto(GStrv) names = g_strsplit(mechanisms, ",", -1);
	return g_strv_contains((const char *const *)names, "PLAIN");
}


static gboolean wants_cap(IrcClient *client, const char *name) {
	if (!g_strv_contains((const char *const *)client->wanted_caps, name))
		return FALSE;
	if (g_str_equal(name, "sasl"))
		return has_sasl_login(client) && offers_plain_login(client);
	return TRUE;
}


/* Asks for every wanted capability among the names, in lines short enough for the server */
static void request_caps(IrcClient *client, const char *const *names) {
	GString *line = g_string_new(NULL);
	for (int index = 0; names[index]; index++) {
		if (!wants_cap(client, names[index]) || has_irc_cap(client->state, names[index]))
			continue;
		if (line->len + strlen(names[index]) + 1 > REQUEST_BYTES) {
			send_irc_command(client->stream, "CAP", "REQ", line->str, NULL);
			client->pending_requests++;
			g_string_truncate(line, 0);
		}
		if (line->len)
			g_string_append_c(line, ' ');
		g_string_append(line, names[index]);
	}
	if (line->len) {
		send_irc_command(client->stream, "CAP", "REQ", line->str, NULL);
		client->pending_requests++;
	}
	g_string_free(line, TRUE);
}


static void read_cap_list(IrcClient *client, const char *list) {
	g_auto(GStrv) items = g_strsplit(list ? list : "", " ", -1);
	for (int index = 0; items[index]; index++) {
		if (!*items[index])
			continue;
		char *equals = strchr(items[index], '=');
		if (equals)
			*equals = '\0';
		g_hash_table_insert(client->state->available_caps, g_strdup(items[index]),
			g_strdup(equals ? equals + 1 : ""));
	}
}


/* Strict transport security asks a plain connection to come back over TLS on the given port */
static gboolean upgrade_to_tls(IrcClient *client) {
	const char *policy = g_hash_table_lookup(client->state->available_caps, "sts");
	if (client->settings.uses_tls || !policy)
		return FALSE;
	int port = TLS_PORT;
	g_auto(GStrv) keys = g_strsplit(policy, ",", -1);
	for (int index = 0; keys[index]; index++) {
		if (g_str_has_prefix(keys[index], "port="))
			port = (int)g_ascii_strtoll(keys[index] + 5, NULL, 10);
	}
	client->settings.uses_tls = TRUE;
	client->settings.port = port;
	drop_connection(client);
	start_connecting(client);
	return TRUE;
}


static void finish_listing_caps(IrcClient *client) {
	if (upgrade_to_tls(client))
		return;
	g_autoptr(GPtrArray) names = g_hash_table_get_keys_as_ptr_array(client->state->available_caps);
	g_ptr_array_add(names, NULL);
	request_caps(client, (const char *const *)names->pdata);
	if (!client->pending_requests)
		end_caps(client);
}


static void start_login(IrcClient *client) {
	send_irc_command(client->stream, "AUTHENTICATE", "PLAIN", NULL);
}


static void send_login(IrcClient *client) {
	const char *name = client->settings.sasl_username, *password = client->settings.sasl_password;
	gsize name_length = strlen(name), password_length = strlen(password);
	gsize length = name_length * 2 + password_length + 2;
	g_autofree char *plain = g_malloc(length);
	memcpy(plain, name, name_length);
	plain[name_length] = '\0';
	memcpy(plain + name_length + 1, name, name_length);
	plain[name_length * 2 + 1] = '\0';
	memcpy(plain + name_length * 2 + 2, password, password_length);
	g_autofree char *encoded = g_base64_encode((const guchar *)plain, length);
	memset(plain, 0, length);
	gsize encoded_length = strlen(encoded);
	for (gsize offset = 0; offset < encoded_length; offset += AUTHENTICATE_CHUNK) {
		g_autofree char *chunk = g_strndup(encoded + offset, AUTHENTICATE_CHUNK);
		send_irc_command(client->stream, "AUTHENTICATE", chunk, NULL);
	}
	/* A final chunk of exactly the full size needs an empty one after it to show the end */
	if (encoded_length % AUTHENTICATE_CHUNK == 0)
		send_irc_command(client->stream, "AUTHENTICATE", "+", NULL);
}


static void handle_cap_answer(IrcClient *client, const char *subcommand, const char *list) {
	g_auto(GStrv) names = g_strsplit(list ? list : "", " ", -1);
	gboolean is_ack = g_str_equal(subcommand, "ACK");
	for (int index = 0; is_ack && names[index]; index++) {
		if (*names[index] == '-')
			g_hash_table_remove(client->state->enabled_caps, names[index] + 1);
		else if (*names[index])
			g_hash_table_add(client->state->enabled_caps, g_strdup(names[index]));
	}
	if (client->pending_requests)
		client->pending_requests--;
	if (client->pending_requests || client->has_ended_caps)
		return;
	if (has_irc_cap(client->state, "sasl") && !client->state->account && has_sasl_login(client))
		start_login(client);
	else
		end_caps(client);
}


static void handle_cap(IrcClient *client, const IrcMessage *message) {
	const char *subcommand = get_irc_parameter(message, 1);
	guint count = count_irc_parameters(message);
	const char *list = get_irc_parameter(message, count - 1);
	if (!subcommand)
		return;
	if (g_str_equal(subcommand, "LS")) {
		read_cap_list(client, list);
		gboolean is_more = count > 3 && g_str_equal(get_irc_parameter(message, 2), "*");
		if (!is_more && !client->has_ended_caps)
			finish_listing_caps(client);
	} else if (g_str_equal(subcommand, "ACK") || g_str_equal(subcommand, "NAK")) {
		handle_cap_answer(client, subcommand, list);
	} else if (g_str_equal(subcommand, "NEW")) {
		read_cap_list(client, list);
		g_auto(GStrv) names = g_strsplit(list ? list : "", " ", -1);
		for (int index = 0; names[index]; index++) {
			char *equals = strchr(names[index], '=');
			if (equals)
				*equals = '\0';
		}
		request_caps(client, (const char *const *)names);
	} else if (g_str_equal(subcommand, "DEL")) {
		g_auto(GStrv) names = g_strsplit(list ? list : "", " ", -1);
		for (int index = 0; names[index]; index++) {
			g_hash_table_remove(client->state->enabled_caps, names[index]);
			g_hash_table_remove(client->state->available_caps, names[index]);
		}
	}
}


static void try_another_nick(IrcClient *client) {
	char *longer = g_strconcat(client->trying_nick, "_", NULL);
	g_free(client->trying_nick);
	client->trying_nick = longer;
	send_irc_command(client->stream, "NICK", client->trying_nick, NULL);
}


/* Returns whether registration took the message, so it is not handed on */
static gboolean handle_registration(IrcClient *client, const IrcMessage *message) {
	const char *command = message->command;
	if (g_str_equal(command, "CAP")) {
		handle_cap(client, message);
		return FALSE;
	}
	if (g_str_equal(command, "AUTHENTICATE")) {
		if (g_strcmp0(get_irc_parameter(message, 0), "+") == 0 && has_sasl_login(client))
			send_login(client);
		return TRUE;
	}
	if (g_str_equal(command, "903") || g_str_equal(command, "907")) {
		end_caps(client);
		return FALSE;
	}
	if (g_str_equal(command, "902") || g_str_equal(command, "904") || g_str_equal(command, "905")
		|| g_str_equal(command, "906") || g_str_equal(command, "908")) {
		if (!client->state->is_registered)
			fail_login(client, get_irc_parameter(message, count_irc_parameters(message) - 1));
		return TRUE;
	}
	if (g_str_equal(command, "433") && !client->state->is_registered) {
		try_another_nick(client);
		return FALSE;
	}
	if (g_str_equal(command, "432") && !client->state->is_registered) {
		fail_login(client, get_irc_parameter(message, count_irc_parameters(message) - 1));
		return TRUE;
	}
	return FALSE;
}


/* Lines replayed from history describe the past, so a person who left back then is not removed from the room now */
static gboolean is_history_line(IrcClient *client, const IrcMessage *message) {
	const char *reference = get_irc_tag(message, "batch");
	while (reference) {
		IrcBatch *batch = g_hash_table_lookup(client->batches, reference);
		if (!batch)
			return FALSE;
		if (g_strcmp0(batch->type, "chathistory") == 0)
			return TRUE;
		reference = batch->outer;
	}
	return FALSE;
}


static void track_batch(IrcClient *client, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	if (!reference || (*reference != '+' && *reference != '-'))
		return;
	if (*reference == '-')
		return;
	IrcBatch *batch = g_new0(IrcBatch, 1);
	batch->reference = g_strdup(reference + 1);
	batch->type = g_strdup(get_irc_parameter(message, 1));
	batch->parameters = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 2; index < count_irc_parameters(message); index++)
		g_ptr_array_add(batch->parameters, g_strdup(get_irc_parameter(message, index)));
	batch->outer = g_strdup(get_irc_tag(message, "batch"));
	g_hash_table_insert(client->batches, batch->reference, batch);
}


static void handle_message(IrcStream *stream, IrcMessage *message, gpointer data) {
	IrcClient *client = data;
	if (handle_registration(client, message) || client->status == IRC_CLIENT_DISCONNECTED)
		return;
	if (g_str_equal(message->command, "ERROR")) {
		end_connection(client, get_irc_parameter(message, 0));
		return;
	}
	if (g_str_equal(message->command, "BATCH"))
		track_batch(client, message);
	g_autoptr(GPtrArray) affected = g_ptr_array_new_with_free_func(g_free);
	if (!is_history_line(client, message))
		update_irc_state(client->state, message, affected);
	if (g_str_equal(message->command, "001")) {
		client->failure = IRC_FAILURE_NONE;
		set_status(client, IRC_CLIENT_REGISTERED, NULL);
	}
	client->events.received(client, message, affected, client->data);
	const char *reference = get_irc_parameter(message, 0);
	if (g_str_equal(message->command, "BATCH") && reference && *reference == '-' && client->stream)
		g_hash_table_remove(client->batches, reference + 1);
}


static void handle_closed(IrcStream *stream, const char *reason, gpointer data) {
	IrcClient *client = data;
	client->failure = IRC_FAILURE_NETWORK;
	end_connection(client, reason);
}


static gboolean check_certificate(GTlsConnection *connection, GTlsCertificate *certificate,
	GTlsCertificateFlags errors, gpointer data) {
	IrcClient *client = data;
	g_autofree char *hash = hash_certificate(certificate);
	if (hash && client->settings.trusted_certificate && g_str_equal(hash, client->settings.trusted_certificate))
		return TRUE;
	g_free(client->refused_certificate);
	client->refused_certificate = g_steal_pointer(&hash);
	return FALSE;
}


static void watch_handshake(GSocketClient *connector, GSocketClientEvent event, GSocketConnectable *target,
	GIOStream *connection, gpointer data) {
	if (event == G_SOCKET_CLIENT_TLS_HANDSHAKING)
		g_signal_connect(connection, "accept-certificate", G_CALLBACK(check_certificate), data);
}


static void finish_connecting(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketConnection) connection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source),
		result, &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	IrcClient *client = data;
	g_clear_object(&client->connecting);
	if (!connection) {
		gboolean is_certificate = g_error_matches(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE)
			&& client->refused_certificate;
		client->failure = is_certificate ? IRC_FAILURE_CERTIFICATE : IRC_FAILURE_NETWORK;
		end_connection(client, error->message);
		return;
	}
	/* The connect timeout also applies to every later read, and a quiet channel must not count as a failure */
	g_socket_set_timeout(g_socket_connection_get_socket(connection), 0);
	client->stream = create_irc_stream(G_IO_STREAM(connection), handle_message, handle_closed, client);
	if (!client->settings.is_bouncer)
		limit_irc_stream_rate(client->stream);
	send_registration(client);
}


static void start_connecting(IrcClient *client) {
	drop_connection(client);
	reset_irc_state(client->state);
	client->has_ended_caps = FALSE;
	client->pending_requests = 0;
	g_clear_pointer(&client->refused_certificate, g_free);
	if (!client->connector) {
		client->connector = g_socket_client_new();
		g_socket_client_set_timeout(client->connector, CONNECT_TIMEOUT_SECONDS);
		g_signal_connect(client->connector, "event", G_CALLBACK(watch_handshake), client);
	}
	g_socket_client_set_tls(client->connector, client->settings.uses_tls);
	client->connecting = g_cancellable_new();
	set_status(client, IRC_CLIENT_CONNECTING, NULL);
	g_socket_client_connect_to_host_async(client->connector, client->settings.host,
		(guint16)client->settings.port, client->connecting, finish_connecting, client);
}


void connect_irc_client(IrcClient *client) {
	client->wants_connection = TRUE;
	client->failure = IRC_FAILURE_NONE;
	client->retry_seconds = FIRST_RETRY_SECONDS;
	start_connecting(client);
}


void disconnect_irc_client(IrcClient *client, const char *reason) {
	client->wants_connection = FALSE;
	if (client->stream && client->status != IRC_CLIENT_DISCONNECTED)
		send_irc_command(client->stream, "QUIT", reason ? reason : "", NULL);
	drop_connection(client);
	if (client->status != IRC_CLIENT_DISCONNECTED)
		set_status(client, IRC_CLIENT_DISCONNECTED, NULL);
}


void change_irc_client_settings(IrcClient *client, const IrcClientSettings *settings) {
	clear_irc_client_settings(&client->settings);
	copy_irc_client_settings(&client->settings, settings);
}


void send_irc_client_message(IrcClient *client, const IrcMessage *message) {
	if (client->stream)
		send_irc_message(client->stream, message);
}


void send_irc_client_command(IrcClient *client, const char *command, ...) {
	g_autoptr(IrcMessage) message = create_irc_message(command, NULL);
	va_list parameters;
	va_start(parameters, command);
	for (const char *parameter = va_arg(parameters, const char *); parameter;
		parameter = va_arg(parameters, const char *))
		add_irc_parameter(message, parameter);
	va_end(parameters);
	send_irc_client_message(client, message);
}


IrcState *get_irc_client_state(IrcClient *client) {
	return client->state;
}


const IrcClientSettings *get_irc_client_settings(IrcClient *client) {
	return &client->settings;
}


IrcClientStatus get_irc_client_status(IrcClient *client) {
	return client->status;
}


IrcFailure get_irc_client_failure(IrcClient *client) {
	return client->failure;
}


const char *get_refused_certificate(IrcClient *client) {
	return client->refused_certificate;
}


IrcBatch *find_irc_batch(IrcClient *client, const char *reference) {
	return reference ? g_hash_table_lookup(client->batches, reference) : NULL;
}


IrcBatch *find_enclosing_batch(IrcClient *client, const IrcMessage *message, const char *type) {
	for (IrcBatch *batch = find_irc_batch(client, get_irc_tag(message, "batch")); batch;
		batch = find_irc_batch(client, batch->outer)) {
		if (g_strcmp0(batch->type, type) == 0)
			return batch;
	}
	return NULL;
}
