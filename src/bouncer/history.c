#include <string.h>

#include "bouncer.h"

#define TIMESTAMP_PREFIX "timestamp="
#define MESSAGE_ID_PREFIX "msgid="
/* A search answers with this many messages at most, picked from this many lines that hold the words */
#define SEARCH_RESULTS 50
#define SEARCH_SCANNED 2000
#define SEARCH_BATCH "crimson-chat/search"


static gboolean is_reaction(const IrcMessage *message) {
	return has_irc_tag(message, "+draft/react") || has_irc_tag(message, "+draft/unreact");
}


/* Which kind of history line a message makes, or zero for one that is not kept */
static HistoryKind classify_message(const IrcMessage *message) {
	const char *command = message->command;
	if (!message->source || !strchr(message->source, '!'))
		return 0;
	if (g_str_equal(command, "PRIVMSG") || g_str_equal(command, "NOTICE"))
		return HISTORY_MESSAGE;
	if (g_str_equal(command, "TAGMSG"))
		return is_reaction(message) ? HISTORY_TAG_MESSAGE : 0;
	if (g_str_equal(command, "REDACT"))
		return HISTORY_REDACTION;
	static const char *const EVENTS[] = {"JOIN", "PART", "KICK", "TOPIC", "MODE", "QUIT", "NICK", NULL};
	return g_strv_contains(EVENTS, command) ? HISTORY_EVENT : 0;
}


/* The conversation a message belongs to, which for a private message is the other person */
static char *find_conversation(IrcState *state, const IrcMessage *message) {
	const char *target = get_irc_parameter(message, 0);
	if (!target || is_irc_channel_name(state, target))
		return g_strdup(target);
	g_autofree char *sender = get_source_nick(message->source);
	return is_own_nick(state, sender) ? g_strdup(target) : g_steal_pointer(&sender);
}


void store_upstream_message(Upstream *upstream, IrcMessage *message, GPtrArray *affected) {
	HistoryKind kind = classify_message(message);
	if (!kind)
		return;
	/* Every kept line has an id, so pages that overlap never show the same join twice */
	if (!get_irc_tag(message, "msgid")) {
		g_autofree char *message_id = create_irc_identifier();
		set_irc_tag(message, "msgid", message_id);
	}
	IrcState *state = get_irc_client_state(upstream->client);
	g_autofree char *line = format_irc_line(message);
	const char *time = get_irc_tag(message, "time");
	const char *message_id = get_irc_tag(message, "msgid");
	Database *database = upstream->user->bouncer->database;
	gboolean is_spread = g_str_equal(message->command, "QUIT") || g_str_equal(message->command, "NICK");
	if (is_spread) {
		for (guint index = 0; affected && index < affected->len; index++) {
			const char *channel = g_ptr_array_index(affected, index);
			g_autofree char *folded = fold_state_name(state, channel);
			store_history(database, upstream->record.id, channel, folded, time, message_id, kind, line);
		}
		return;
	}
	g_autofree char *target = find_conversation(state, message);
	if (!target)
		return;
	g_autofree char *folded = fold_state_name(state, target);
	store_history(database, upstream->record.id, target, folded, time, message_id, kind, line);
}


/* Replies that only fill in what an app keeps itself, such as name lists, profiles and room settings, which the
 * server's chat never shows */
static const char *const QUIET_REPLIES[] = {
	"311", "312", "313", "314", "315", "317", "318", "319", "321", "322", "323", "324", "329", "330", "333", "338",
	"346", "347", "348", "349", "352", "353", "354", "366", "367", "368", "369", "378", "379", "671", "761", "766",
	"770", "771", "772", "774", NULL,
};


gboolean is_server_line(const IrcMessage *message, gboolean is_answer) {
	const char *command = message->command;
	/* The message of the day is kept whole once it ends, rather than line by line */
	gboolean is_motd = g_str_equal(command, "375") || g_str_equal(command, "372") || g_str_equal(command, "376");
	if (g_ascii_isdigit(*command))
		return is_answer && !is_motd && !g_strv_contains(QUIET_REPLIES, command);
	gboolean is_text = g_str_equal(command, "NOTICE") || g_str_equal(command, "PRIVMSG");
	return is_text && message->source && !strchr(message->source, '!');
}


void store_server_line(Upstream *upstream, IrcMessage *message) {
	if (!get_irc_tag(message, "msgid")) {
		g_autofree char *message_id = create_irc_identifier();
		set_irc_tag(message, "msgid", message_id);
	}
	g_autofree char *line = format_irc_line(message);
	store_history(upstream->user->bouncer->database, upstream->record.id, SERVER_TARGET, SERVER_TARGET,
		get_irc_tag(message, "time"), get_irc_tag(message, "msgid"), HISTORY_MESSAGE, line);
}


/* The time in the exact form history is stored in, so text order matches time order */
static char *normalize_time(const char *text) {
	g_autoptr(GDateTime) time = parse_irc_time(text);
	return time ? format_irc_time(time) : NULL;
}


/* Reads timestamp= or msgid=, where a timestamp sits before every message of its moment or after them all */
static gboolean read_position(Downstream *downstream, Upstream *upstream, const char *text, gboolean is_late,
	HistoryPosition *position) {
	*position = (HistoryPosition){0};
	if (!text)
		return FALSE;
	if (g_str_has_prefix(text, TIMESTAMP_PREFIX)) {
		position->time = normalize_time(text + strlen(TIMESTAMP_PREFIX));
		position->id = is_late ? G_MAXINT64 : 0;
		return position->time != NULL;
	}
	if (g_str_has_prefix(text, MESSAGE_ID_PREFIX))
		return find_history_message(downstream->bouncer->database, upstream->record.id,
			text + strlen(MESSAGE_ID_PREFIX), position);
	return FALSE;
}


static int list_wanted_kinds(Downstream *downstream) {
	int kinds = HISTORY_MESSAGE;
	if (has_downstream_cap(downstream, "message-tags"))
		kinds |= HISTORY_TAG_MESSAGE;
	if (has_downstream_cap(downstream, "draft/message-redaction"))
		kinds |= HISTORY_REDACTION;
	if (has_downstream_cap(downstream, "draft/event-playback"))
		kinds |= HISTORY_EVENT;
	return kinds;
}


static void send_bouncer_line(Downstream *downstream, Upstream *upstream, IrcMessage *message, const char *batch) {
	g_free(message->source);
	message->source = g_strdup(BOUNCER_NAME);
	if (batch)
		set_irc_tag(message, "batch", batch);
	send_to_downstream(downstream, upstream, message);
}


/* Sends history lines in a batch, marked when none are left beyond them */
static void send_history(Downstream *downstream, Upstream *upstream, const char *type, const char *target,
	GPtrArray *lines, gboolean is_end) {
	gboolean uses_batch = has_downstream_cap(downstream, "batch");
	g_autofree char *reference = create_irc_identifier();
	g_autofree char *opening = g_strconcat("+", reference, NULL);
	g_autofree char *closing = g_strconcat("-", reference, NULL);
	if (uses_batch) {
		g_autoptr(IrcMessage) start = create_irc_message("BATCH", opening, type, target, NULL);
		if (is_end)
			set_irc_tag(start, "draft/chathistory-end", NULL);
		send_bouncer_line(downstream, upstream, start, NULL);
	}
	for (guint index = 0; index < lines->len; index++) {
		g_autoptr(IrcMessage) message = parse_irc_line(g_ptr_array_index(lines, index));
		if (!message)
			continue;
		if (uses_batch)
			set_irc_tag(message, "batch", reference);
		send_to_downstream(downstream, upstream, message);
	}
	if (uses_batch) {
		g_autoptr(IrcMessage) end = create_irc_message("BATCH", closing, NULL);
		send_bouncer_line(downstream, upstream, end, NULL);
	}
}


static void fail_history(Downstream *downstream, Upstream *upstream, const char *code, const char *subcommand,
	const char *detail) {
	g_autoptr(IrcMessage) message = create_irc_message("FAIL", "CHATHISTORY", code, subcommand ? subcommand : "*",
		detail, NULL);
	send_bouncer_line(downstream, upstream, message, NULL);
}


static void answer_targets(Downstream *downstream, Upstream *upstream, const IrcMessage *message, int limit) {
	g_autofree char *from = NULL, *to = NULL;
	const char *first = get_irc_parameter(message, 1), *second = get_irc_parameter(message, 2);
	if (first && g_str_has_prefix(first, TIMESTAMP_PREFIX))
		from = normalize_time(first + strlen(TIMESTAMP_PREFIX));
	if (second && g_str_has_prefix(second, TIMESTAMP_PREFIX))
		to = normalize_time(second + strlen(TIMESTAMP_PREFIX));
	if (!from || !to) {
		fail_history(downstream, upstream, "INVALID_PARAMS", "TARGETS", "Invalid timestamp");
		return;
	}
	g_autoptr(GPtrArray) targets = query_history_targets(downstream->bouncer->database,
		upstream->record.id, from, to, limit);
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < targets->len; index++) {
		HistoryTarget *target = g_ptr_array_index(targets, index);
		/* The server's own chat is always there, so it is no conversation to list */
		if (g_str_equal(target->target, SERVER_TARGET))
			continue;
		g_autoptr(IrcMessage) line = create_irc_message("CHATHISTORY", "TARGETS", target->target, target->time,
			NULL);
		line->source = g_strdup(BOUNCER_NAME);
		g_ptr_array_add(lines, format_irc_line(line));
	}
	send_history(downstream, upstream, "draft/chathistory-targets", NULL, lines, FALSE);
}


static void clear_position(HistoryPosition *position) {
	g_clear_pointer(&position->time, g_free);
}


/* Messages on both sides of a point, half before it and the rest from it onwards */
static GPtrArray *query_around(Downstream *downstream, Upstream *upstream, const char *folded,
	const HistoryPosition *point, int limit, int kinds) {
	Database *database = downstream->bouncer->database;
	gint64 network_id = upstream->record.id;
	GPtrArray *lines = query_history(database, network_id, folded, NULL, point, TRUE, limit / 2, kinds, NULL);
	HistoryPosition from = {point->time, point->id == G_MAXINT64 || point->id == 0 ? -1 : point->id - 1};
	g_autoptr(GPtrArray) later = query_history(database, network_id, folded, &from, NULL, FALSE,
		limit - limit / 2, kinds, NULL);
	for (guint index = 0; index < later->len; index++)
		g_ptr_array_add(lines, g_strdup(g_ptr_array_index(later, index)));
	return lines;
}


static GPtrArray *run_history_query(Downstream *downstream, Upstream *upstream, const char *subcommand,
	const char *folded, const IrcMessage *message, int limit, gboolean *is_valid, gboolean *is_end) {
	Database *database = downstream->bouncer->database;
	gint64 network_id = upstream->record.id;
	int kinds = list_wanted_kinds(downstream);
	const char *first = get_irc_parameter(message, 2), *second = get_irc_parameter(message, 3);
	HistoryPosition early = {0}, late = {0};
	*is_valid = TRUE;
	GPtrArray *lines = NULL;
	if (g_ascii_strcasecmp(subcommand, "LATEST") == 0) {
		gboolean is_bounded = g_strcmp0(first, "*") != 0;
		*is_valid = !is_bounded || read_position(downstream, upstream, first, TRUE, &late);
		lines = *is_valid ? query_history(database, network_id, folded, is_bounded ? &late : NULL, NULL, TRUE, limit,
			kinds, is_end) : NULL;
	} else if (g_ascii_strcasecmp(subcommand, "BEFORE") == 0) {
		*is_valid = read_position(downstream, upstream, first, FALSE, &early);
		lines = *is_valid ? query_history(database, network_id, folded, NULL, &early, TRUE, limit, kinds, is_end)
			: NULL;
	} else if (g_ascii_strcasecmp(subcommand, "AFTER") == 0) {
		*is_valid = read_position(downstream, upstream, first, TRUE, &late);
		lines = *is_valid ? query_history(database, network_id, folded, &late, NULL, FALSE, limit, kinds, is_end)
			: NULL;
	} else if (g_ascii_strcasecmp(subcommand, "AROUND") == 0) {
		*is_valid = read_position(downstream, upstream, first, FALSE, &early);
		lines = *is_valid ? query_around(downstream, upstream, folded, &early, limit, kinds) : NULL;
	} else if (g_ascii_strcasecmp(subcommand, "BETWEEN") == 0) {
		*is_valid = read_position(downstream, upstream, first, TRUE, &late)
			&& read_position(downstream, upstream, second, FALSE, &early);
		gboolean is_backwards = *is_valid && g_strcmp0(late.time, early.time) > 0;
		if (is_backwards) {
			clear_position(&late);
			clear_position(&early);
			read_position(downstream, upstream, first, FALSE, &early);
			read_position(downstream, upstream, second, TRUE, &late);
		}
		lines = *is_valid ? query_history(database, network_id, folded, &late, &early, is_backwards, limit, kinds,
			is_end) : NULL;
	} else {
		*is_valid = FALSE;
	}
	clear_position(&early);
	clear_position(&late);
	return lines;
}


void answer_history_request(Downstream *downstream, Upstream *upstream, const IrcMessage *message) {
	const char *subcommand = get_irc_parameter(message, 0);
	guint count = count_irc_parameters(message);
	const char *limit_text = count > 1 ? get_irc_parameter(message, count - 1) : NULL;
	int limit = limit_text ? (int)g_ascii_strtoll(limit_text, NULL, 10) : 0;
	if (!subcommand || limit <= 0 || count < 3) {
		fail_history(downstream, upstream, "INVALID_PARAMS", subcommand, "Insufficient parameters");
		return;
	}
	limit = MIN(limit, HISTORY_PAGE_LIMIT);
	if (g_ascii_strcasecmp(subcommand, "TARGETS") == 0) {
		answer_targets(downstream, upstream, message, limit);
		return;
	}
	const char *target = get_irc_parameter(message, 1);
	IrcState *state = get_irc_client_state(upstream->client);
	g_autofree char *folded = fold_state_name(state, target);
	gboolean is_valid = FALSE, is_end = FALSE;
	g_autoptr(GPtrArray) lines = run_history_query(downstream, upstream, subcommand, folded, message, limit, &is_valid,
		&is_end);
	if (!is_valid || !lines) {
		fail_history(downstream, upstream, "INVALID_PARAMS", subcommand, "Invalid message reference");
		return;
	}
	send_history(downstream, upstream, "chathistory", target, lines, is_end);
}


/* Whether what a person wrote holds the words, ignoring case, which the database only matched in the whole line */
static gboolean says_words(const char *line, const char *folded_words) {
	g_autoptr(IrcMessage) message = parse_irc_line(line);
	const char *text = message ? get_last_irc_parameter(message) : NULL;
	g_autofree char *folded = text ? g_utf8_casefold(text, -1) : NULL;
	return folded && strstr(folded, folded_words);
}


/* CRIMSON SEARCH with a conversation, or * for all of them, and the words, answers with the newest messages that
 * say them in a batch naming the same two, so the app can tell which search it answers */
void answer_search(Downstream *downstream, Upstream *upstream, const IrcMessage *message) {
	const char *target = get_irc_parameter(message, 1);
	const char *words = get_irc_parameter(message, 2);
	if (!target || !words || !*words) {
		g_autoptr(IrcMessage) failure = create_irc_message("FAIL", "CRIMSON", "NEED_MORE_PARAMS", "SEARCH",
			"Missing parameters", NULL);
		send_bouncer_line(downstream, upstream, failure, NULL);
		return;
	}
	IrcState *state = get_irc_client_state(upstream->client);
	g_autofree char *folded = g_str_equal(target, "*") ? NULL : fold_state_name(state, target);
	g_autoptr(GPtrArray) candidates = search_history(downstream->bouncer->database, upstream->record.id, folded,
		words, SEARCH_SCANNED);
	g_autofree char *folded_words = g_utf8_casefold(words, -1);
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < candidates->len && lines->len < SEARCH_RESULTS; index++) {
		const char *line = g_ptr_array_index(candidates, index);
		if (says_words(line, folded_words))
			g_ptr_array_add(lines, g_strdup(line));
	}
	g_autofree char *reference = create_irc_identifier();
	g_autofree char *opening = g_strconcat("+", reference, NULL);
	g_autofree char *closing = g_strconcat("-", reference, NULL);
	g_autoptr(IrcMessage) start = create_irc_message("BATCH", opening, SEARCH_BATCH, target, words, NULL);
	send_bouncer_line(downstream, upstream, start, NULL);
	for (guint index = 0; index < lines->len; index++) {
		g_autoptr(IrcMessage) line = parse_irc_line(g_ptr_array_index(lines, index));
		if (!line)
			continue;
		set_irc_tag(line, "batch", reference);
		send_to_downstream(downstream, upstream, line);
	}
	g_autoptr(IrcMessage) end = create_irc_message("BATCH", closing, NULL);
	send_bouncer_line(downstream, upstream, end, NULL);
}


void send_read_marker(Downstream *downstream, Upstream *upstream, const char *target, const char *time) {
	if (!has_downstream_cap(downstream, "draft/read-marker"))
		return;
	g_autofree char *value = time ? g_strconcat(TIMESTAMP_PREFIX, time, NULL) : g_strdup("*");
	g_autoptr(IrcMessage) message = create_irc_message("MARKREAD", target, value, NULL);
	send_bouncer_line(downstream, upstream, message, NULL);
}


static void share_read_marker(Downstream *origin, Upstream *upstream, const char *target, const char *time) {
	for (GList *item = origin->bouncer->downstreams; item; item = item->next) {
		Downstream *downstream = item->data;
		if (downstream->user == origin->user && downstream->is_registered)
			send_read_marker(downstream, upstream, target, time);
	}
}


void answer_read_marker(Downstream *downstream, Upstream *upstream, const IrcMessage *message) {
	const char *target = get_irc_parameter(message, 0);
	const char *value = get_irc_parameter(message, 1);
	if (!target) {
		g_autoptr(IrcMessage) failure = create_irc_message("FAIL", "MARKREAD", "NEED_MORE_PARAMS",
			"Missing parameters", NULL);
		send_bouncer_line(downstream, upstream, failure, NULL);
		return;
	}
	IrcState *state = get_irc_client_state(upstream->client);
	g_autofree char *folded = fold_state_name(state, target);
	Database *database = downstream->bouncer->database;
	gint64 network_id = upstream->record.id;
	g_autofree char *stored = load_read_marker(database, network_id, folded);
	g_autofree char *wanted = value && g_str_has_prefix(value, TIMESTAMP_PREFIX)
		? normalize_time(value + strlen(TIMESTAMP_PREFIX)) : NULL;
	if (value && !wanted) {
		g_autoptr(IrcMessage) failure = create_irc_message("FAIL", "MARKREAD", "INVALID_PARAMS", value,
			"Invalid parameters", NULL);
		send_bouncer_line(downstream, upstream, failure, NULL);
		return;
	}
	/* A read marker only ever moves forward, and an older one gets the stored value back */
	if (!wanted || (stored && g_strcmp0(wanted, stored) <= 0)) {
		send_read_marker(downstream, upstream, target, stored);
		return;
	}
	save_read_marker(database, network_id, folded, wanted);
	share_read_marker(downstream, upstream, target, wanted);
}
