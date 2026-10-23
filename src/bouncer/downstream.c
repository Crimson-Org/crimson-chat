#include <stdarg.h>
#include <string.h>

#include "bouncer.h"

#define REGISTRATION_SECONDS 60
#define LOGIN_ATTEMPTS 3
#define AUTHENTICATE_CHUNK 400
#define LONGEST_LOGIN_BYTES 4096
#define SUPPORT_TOKENS_PER_LINE 12
#define NAMES_LINE_BYTES 400
#define DEFAULT_TLS_PORT 6697
#define DEFAULT_PLAIN_PORT 6667

static const char *const OFFERED_CAPS[] = {
	"sasl=PLAIN", "soju.im/bouncer-networks", "soju.im/bouncer-networks-notify", "server-time", "message-tags",
	"batch", "echo-message", "draft/chathistory", "draft/event-playback", "draft/read-marker", "cap-notify",
	"account-notify", "away-notify", "extended-join", "multi-prefix", "userhost-in-names", "chghost", "setname",
	"invite-notify", "account-tag", "draft/message-redaction", "draft/metadata-2", "draft/channel-rename",
	"extended-monitor", "crimson-chat/invites", "crimson-chat/reconnect", "draft/account-registration", NULL,
};

/* Commands a client only receives once it took the capability that describes them */
static const struct {
	const char *command, *cap;
} COMMAND_CAPS[] = {
	{"AWAY", "away-notify"}, {"ACCOUNT", "account-notify"}, {"CHGHOST", "chghost"}, {"SETNAME", "setname"},
	{"TAGMSG", "message-tags"}, {"BATCH", "batch"}, {"METADATA", "draft/metadata-2"},
	{"REDACT", "draft/message-redaction"}, {"RENAME", "draft/channel-rename"}, {"MARKREAD", "draft/read-marker"},
};

/* Capabilities the network itself has to support, since the bouncer only passes their commands on */
static const char *const NETWORK_CAPS[] = {
	"account-notify", "away-notify", "extended-join", "userhost-in-names", "chghost", "setname", "invite-notify",
	"account-tag", "draft/message-redaction", "draft/metadata-2", "draft/channel-rename", "extended-monitor",
	"draft/account-registration", NULL,
};

static const char *const NETWORK_ATTRIBUTES[] = {
	"name", "host", "port", "tls", "nickname", "username", "realname", "pass", "sasl-username", "sasl-password", NULL,
};


gboolean has_downstream_cap(Downstream *downstream, const char *name) {
	return g_hash_table_contains(downstream->caps, name);
}


static gboolean is_tag_allowed(Downstream *downstream, const char *name) {
	if (g_str_equal(name, "time"))
		return has_downstream_cap(downstream, "server-time");
	if (g_str_equal(name, "account"))
		return has_downstream_cap(downstream, "account-tag");
	if (g_str_equal(name, "batch"))
		return has_downstream_cap(downstream, "batch");
	if (g_str_equal(name, "label"))
		return FALSE;
	return has_downstream_cap(downstream, "message-tags");
}


/* Rewrites a NAMES reply for a client without multi-prefix or userhost-in-names */
static void shape_names(Downstream *downstream, IrcMessage *message) {
	gboolean keeps_prefixes = has_downstream_cap(downstream, "multi-prefix");
	gboolean keeps_hosts = has_downstream_cap(downstream, "userhost-in-names");
	const char *names = get_irc_parameter(message, 3);
	if ((keeps_prefixes && keeps_hosts) || !names || !downstream->upstream)
		return;
	IrcState *state = get_irc_client_state(downstream->upstream->client);
	g_auto(GStrv) entries = g_strsplit(names, " ", -1);
	GString *shaped = g_string_new(NULL);
	for (int index = 0; entries[index]; index++) {
		const char *nick = entries[index];
		while (*nick && strchr(state->prefix_symbols, *nick))
			nick++;
		g_autofree char *bare = g_strdup(nick);
		char *bang = strchr(bare, '!');
		if (bang && !keeps_hosts)
			*bang = '\0';
		g_string_append_printf(shaped, "%s%.*s%s", shaped->len ? " " : "",
			keeps_prefixes ? (int)(nick - entries[index]) : MIN(1, (int)(nick - entries[index])), entries[index],
			bare);
	}
	g_free(g_ptr_array_index(message->parameters, 3));
	g_ptr_array_index(message->parameters, 3) = g_string_free(shaped, FALSE);
}


static gboolean is_command_allowed(Downstream *downstream, const IrcMessage *message) {
	for (guint index = 0; index < G_N_ELEMENTS(COMMAND_CAPS); index++) {
		if (g_str_equal(message->command, COMMAND_CAPS[index].command))
			return has_downstream_cap(downstream, COMMAND_CAPS[index].cap);
	}
	if (g_str_equal(message->command, "INVITE"))
		return has_downstream_cap(downstream, "invite-notify")
			|| g_strcmp0(get_irc_parameter(message, 0), downstream->nick) == 0;
	return TRUE;
}


void send_to_downstream(Downstream *downstream, const IrcMessage *message) {
	if (!is_command_allowed(downstream, message))
		return;
	g_autoptr(IrcMessage) shaped = copy_irc_message(message);
	GHashTableIter iterator;
	gpointer name;
	g_hash_table_iter_init(&iterator, shaped->tags);
	while (g_hash_table_iter_next(&iterator, &name, NULL)) {
		if (!is_tag_allowed(downstream, name))
			g_hash_table_iter_remove(&iterator);
	}
	if (g_str_equal(shaped->command, "JOIN") && !has_downstream_cap(downstream, "extended-join"))
		g_ptr_array_set_size(shaped->parameters, MIN(shaped->parameters->len, 1));
	if (g_str_equal(shaped->command, "353"))
		shape_names(downstream, shaped);
	send_irc_message(downstream->stream, shaped);
}


static void send_downstream_list(Downstream *downstream, const char *numeric, va_list parameters) {
	g_autoptr(IrcMessage) message = create_irc_message(numeric, downstream->nick ? downstream->nick : "*", NULL);
	message->source = g_strdup(BOUNCER_NAME);
	for (const char *parameter = va_arg(parameters, const char *); parameter;
		parameter = va_arg(parameters, const char *))
		add_irc_parameter(message, parameter);
	send_to_downstream(downstream, message);
}


void send_downstream_numeric(Downstream *downstream, const char *numeric, ...) {
	va_list parameters;
	va_start(parameters, numeric);
	send_downstream_list(downstream, numeric, parameters);
	va_end(parameters);
}


static void send_from_bouncer(Downstream *downstream, const char *command, ...) {
	g_autoptr(IrcMessage) message = create_irc_message(command, NULL);
	message->source = g_strdup(BOUNCER_NAME);
	va_list parameters;
	va_start(parameters, command);
	for (const char *parameter = va_arg(parameters, const char *); parameter;
		parameter = va_arg(parameters, const char *))
		add_irc_parameter(message, parameter);
	va_end(parameters);
	send_to_downstream(downstream, message);
}


static gboolean free_downstream(gpointer data) {
	Downstream *downstream = data;
	g_hash_table_unref(downstream->caps);
	g_free(downstream->address);
	g_free(downstream->nick);
	g_free(downstream->username);
	g_free(downstream->password);
	g_free(downstream->bound_network);
	g_string_free(downstream->login, TRUE);
	g_free(downstream);
	return G_SOURCE_REMOVE;
}


/* The memory goes once the current message is handled, since a handler may close its own client */
void close_downstream(Downstream *downstream) {
	if (!downstream->stream)
		return;
	g_clear_pointer(&downstream->stream, close_irc_stream);
	g_clear_handle_id(&downstream->registration_source, g_source_remove);
	forget_downstream(downstream->bouncer, downstream);
	g_idle_add(free_downstream, downstream);
}


static void refuse_downstream(Downstream *downstream, const char *reason) {
	send_irc_command(downstream->stream, "ERROR", reason, NULL);
	g_message("Refused %s: %s", downstream->address, reason);
	close_downstream(downstream);
}


static char *format_support_token(const char *name, const char *value) {
	if (!value || !*value)
		return g_strdup(name);
	GString *token = g_string_new(name);
	g_string_append_c(token, '=');
	for (const char *character = value; *character; character++) {
		if (*character == ' ' || *character == '\\' || *character == '=')
			g_string_append_printf(token, "\\x%02X", (unsigned char)*character);
		else
			g_string_append_c(token, *character);
	}
	return g_string_free(token, FALSE);
}


static GPtrArray *list_support_tokens(Downstream *downstream) {
	GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
	if (!downstream->upstream) {
		g_ptr_array_add(tokens, g_strdup("CASEMAPPING=ascii"));
		/* The name the owner gave the bouncer, which an app shows instead of its address */
		const char *name = downstream->bouncer->name ? downstream->bouncer->name : BOUNCER_NAME;
		g_ptr_array_add(tokens, g_strdup_printf("NETWORK=%s", name));
		return tokens;
	}
	GHashTableIter iterator;
	gpointer name, value;
	g_hash_table_iter_init(&iterator, get_irc_client_state(downstream->upstream->client)->support);
	while (g_hash_table_iter_next(&iterator, &name, &value)) {
		if (!g_str_equal(name, "CHATHISTORY") && !g_str_equal(name, "MSGREFTYPES")
			&& !g_str_equal(name, "BOUNCER_NETID"))
			g_ptr_array_add(tokens, format_support_token(name, value));
	}
	g_ptr_array_add(tokens, g_strdup_printf("BOUNCER_NETID=%" G_GINT64_FORMAT, downstream->upstream->record.id));
	/* Where the server keeps pictures, the bouncer takes uploads for them: at its public address, or else at its
	 * website's port on whatever address the app reached it by */
	AvatarSettings *avatars = &downstream->bouncer->avatars;
	if (avatars->is_enabled && has_irc_cap(get_irc_client_state(downstream->upstream->client), "draft/metadata-2"))
		g_ptr_array_add(tokens, avatars->public_url ? g_strdup_printf("crimson-chat/AVATARS=%s/avatar",
			avatars->public_url) : g_strdup_printf("crimson-chat/AVATARS=%s", downstream->bouncer->web_port));
	g_ptr_array_add(tokens, g_strdup_printf("CHATHISTORY=%d", HISTORY_PAGE_LIMIT));
	g_ptr_array_add(tokens, g_strdup("MSGREFTYPES=timestamp,msgid"));
	return tokens;
}


void send_support_tokens(Downstream *downstream) {
	g_autoptr(GPtrArray) tokens = list_support_tokens(downstream);
	for (guint start = 0; start < tokens->len; start += SUPPORT_TOKENS_PER_LINE) {
		g_autoptr(IrcMessage) message = create_irc_message("005", downstream->nick, NULL);
		message->source = g_strdup(BOUNCER_NAME);
		for (guint index = start; index < tokens->len && index < start + SUPPORT_TOKENS_PER_LINE; index++)
			add_irc_parameter(message, g_ptr_array_index(tokens, index));
		add_irc_parameter(message, "are supported by this server");
		send_to_downstream(downstream, message);
	}
}


static void send_welcome(Downstream *downstream) {
	g_autofree char *welcome = g_strdup_printf("Welcome to the Crimson Chat bouncer, %s", downstream->nick);
	send_downstream_numeric(downstream, "001", welcome, NULL);
	send_downstream_numeric(downstream, "002", "Your host is " BOUNCER_NAME, NULL);
	send_downstream_numeric(downstream, "003", "This bouncer keeps your networks connected", NULL);
	send_downstream_numeric(downstream, "004", BOUNCER_NAME, APPLICATION_VERSION, NULL);
	send_support_tokens(downstream);
	send_downstream_numeric(downstream, "422", "No message of the day", NULL);
}


static void send_network_list(Downstream *downstream) {
	g_autofree char *reference = create_irc_identifier();
	gboolean uses_batch = has_downstream_cap(downstream, "batch");
	g_autofree char *opening = g_strconcat("+", reference, NULL);
	g_autofree char *closing = g_strconcat("-", reference, NULL);
	if (uses_batch)
		send_from_bouncer(downstream, "BATCH", opening, "soju.im/bouncer-networks", NULL);
	GPtrArray *upstreams = downstream->user->upstreams;
	for (guint index = 0; index < upstreams->len; index++) {
		Upstream *upstream = g_ptr_array_index(upstreams, index);
		g_autoptr(GHashTable) attributes = describe_upstream(upstream);
		g_autofree char *text = format_irc_attributes(attributes);
		g_autofree char *network_id = g_strdup_printf("%" G_GINT64_FORMAT, upstream->record.id);
		g_autoptr(IrcMessage) message = create_irc_message("BOUNCER", "NETWORK", network_id, text, NULL);
		message->source = g_strdup(BOUNCER_NAME);
		if (uses_batch)
			set_irc_tag(message, "batch", reference);
		send_to_downstream(downstream, message);
	}
	if (uses_batch)
		send_from_bouncer(downstream, "BATCH", closing, NULL);
}


/* A client bound to a network loses the capabilities that network lacks, so it never sends their commands */
void drop_missing_caps(Downstream *downstream, gpointer data) {
	if (!downstream->upstream || !has_downstream_cap(downstream, "cap-notify"))
		return;
	IrcClient *client = downstream->upstream->client;
	if (get_irc_client_status(client) != IRC_CLIENT_REGISTERED)
		return;
	IrcState *state = get_irc_client_state(client);
	g_autoptr(GString) missing = g_string_new(NULL);
	for (int index = 0; NETWORK_CAPS[index]; index++) {
		if (has_irc_cap(state, NETWORK_CAPS[index]))
			continue;
		g_string_append_printf(missing, "%s%s", missing->len ? " " : "", NETWORK_CAPS[index]);
		g_hash_table_remove(downstream->caps, NETWORK_CAPS[index]);
	}
	if (missing->len)
		send_from_bouncer(downstream, "CAP", downstream->nick, "DEL", missing->str, NULL);
}


static void finish_registration(Downstream *downstream) {
	if (downstream->bound_network) {
		downstream->upstream = find_upstream(downstream->user, downstream->bound_network);
		if (!downstream->upstream) {
			send_from_bouncer(downstream, "FAIL", "BOUNCER", "INVALID_NETID", "BIND", downstream->bound_network,
				"Network not found", NULL);
			refuse_downstream(downstream, "Network not found");
			return;
		}
		g_free(downstream->nick);
		downstream->nick = get_upstream_nick(downstream->upstream);
	}
	downstream->is_registered = TRUE;
	g_clear_handle_id(&downstream->registration_source, g_source_remove);
	g_message("%s logged in from %s%s%s", downstream->user->name, downstream->address,
		downstream->upstream ? " to " : "", downstream->upstream ? downstream->upstream->record.host : "");
	drop_missing_caps(downstream, NULL);
	send_welcome(downstream);
	/* An admin's app offers invites and the website's admin page, so it learns the website's port */
	if (!downstream->upstream && downstream->user->is_admin && has_downstream_cap(downstream, "crimson-chat/invites"))
		send_from_bouncer(downstream, "CRIMSON", "ADMIN", downstream->bouncer->web_port, NULL);
	if (downstream->upstream)
		replay_upstream(downstream);
	/* A client joining while the network is down learns why, and whether the bouncer is trying again */
	if (downstream->upstream && get_irc_client_status(downstream->upstream->client) != IRC_CLIENT_REGISTERED) {
		g_autofree char *progress = describe_upstream_progress(downstream->upstream);
		send_from_bouncer(downstream, "NOTICE", downstream->nick, progress, NULL);
	}
	else if (has_downstream_cap(downstream, "soju.im/bouncer-networks-notify"))
		send_network_list(downstream);
}


/* A client without SASL logs in with PASS as name:password, or the password alone with its USER name */
static gboolean log_in_with_pass(Downstream *downstream) {
	if (!downstream->password)
		return FALSE;
	const char *colon = strchr(downstream->password, ':');
	g_autofree char *name = colon ? g_strndup(downstream->password, (gsize)(colon - downstream->password))
		: g_strdup(downstream->username);
	const char *password = colon ? colon + 1 : downstream->password;
	downstream->user = log_in_user(downstream->bouncer, name, password);
	return downstream->user != NULL;
}


static void try_registering(Downstream *downstream) {
	if (downstream->is_registered || downstream->is_negotiating || !downstream->nick || !downstream->username)
		return;
	if (!downstream->user && !log_in_with_pass(downstream)) {
		send_downstream_numeric(downstream, "464", "Password incorrect", NULL);
		refuse_downstream(downstream, "Login failed");
		return;
	}
	finish_registration(downstream);
}


static char *list_offered_caps(Downstream *downstream) {
	GString *list = g_string_new(NULL);
	for (int index = 0; OFFERED_CAPS[index]; index++) {
		const char *cap = OFFERED_CAPS[index];
		const char *equals = strchr(cap, '=');
		g_string_append_printf(list, "%s%.*s", list->len ? " " : "",
			equals && !downstream->has_cap_values ? (int)(equals - cap) : (int)strlen(cap), cap);
	}
	return g_string_free(list, FALSE);
}


static gboolean is_cap_offered(const char *name) {
	for (int index = 0; OFFERED_CAPS[index]; index++) {
		const char *equals = strchr(OFFERED_CAPS[index], '=');
		gsize length = equals ? (gsize)(equals - OFFERED_CAPS[index]) : strlen(OFFERED_CAPS[index]);
		if (strlen(name) == length && strncmp(name, OFFERED_CAPS[index], length) == 0)
			return TRUE;
	}
	return FALSE;
}


static void request_downstream_caps(Downstream *downstream, const char *list) {
	g_auto(GStrv) names = g_strsplit(list ? list : "", " ", -1);
	for (int index = 0; names[index]; index++) {
		const char *name = *names[index] == '-' ? names[index] + 1 : names[index];
		if (*names[index] && !is_cap_offered(name)) {
			send_from_bouncer(downstream, "CAP", downstream->nick ? downstream->nick : "*", "NAK", list, NULL);
			return;
		}
	}
	for (int index = 0; names[index]; index++) {
		if (*names[index] == '-')
			g_hash_table_remove(downstream->caps, names[index] + 1);
		else if (*names[index])
			g_hash_table_add(downstream->caps, g_strdup(names[index]));
	}
	send_from_bouncer(downstream, "CAP", downstream->nick ? downstream->nick : "*", "ACK", list, NULL);
}


static void handle_cap(Downstream *downstream, const IrcMessage *message) {
	const char *subcommand = get_irc_parameter(message, 0);
	const char *nick = downstream->nick ? downstream->nick : "*";
	if (!subcommand)
		return;
	if (g_ascii_strcasecmp(subcommand, "LS") == 0) {
		if (!downstream->is_registered)
			downstream->is_negotiating = TRUE;
		const char *version = get_irc_parameter(message, 1);
		if (version && g_ascii_strtoll(version, NULL, 10) >= 302) {
			downstream->has_cap_values = TRUE;
			g_hash_table_add(downstream->caps, g_strdup("cap-notify"));
		}
		g_autofree char *list = list_offered_caps(downstream);
		send_from_bouncer(downstream, "CAP", nick, "LS", list, NULL);
	} else if (g_ascii_strcasecmp(subcommand, "LIST") == 0) {
		g_autoptr(GPtrArray) names = g_hash_table_get_keys_as_ptr_array(downstream->caps);
		g_ptr_array_add(names, NULL);
		g_autofree char *list = g_strjoinv(" ", (char **)names->pdata);
		send_from_bouncer(downstream, "CAP", nick, "LIST", list, NULL);
	} else if (g_ascii_strcasecmp(subcommand, "REQ") == 0) {
		if (!downstream->is_registered)
			downstream->is_negotiating = TRUE;
		request_downstream_caps(downstream, get_irc_parameter(message, 1));
	} else if (g_ascii_strcasecmp(subcommand, "END") == 0) {
		downstream->is_negotiating = FALSE;
		try_registering(downstream);
	}
}


static void check_login(Downstream *downstream) {
	gsize length = 0;
	g_autofree guchar *plain = g_base64_decode(downstream->login->str, &length);
	g_string_truncate(downstream->login, 0);
	/* PLAIN is the authorization name, the login name and the password, separated by zero bytes */
	const char *fields[3] = {0};
	gsize start = 0;
	int field = 0;
	for (gsize index = 0; plain && index <= length && field < 3; index++) {
		if (index == length || plain[index] == '\0') {
			fields[field++] = (const char *)plain + start;
			if (index < length)
				plain[index] = '\0';
			start = index + 1;
		}
	}
	BouncerUser *user = field == 3 ? log_in_user(downstream->bouncer, fields[1], fields[2]) : NULL;
	if (plain)
		memset(plain, 0, length);
	if (!user) {
		send_downstream_numeric(downstream, "904", "SASL authentication failed", NULL);
		if (++downstream->failed_logins >= LOGIN_ATTEMPTS)
			refuse_downstream(downstream, "Too many failed logins");
		return;
	}
	downstream->user = user;
	g_autofree char *mask = g_strdup_printf("%s!%s@%s", downstream->nick ? downstream->nick : "*", user->name,
		BOUNCER_NAME);
	g_autofree char *greeting = g_strdup_printf("You are now logged in as %s", user->name);
	send_downstream_numeric(downstream, "900", mask, user->name, greeting, NULL);
	send_downstream_numeric(downstream, "903", "SASL authentication successful", NULL);
}


static void handle_authenticate(Downstream *downstream, const IrcMessage *message) {
	const char *data = get_irc_parameter(message, 0);
	if (downstream->user) {
		send_downstream_numeric(downstream, "907", "You have already authenticated", NULL);
		return;
	}
	if (!data)
		return;
	if (g_str_equal(data, "*")) {
		g_string_truncate(downstream->login, 0);
		send_downstream_numeric(downstream, "906", "SASL authentication aborted", NULL);
	} else if (g_ascii_strcasecmp(data, "PLAIN") == 0) {
		send_from_bouncer(downstream, "AUTHENTICATE", "+", NULL);
	} else if (!g_str_equal(data, "+") && downstream->login->len + strlen(data) > LONGEST_LOGIN_BYTES) {
		refuse_downstream(downstream, "Login too long");
	} else {
		if (!g_str_equal(data, "+"))
			g_string_append(downstream->login, data);
		if (strlen(data) < AUTHENTICATE_CHUNK || g_str_equal(data, "+"))
			check_login(downstream);
	}
}


static gboolean is_known_attribute(const char *name) {
	return g_strv_contains(NETWORK_ATTRIBUTES, name);
}


/* Returns whether every attribute can be used, and otherwise sends the failure for the first one */
static gboolean check_attributes(Downstream *downstream, const char *subcommand, const char *network_id,
	GHashTable *attributes) {
	GHashTableIter iterator;
	gpointer name, value;
	g_hash_table_iter_init(&iterator, attributes);
	while (g_hash_table_iter_next(&iterator, &name, &value)) {
		const char *problem = NULL;
		if (g_str_equal(name, "state") || g_str_equal(name, "error"))
			problem = "READ_ONLY_ATTRIBUTE";
		else if (!is_known_attribute(name))
			problem = "UNKNOWN_ATTRIBUTE";
		else if (g_str_equal(name, "port") && (g_ascii_strtoll(value, NULL, 10) < 1
			|| g_ascii_strtoll(value, NULL, 10) > G_MAXUINT16))
			problem = "INVALID_ATTRIBUTE";
		else if (g_str_equal(name, "tls") && !g_str_equal(value, "0") && !g_str_equal(value, "1"))
			problem = "INVALID_ATTRIBUTE";
		else if (g_str_equal(name, "host") && !*(const char *)value)
			problem = "INVALID_ATTRIBUTE";
		if (problem) {
			send_from_bouncer(downstream, "FAIL", "BOUNCER", problem, subcommand, network_id, name,
				"Cannot use this attribute", NULL);
			return FALSE;
		}
	}
	return TRUE;
}


static void set_record_text(char **field, GHashTable *attributes, const char *name) {
	const char *value = g_hash_table_lookup(attributes, name);
	if (!value)
		return;
	g_free(*field);
	*field = *value ? g_strdup(value) : NULL;
}


static void apply_attributes(NetworkRecord *record, GHashTable *attributes) {
	set_record_text(&record->name, attributes, "name");
	set_record_text(&record->host, attributes, "host");
	set_record_text(&record->nickname, attributes, "nickname");
	set_record_text(&record->username, attributes, "username");
	set_record_text(&record->realname, attributes, "realname");
	set_record_text(&record->password, attributes, "pass");
	set_record_text(&record->sasl_username, attributes, "sasl-username");
	set_record_text(&record->sasl_password, attributes, "sasl-password");
	const char *tls = g_hash_table_lookup(attributes, "tls");
	if (tls)
		record->uses_tls = g_str_equal(tls, "1");
	const char *port = g_hash_table_lookup(attributes, "port");
	if (port)
		record->port = (int)g_ascii_strtoll(port, NULL, 10);
}


static void add_network_for(Downstream *downstream, const char *text) {
	g_autoptr(GHashTable) attributes = parse_irc_attributes(text);
	if (!check_attributes(downstream, "ADDNETWORK", "*", attributes))
		return;
	if (!g_hash_table_lookup(attributes, "host")) {
		send_from_bouncer(downstream, "FAIL", "BOUNCER", "NEED_ATTRIBUTE", "ADDNETWORK", "host",
			"Missing required attribute", NULL);
		return;
	}
	NetworkRecord record = {.user_id = downstream->user->id, .uses_tls = TRUE, .port = 0};
	apply_attributes(&record, attributes);
	if (!record.port)
		record.port = record.uses_tls ? DEFAULT_TLS_PORT : DEFAULT_PLAIN_PORT;
	record.id = add_network(downstream->bouncer->database, &record);
	add_upstream(downstream->user, &record);
	g_autofree char *network_id = g_strdup_printf("%" G_GINT64_FORMAT, record.id);
	send_from_bouncer(downstream, "BOUNCER", "ADDNETWORK", network_id, NULL);
	clear_network_record(&record);
}


static void change_network_for(Downstream *downstream, const char *network_id, const char *text) {
	Upstream *upstream = find_upstream(downstream->user, network_id);
	if (!upstream) {
		send_from_bouncer(downstream, "FAIL", "BOUNCER", "INVALID_NETID", "CHANGENETWORK", network_id ? network_id
			: "*", "Network not found", NULL);
		return;
	}
	g_autoptr(GHashTable) attributes = parse_irc_attributes(text);
	if (!check_attributes(downstream, "CHANGENETWORK", network_id, attributes))
		return;
	/* A login saved for the account the connection is already logged into, as after registering, needs no
	 * reconnect, and neither does a new name or real name */
	IrcState *state = get_irc_client_state(upstream->client);
	const char *new_login = g_hash_table_lookup(attributes, "sasl-username");
	gboolean is_current_login = new_login && state->account && g_ascii_strcasecmp(new_login, state->account) == 0;
	guint quiet_keys = g_hash_table_contains(attributes, "name") + g_hash_table_contains(attributes, "realname");
	if (is_current_login)
		quiet_keys += g_hash_table_contains(attributes, "sasl-username") + g_hash_table_contains(attributes,
			"sasl-password");
	gboolean needs_restart = quiet_keys < g_hash_table_size(attributes);
	apply_attributes(&upstream->record, attributes);
	save_network(downstream->bouncer->database, &upstream->record);
	/* A network with SETNAME takes the real name at once, and any other one on its next connection */
	const char *realname = g_hash_table_lookup(attributes, "realname");
	gboolean is_registered = get_irc_client_status(upstream->client) == IRC_CLIENT_REGISTERED;
	if (!needs_restart && realname && *realname && is_registered && has_irc_cap(state, "setname"))
		send_irc_client_command(upstream->client, "SETNAME", realname, NULL);
	g_hash_table_remove(attributes, "pass");
	g_hash_table_remove(attributes, "sasl-password");
	announce_network(upstream, g_hash_table_ref(attributes));
	if (needs_restart)
		restart_upstream(upstream);
	send_from_bouncer(downstream, "BOUNCER", "CHANGENETWORK", network_id, NULL);
}


/* The client asks for a network to reconnect, such as to log in with a real name the network cannot change */
static void reconnect_network_for(Downstream *downstream, const char *network_id) {
	Upstream *upstream = find_upstream(downstream->user, network_id);
	if (!upstream) {
		send_from_bouncer(downstream, "FAIL", "BOUNCER", "INVALID_NETID", "RECONNECTNETWORK", network_id ? network_id
			: "*", "Network not found", NULL);
		return;
	}
	restart_upstream(upstream);
}


static void delete_network_for(Downstream *downstream, const char *network_id) {
	Upstream *upstream = find_upstream(downstream->user, network_id);
	if (!upstream) {
		send_from_bouncer(downstream, "FAIL", "BOUNCER", "INVALID_NETID", "DELNETWORK", network_id ? network_id
			: "*", "Network not found", NULL);
		return;
	}
	g_autofree char *kept_id = g_strdup(network_id);
	remove_upstream(upstream);
	send_from_bouncer(downstream, "BOUNCER", "DELNETWORK", kept_id, NULL);
}


static void handle_bouncer(Downstream *downstream, const IrcMessage *message) {
	const char *subcommand = get_irc_parameter(message, 0);
	if (!subcommand)
		return;
	if (g_ascii_strcasecmp(subcommand, "BIND") == 0) {
		if (downstream->is_registered)
			send_from_bouncer(downstream, "FAIL", "BOUNCER", "REGISTRATION_IS_COMPLETED", "BIND",
				"Cannot bind to a network after registration", NULL);
		else if (!downstream->user)
			send_from_bouncer(downstream, "FAIL", "BOUNCER", "ACCOUNT_REQUIRED", "BIND", "Authentication required",
				NULL);
		else {
			g_free(downstream->bound_network);
			downstream->bound_network = g_strdup(get_irc_parameter(message, 1));
		}
	} else if (!downstream->is_registered) {
		send_downstream_numeric(downstream, "451", "You have not registered", NULL);
	} else if (g_ascii_strcasecmp(subcommand, "LISTNETWORKS") == 0) {
		send_network_list(downstream);
	} else if (g_ascii_strcasecmp(subcommand, "ADDNETWORK") == 0) {
		add_network_for(downstream, get_irc_parameter(message, 1));
	} else if (g_ascii_strcasecmp(subcommand, "CHANGENETWORK") == 0) {
		change_network_for(downstream, get_irc_parameter(message, 1), get_irc_parameter(message, 2));
	} else if (g_ascii_strcasecmp(subcommand, "DELNETWORK") == 0) {
		delete_network_for(downstream, get_irc_parameter(message, 1));
	} else if (g_ascii_strcasecmp(subcommand, "RECONNECTNETWORK") == 0) {
		reconnect_network_for(downstream, get_irc_parameter(message, 1));
	} else {
		send_from_bouncer(downstream, "FAIL", "BOUNCER", "UNKNOWN_COMMAND", subcommand, "Unknown subcommand", NULL);
	}
}


static void set_text(char **field, const char *value) {
	g_free(*field);
	*field = g_strdup(value);
}


static void handle_registering(Downstream *downstream, const IrcMessage *message) {
	const char *command = message->command;
	if (g_str_equal(command, "PASS")) {
		set_text(&downstream->password, get_irc_parameter(message, 0));
	} else if (g_str_equal(command, "NICK")) {
		set_text(&downstream->nick, get_irc_parameter(message, 0));
		try_registering(downstream);
	} else if (g_str_equal(command, "USER")) {
		set_text(&downstream->username, get_irc_parameter(message, 0));
		try_registering(downstream);
	} else if (!g_str_equal(command, "PONG")) {
		send_downstream_numeric(downstream, "451", "You have not registered", NULL);
	}
}


/* CRIMSON INVITE CREATE with a use count, or * for unlimited, lets an admin make invite links from a client,
 * and the answer names the website's port the link points to */
static void handle_invite_command(Downstream *downstream, const IrcMessage *message) {
	const char *subcommand = get_irc_parameter(message, 0);
	const char *action = get_irc_parameter(message, 1);
	if (g_strcmp0(subcommand, "INVITE") != 0 || g_strcmp0(action, "CREATE") != 0) {
		send_from_bouncer(downstream, "FAIL", "CRIMSON", "UNKNOWN_COMMAND", subcommand ? subcommand : "*",
			"Unknown subcommand", NULL);
		return;
	}
	if (!downstream->user->is_admin) {
		send_from_bouncer(downstream, "FAIL", "CRIMSON", "NOT_ADMIN", "INVITE", "Only admins can invite people", NULL);
		return;
	}
	const char *count = get_irc_parameter(message, 2);
	int uses = !count || g_str_equal(count, "*") ? -1 : MAX(1, (int)g_ascii_strtoll(count, NULL, 10));
	g_autofree char *token = create_invite(downstream->bouncer, downstream->user, uses);
	g_autofree char *uses_text = uses < 0 ? g_strdup("*") : g_strdup_printf("%d", uses);
	send_from_bouncer(downstream, "CRIMSON", "INVITE", token, uses_text, downstream->bouncer->web_port, NULL);
}


/* Commands that only work where the network has the capability behind them */
static const struct {
	const char *command, *cap;
} NETWORK_COMMANDS[] = {
	{"METADATA", "draft/metadata-2"}, {"REDACT", "draft/message-redaction"}, {"SETNAME", "setname"},
	{"RENAME", "draft/channel-rename"}, {"REGISTER", "draft/account-registration"},
	{"VERIFY", "draft/account-registration"},
};


/* A client can send such a command before it learns the network lacks it, as while the bouncer reconnects, so the
 * bouncer answers for the network instead of passing on a command it would only call unknown */
static void forward_supported(Downstream *downstream, const IrcMessage *message) {
	IrcClient *client = downstream->upstream->client;
	gboolean is_registered = get_irc_client_status(client) == IRC_CLIENT_REGISTERED;
	for (guint index = 0; index < G_N_ELEMENTS(NETWORK_COMMANDS); index++) {
		if (!g_str_equal(message->command, NETWORK_COMMANDS[index].command))
			continue;
		if (is_registered && has_irc_cap(get_irc_client_state(client), NETWORK_COMMANDS[index].cap))
			break;
		send_from_bouncer(downstream, "FAIL", message->command, "UNSUPPORTED",
			"The network does not support this", NULL);
		return;
	}
	forward_to_upstream(downstream->upstream, downstream, message);
}


static void handle_message(IrcStream *stream, IrcMessage *message, gpointer data) {
	Downstream *downstream = data;
	const char *command = message->command;
	if (g_str_equal(command, "QUIT"))
		close_downstream(downstream);
	else if (g_str_equal(command, "CAP"))
		handle_cap(downstream, message);
	else if (g_str_equal(command, "AUTHENTICATE"))
		handle_authenticate(downstream, message);
	else if (g_str_equal(command, "BOUNCER"))
		handle_bouncer(downstream, message);
	else if (!downstream->is_registered)
		handle_registering(downstream, message);
	else if (g_str_equal(command, "PASS") || g_str_equal(command, "USER"))
		send_downstream_numeric(downstream, "462", "You may not reregister", NULL);
	else if (g_str_equal(command, "CRIMSON"))
		handle_invite_command(downstream, message);
	else if (!downstream->upstream)
		send_downstream_numeric(downstream, "421", command, "Bind to a network to use this command", NULL);
	else if (g_str_equal(command, "CHATHISTORY"))
		answer_history_request(downstream, message);
	else if (g_str_equal(command, "MARKREAD"))
		answer_read_marker(downstream, message);
	else
		forward_supported(downstream, message);
}


static void handle_closed(IrcStream *stream, const char *reason, gpointer data) {
	Downstream *downstream = data;
	if (downstream->user)
		g_message("%s from %s left: %s", downstream->user->name, downstream->address, reason);
	close_downstream(downstream);
}


static gboolean drop_unregistered(gpointer data) {
	Downstream *downstream = data;
	downstream->registration_source = 0;
	if (!downstream->is_registered)
		refuse_downstream(downstream, "Registration timed out");
	return G_SOURCE_REMOVE;
}


void accept_downstream(Bouncer *bouncer, GIOStream *connection, const char *address) {
	Downstream *downstream = g_new0(Downstream, 1);
	downstream->bouncer = bouncer;
	downstream->address = g_strdup(address);
	downstream->caps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	downstream->login = g_string_new(NULL);
	downstream->stream = create_irc_stream(connection, handle_message, handle_closed, downstream);
	bouncer->downstreams = g_list_prepend(bouncer->downstreams, downstream);
	downstream->registration_source = g_timeout_add_seconds(REGISTRATION_SECONDS, drop_unregistered, downstream);
}
