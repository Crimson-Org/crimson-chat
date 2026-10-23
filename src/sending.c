#include <string.h>

#include "../lang/en.h"
#include "session.h"
#include "settings.h"

/* Room for the sender's own nick, user and host, which the server adds when it passes a line on */
#define LINE_BYTES 400
#define CTCP_MARK "\001"


static char *get_own_nick(CrimsonNetwork *network) {
	IrcState *state = get_network_state(network);
	return g_strdup(state && state->nick ? state->nick : network->session->record->nick);
}


/* Cuts text into pieces that fit a line, between words where it can and never inside a character */
static GPtrArray *split_long_text(const char *text, gsize limit) {
	GPtrArray *pieces = g_ptr_array_new_with_free_func(g_free);
	const char *start = text;
	while (strlen(start) > limit) {
		const char *end = start + limit;
		while (end > start && (*end & 0xc0) == 0x80)
			end--;
		const char *space = end;
		while (space > start && *space != ' ')
			space--;
		if (space > start)
			end = space + 1;
		g_ptr_array_add(pieces, g_strndup(start, (gsize)(end - start)));
		start = end;
	}
	g_ptr_array_add(pieces, g_strdup(start));
	return pieces;
}


static void track_pending(CrimsonRoom *room, CrimsonMessage *line) {
	NetworkWork *work = get_network_work(room->network);
	if (line->label)
		g_hash_table_insert(work->pending, g_strdup(line->label), g_object_ref(line));
	insert_room_message(room, line);
	room->read_time = MAX(room->read_time, line->time);
	count_unread(room);
	announce_room_change(room);
	save_chat_cache_later(room->network->session->chat);
}


/* Shows a sent message at once, waiting for the server's echo to confirm it where the server echoes */
static CrimsonMessage *add_own_message(CrimsonRoom *room, MessageKind kind, const char *text,
	CrimsonMessage *reply_to) {
	CrimsonNetwork *network = room->network;
	g_autofree char *nick = get_own_nick(network);
	CrimsonMessage *line = create_message(kind, nick, text, g_get_real_time());
	line->is_own = TRUE;
	line->is_fresh = TRUE;
	line->is_pending = can_network(network, "echo-message");
	line->reply_to = reply_to && reply_to->id ? g_strdup(reply_to->id) : NULL;
	if (line->is_pending && can_network(network, "labeled-response"))
		line->label = create_irc_identifier();
	return line;
}


static IrcMessage *create_text_line(const char *command, const char *target, const char *text,
	CrimsonMessage *line) {
	IrcMessage *message = create_irc_message(command, target, text, NULL);
	if (line->label)
		set_irc_tag(message, "label", line->label);
	if (line->reply_to)
		set_irc_tag(message, "+reply", line->reply_to);
	return message;
}


/* A multiline batch keeps line breaks and long lines together as one message */
static void send_multiline(CrimsonRoom *room, const char *command, char **lines, CrimsonMessage *line) {
	IrcClient *client = room->network->client;
	g_autofree char *reference = create_irc_identifier();
	g_autofree char *opening = g_strconcat("+", reference, NULL);
	g_autofree char *closing = g_strconcat("-", reference, NULL);
	g_autoptr(IrcMessage) start = create_irc_message("BATCH", opening, "draft/multiline", room->name, NULL);
	if (line->label)
		set_irc_tag(start, "label", line->label);
	if (line->reply_to)
		set_irc_tag(start, "+reply", line->reply_to);
	send_irc_client_message(client, start);
	for (int index = 0; lines[index]; index++) {
		g_autoptr(GPtrArray) pieces = split_long_text(lines[index], LINE_BYTES - strlen(room->name));
		for (guint piece = 0; piece < pieces->len; piece++) {
			g_autoptr(IrcMessage) message = create_irc_message(command, room->name, g_ptr_array_index(pieces, piece),
				NULL);
			set_irc_tag(message, "batch", reference);
			if (piece > 0)
				set_irc_tag(message, "draft/multiline-concat", NULL);
			send_irc_client_message(client, message);
		}
	}
	g_autoptr(IrcMessage) end = create_irc_message("BATCH", closing, NULL);
	send_irc_client_message(client, end);
}


static void send_text_as(CrimsonRoom *room, const char *command, const char *text, gboolean is_action,
	CrimsonMessage *reply_to) {
	CrimsonNetwork *network = room->network;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	guint count = g_strv_length(lines);
	while (count > 1 && !*g_strstrip(lines[count - 1])) {
		g_free(lines[--count]);
		lines[count] = NULL;
	}
	MessageKind kind = is_action ? MESSAGE_ACTION : g_str_equal(command, "NOTICE") ? MESSAGE_NOTICE : MESSAGE_TEXT;
	gboolean is_long = count > 1 || strlen(text) > LINE_BYTES - strlen(room->name);
	if (is_long && !is_action && can_network(network, "draft/multiline") && can_network(network, "batch")) {
		g_autoptr(CrimsonMessage) line = add_own_message(room, kind, text, reply_to);
		send_multiline(room, command, lines, line);
		track_pending(room, line);
		return;
	}
	for (guint index = 0; index < count; index++) {
		g_autoptr(GPtrArray) pieces = split_long_text(lines[index], LINE_BYTES - strlen(room->name));
		for (guint piece = 0; piece < pieces->len; piece++) {
			const char *body = g_ptr_array_index(pieces, piece);
			if (!*body)
				continue;
			g_autoptr(CrimsonMessage) line = add_own_message(room, kind, body, index == 0 && piece == 0 ? reply_to
				: NULL);
			g_autofree char *sent = is_action ? g_strconcat(CTCP_MARK "ACTION ", body, CTCP_MARK, NULL)
				: g_strdup(body);
			g_autoptr(IrcMessage) message = create_text_line(command, room->name, sent, line);
			send_irc_client_message(network->client, message);
			track_pending(room, line);
		}
	}
}


static void send_raw(CrimsonNetwork *network, const char *line) {
	g_autoptr(IrcMessage) message = parse_irc_line(line);
	if (message && network->client)
		send_irc_client_message(network->client, message);
}


/* Splits "word rest" into the first word and what follows it */
static char *take_argument(const char **rest) {
	while (**rest == ' ')
		(*rest)++;
	const char *end = strchr(*rest, ' ');
	char *word = end ? g_strndup(*rest, (gsize)(end - *rest)) : g_strdup(*rest);
	*rest = end ? end + 1 : *rest + strlen(*rest);
	while (**rest == ' ')
		(*rest)++;
	return word;
}


static void message_someone(CrimsonNetwork *network, const char *rest, const char *command) {
	g_autofree char *target = take_argument(&rest);
	if (!*target)
		return;
	IrcState *state = get_network_state(network);
	/* Talking to a service happens in the server's messages, where its answers arrive */
	gboolean is_service = is_service_nick(target);
	CrimsonRoom *room = is_service || (state && is_irc_channel_name(state, target)) ? find_room(network, target)
		: open_private_room(network, target);
	if (!room) {
		g_autofree char *line = g_strdup_printf("%s %s :%s", command, target, rest);
		send_raw(network, line);
		if (is_service)
			network->session->chat->events.room_opened(network->server_room, network->session->chat->data);
		return;
	}
	if (*rest)
		send_text_as(room, command, rest, FALSE, NULL);
	if (g_str_equal(command, "PRIVMSG"))
		network->session->chat->events.room_opened(room, network->session->chat->data);
}


static gboolean run_room_command(CrimsonRoom *room, const char *name, const char *rest) {
	CrimsonNetwork *network = room->network;
	gboolean is_channel = room->kind == ROOM_CHANNEL;
	if (g_str_equal(name, "me")) {
		if (room->kind != ROOM_SERVER)
			send_text_as(room, "PRIVMSG", rest, TRUE, NULL);
	} else if (g_str_equal(name, "topic") && is_channel) {
		send_irc_client_command(network->client, "TOPIC", room->name, *rest ? rest : NULL, NULL);
	} else if ((g_str_equal(name, "part") || g_str_equal(name, "leave")) && !*rest) {
		leave_room(room);
	} else if (g_str_equal(name, "close")) {
		leave_room(room);
	} else if (g_str_equal(name, "kick") && is_channel) {
		g_autofree char *nick = take_argument(&rest);
		send_irc_client_command(network->client, "KICK", room->name, nick, *rest ? rest : NULL, NULL);
	} else if (g_str_equal(name, "invite") && is_channel && !strchr(rest, ' ')) {
		send_irc_client_command(network->client, "INVITE", rest, room->name, NULL);
	} else if (g_str_equal(name, "whois")) {
		get_network_work(network)->whois_room = room;
		send_irc_client_command(network->client, "WHOIS", rest, NULL);
	} else {
		return FALSE;
	}
	return TRUE;
}


static void run_network_command(CrimsonRoom *room, const char *name, const char *rest) {
	CrimsonNetwork *network = room->network;
	if (g_str_equal(name, "join") || g_str_equal(name, "j")) {
		g_autofree char *channel = take_argument(&rest);
		join_channel(network, channel, *rest ? rest : NULL);
	} else if (g_str_equal(name, "part") || g_str_equal(name, "leave")) {
		g_autofree char *channel = take_argument(&rest);
		CrimsonRoom *target = find_room(network, channel);
		if (target)
			leave_room(target);
		else
			send_irc_client_command(network->client, "PART", channel, *rest ? rest : NULL, NULL);
	} else if (g_str_equal(name, "msg") || g_str_equal(name, "query")) {
		message_someone(network, rest, "PRIVMSG");
	} else if (g_str_equal(name, "notice")) {
		message_someone(network, rest, "NOTICE");
	} else if (g_str_equal(name, "nick")) {
		send_irc_client_command(network->client, "NICK", rest, NULL);
	} else if (g_str_equal(name, "away")) {
		send_irc_client_command(network->client, "AWAY", *rest ? rest : TEXT_AWAY_DEFAULT, NULL);
	} else if (g_str_equal(name, "back")) {
		send_irc_client_command(network->client, "AWAY", NULL);
	} else if (g_str_equal(name, "quote") || g_str_equal(name, "raw")) {
		send_raw(network, rest);
	} else {
		g_autofree char *upper = g_ascii_strup(name, -1);
		g_autofree char *line = g_strconcat(upper, *rest ? " " : "", rest, NULL);
		send_raw(network, line);
	}
}


static void run_command(CrimsonRoom *room, const char *line) {
	const char *rest = line;
	g_autofree char *word = take_argument(&rest);
	g_autofree char *name = g_ascii_strdown(word, -1);
	if (!run_room_command(room, name, rest))
		run_network_command(room, name, rest);
}


/* CLIENTTAGDENY lists the client tags a server drops, with * for all of them and -name for exceptions */
gboolean allows_client_tag(CrimsonNetwork *network, const char *name) {
	const char *denied = get_irc_support(get_network_state(network), "CLIENTTAGDENY");
	if (!denied)
		return TRUE;
	g_auto(GStrv) entries = g_strsplit(denied, ",", -1);
	g_autofree char *exception = g_strconcat("-", name, NULL);
	gboolean is_denied = FALSE;
	for (int index = 0; entries[index]; index++) {
		if (g_str_equal(entries[index], "*") || g_str_equal(entries[index], name))
			is_denied = TRUE;
		if (g_str_equal(entries[index], exception))
			return TRUE;
	}
	return !is_denied;
}


void send_room_text(CrimsonRoom *room, const char *text, CrimsonMessage *reply_to) {
	if (!room->network->client || !*text)
		return;
	if (text[0] == '/' && text[1] != '/') {
		g_autofree char *command = g_strdup(text + 1);
		g_strchomp(command);
		run_command(room, command);
		return;
	}
	const char *body = text[0] == '/' ? text + 1 : text;
	/* A server that strips the reply tag would leave the reply without context, so it names the person instead, the
	 * way replies have always looked on IRC */
	g_autofree char *named = NULL;
	if (reply_to && reply_to->sender && room->kind == ROOM_CHANNEL && !allows_client_tag(room->network, "reply")) {
		g_autofree char *prefix = g_strdup_printf("%s: ", reply_to->sender);
		if (!g_str_has_prefix(body, prefix))
			body = named = g_strconcat(prefix, body, NULL);
	}
	if (room->kind == ROOM_SERVER)
		send_raw(room->network, body);
	else
		send_text_as(room, "PRIVMSG", body, FALSE, reply_to);
}


void send_reaction(CrimsonRoom *room, CrimsonMessage *message, const char *reaction) {
	CrimsonNetwork *network = room->network;
	if (!message->id || !can_network(network, "message-tags") || !allows_client_tag(network, "draft/react"))
		return;
	g_autofree char *nick = get_own_nick(network);
	gboolean is_removing = has_reacted(message, reaction, nick);
	g_autoptr(IrcMessage) tag_message = create_irc_message("TAGMSG", room->name, NULL);
	set_irc_tag(tag_message, "+reply", message->id);
	set_irc_tag(tag_message, is_removing ? "+draft/unreact" : "+draft/react", reaction);
	send_irc_client_message(network->client, tag_message);
	/* The server's echo applies the reaction, and without echoes it shows at once */
	if (!can_network(network, "echo-message") && change_reaction(message, reaction, nick, !is_removing))
		announce_message_change(message);
}


void redact_message(CrimsonRoom *room, CrimsonMessage *message) {
	if (!message->id || !can_network(room->network, "draft/message-redaction"))
		return;
	send_irc_client_command(room->network->client, "REDACT", room->name, message->id, NULL);
}




void send_typing(CrimsonRoom *room, const char *state) {
	CrimsonNetwork *network = room->network;
	if (!get_settings()->sends_typing || room->kind == ROOM_SERVER || !can_network(network, "message-tags")
		|| !allows_client_tag(network, "typing"))
		return;
	g_autoptr(IrcMessage) message = create_irc_message("TAGMSG", room->name, NULL);
	set_irc_tag(message, "+typing", state);
	send_irc_client_message(network->client, message);
}


void mark_room_read(CrimsonRoom *room) {
	CrimsonMessage *last = get_last_message(room);
	if (!last || last->time <= room->read_time) {
		if (room->unread_count) {
			count_unread(room);
			announce_room_change(room);
		}
		return;
	}
	room->read_time = last->time;
	count_unread(room);
	announce_room_change(room);
	save_chat_cache_later(room->network->session->chat);
	if (room->kind == ROOM_SERVER || !get_settings()->sends_read_markers || !can_network(room->network,
		"draft/read-marker"))
		return;
	g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(last->time);
	g_autofree char *moment = format_irc_time(time);
	g_autofree char *value = g_strconcat("timestamp=", moment, NULL);
	send_irc_client_command(room->network->client, "MARKREAD", room->name, value, NULL);
}


void join_channel(CrimsonNetwork *network, const char *name, const char *key) {
	if (!name || !*name)
		return;
	IrcState *state = get_network_state(network);
	g_autofree char *channel = state && !is_irc_channel_name(state, name) ? g_strconcat("#", name, NULL)
		: g_strdup(name);
	CrimsonRoom *room = find_room(network, channel);
	if (room && room->is_joined) {
		network->session->chat->events.room_opened(room, network->session->chat->data);
		return;
	}
	NetworkWork *work = get_network_work(network);
	g_free(work->opening_room);
	work->opening_room = g_strdup(channel);
	send_irc_client_command(network->client, "JOIN", channel, key, NULL);
}


void leave_room(CrimsonRoom *room) {
	CrimsonNetwork *network = room->network;
	if (room->kind == ROOM_SERVER)
		return;
	g_autoptr(CrimsonRoom) kept = g_object_ref(room);
	if (room->kind == ROOM_CHANNEL && room->is_joined)
		send_irc_client_command(network->client, "PART", room->name, NULL);
	else if (room->kind == ROOM_PRIVATE && get_irc_support(get_network_state(network), "MONITOR"))
		send_irc_client_command(network->client, "MONITOR", "-", room->name, NULL);
	if (room->kind == ROOM_PRIVATE)
		note_deleted_chat(network->key, room->name, g_get_real_time());
	room->is_joined = FALSE;
	remove_room(network, room);
	save_network_channels(network);
	save_chat_cache_later(network->session->chat);
}


CrimsonRoom *open_private_room(CrimsonNetwork *network, const char *nick) {
	gboolean is_new = !find_room(network, nick);
	CrimsonRoom *room = ensure_room(network, ROOM_PRIVATE, nick);
	if (!is_new)
		return room;
	IrcState *state = get_network_state(network);
	if (state && get_irc_support(state, "MONITOR"))
		send_irc_client_command(network->client, "MONITOR", "+", nick, NULL);
	request_latest_history(room);
	save_chat_cache_later(network->session->chat);
	return room;
}
