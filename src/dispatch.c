#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "modes.h"
#include "notifications.h"
#include "questions.h"
#include "replies.h"
#include "settings.h"

#define TYPING_ACTIVE_SECONDS 6
#define TYPING_PAUSED_SECONDS 30
#define TARGETS_DAYS 30
#define SEARCH_BATCH "crimson-chat/search"
/* The conversation a bouncer keeps the server's own chat under, and the tag its kept message of the day is in */
#define SERVER_TARGET "*"
#define WELCOME_MESSAGE_TAG "crimson-chat/motd"
/* Starts each line of the message of the day plain and in a fixed width font */
#define FIXED_WIDTH_LINE "\x0f\x11"
/* How much of the user's message the list quotes when someone reacts to it */
#define REACTION_EXCERPT_CHARS 30
#define TARGETS_LIMIT 100
#define CTCP_MARK '\001'

/* A history batch being received for one room */
typedef struct {
	CrimsonRoom *room;
	gboolean is_end, is_batching;
	guint lines;
} HistoryLoad;

/* Replies to WHOIS, shown in the room the question was asked from */
static const char *const WHOIS_REPLIES[] = {
	"276", "301", "311", "312", "313", "317", "318", "319", "320", "330", "335", "338", "378", "379", "671", NULL,
};

static void handle_message_in(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected,
	HistoryLoad *load);


gint64 read_message_time(const IrcMessage *message) {
	g_autoptr(GDateTime) time = parse_irc_time(get_irc_tag(message, "time"));
	return time ? g_date_time_to_unix_usec(time) : g_get_real_time();
}


/* A line the bouncer keeps carries its id, so the same line loaded again from history shows once */
static void add_status(CrimsonRoom *room, const IrcMessage *message, const char *text) {
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_STATUS, NULL, text, read_message_time(message));
	line->id = g_strdup(get_irc_tag(message, "msgid"));
	insert_room_message(room, line);
	announce_room_change(room);
}


/* The server's answer to something the user sent brings its messages into the room list, and the first line of it
 * opens them so the whole answer shows, unless the user sent it from there */
static void show_answer(CrimsonNetwork *network, Question *question) {
	CrimsonRoom *room = network->server_room;
	if (!room->is_listed) {
		room->is_listed = TRUE;
		save_chat_cache_later(network->session->chat);
	}
	count_unread(room);
	announce_room_change(room);
	if (!question->has_shown_answer && !question->was_typed_in_server)
		network->session->chat->events.room_opened(room, network->session->chat->data);
	question->has_shown_answer = TRUE;
}


/* What the server itself says goes with its messages, as news when it answers the user */
static void add_server_line(CrimsonNetwork *network, const IrcMessage *message, const char *text) {
	Question *question = find_question(network, message);
	if (!question) {
		add_status(network->server_room, message, text);
		return;
	}
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_NOTICE, NULL, text, read_message_time(message));
	line->id = g_strdup(get_irc_tag(message, "msgid"));
	line->arrival_time = g_get_monotonic_time();
	insert_room_message(network->server_room, line);
	show_answer(network, question);
}


static gboolean is_word_character(gunichar character) {
	return g_unichar_isalnum(character) || character == '_' || character == '-' || character == '[' || character == ']'
		|| character == '\\' || character == '`' || character == '^' || character == '{' || character == '}'
		|| character == '|';
}


/* Whether a word appears on its own, ignoring case */
static gboolean contains_word(const char *text, const char *word) {
	if (!word || !*word)
		return FALSE;
	g_autofree char *haystack = g_utf8_casefold(text, -1);
	g_autofree char *needle = g_utf8_casefold(word, -1);
	gsize length = strlen(needle);
	for (const char *found = strstr(haystack, needle); found; found = strstr(found + 1, needle)) {
		gboolean starts = found == haystack || !is_word_character(g_utf8_get_char(g_utf8_prev_char(found)));
		gboolean ends = !found[length] || !is_word_character(g_utf8_get_char(found + length));
		if (starts && ends)
			return TRUE;
	}
	return FALSE;
}


static gboolean is_mention(CrimsonNetwork *network, const char *text) {
	IrcState *state = get_network_state(network);
	if (state && contains_word(text, state->nick))
		return TRUE;
	g_auto(GStrv) words = g_strsplit(get_settings()->highlight_words ? get_settings()->highlight_words : "", ",", -1);
	for (int index = 0; words[index]; index++) {
		if (contains_word(text, g_strstrip(words[index])))
			return TRUE;
	}
	return FALSE;
}


/* A channel target may start with status symbols, as in @#channel for its operators only */
static const char *skip_status_symbols(IrcState *state, const char *target) {
	const char *symbols = get_irc_support(state, "STATUSMSG");
	while (symbols && *target && strchr(symbols, *target) && is_irc_channel_name(state, target + 1))
		target++;
	return target;
}


static gboolean is_from_server(const IrcMessage *message) {
	return !message->source || !strchr(message->source, '!');
}


/* The room a message, reaction or redaction belongs in, made when it does not exist yet */
static CrimsonRoom *find_conversation(CrimsonNetwork *network, const IrcMessage *message) {
	IrcState *state = get_network_state(network);
	const char *target = get_irc_parameter(message, 0);
	if (!target || !state || is_from_server(message) || g_str_equal(target, "*"))
		return network->server_room;
	target = skip_status_symbols(state, target);
	if (is_irc_channel_name(state, target))
		return ensure_room(network, ROOM_CHANNEL, target);
	const char *context = get_irc_tag(message, "+channel-context");
	CrimsonRoom *context_room = context ? find_room(network, context) : NULL;
	if (context_room && context_room->kind == ROOM_CHANNEL)
		return context_room;
	g_autofree char *sender = get_source_nick(message->source);
	const char *peer = is_own_nick(state, sender) ? target : sender;
	/* What services say, such as the last login, goes with the server's messages instead of opening chats */
	if (is_service_nick(peer))
		return network->server_room;
	gboolean is_new = !find_room(network, peer);
	CrimsonRoom *room = ensure_room(network, ROOM_PRIVATE, peer);
	if (is_new && get_irc_support(state, "MONITOR"))
		send_irc_client_command(network->client, "MONITOR", "+", peer, NULL);
	return room;
}


/* The bouncer answers for its networks itself, once for all of the user's devices */
static void answer_ctcp(CrimsonNetwork *network, const char *sender, const char *request) {
	if (network->bouncer_id || g_ascii_strncasecmp(request, "VERSION", 7) != 0)
		return;
	g_autofree char *answer = g_strdup_printf("%cVERSION %s %s%c", CTCP_MARK, TEXT_APPLICATION_NAME,
		APPLICATION_VERSION, CTCP_MARK);
	send_irc_client_command(network->client, "NOTICE", sender, answer, NULL);
}


/* Turns PRIVMSG and NOTICE into a message, or NULL for a client query such as VERSION */
static CrimsonMessage *read_text_message(CrimsonNetwork *network, const IrcMessage *message, gboolean is_history) {
	g_autofree char *sender = get_source_nick(message->source);
	const char *text = get_last_irc_parameter(message);
	MessageKind kind = g_str_equal(message->command, "NOTICE") ? MESSAGE_NOTICE : MESSAGE_TEXT;
	g_autofree char *body = g_strdup(text);
	if (*body == CTCP_MARK) {
		gsize length = strlen(body);
		if (length > 1 && body[length - 1] == CTCP_MARK)
			body[length - 1] = '\0';
		if (g_ascii_strncasecmp(body + 1, "ACTION ", 7) != 0) {
			if (kind == MESSAGE_TEXT && !is_history)
				answer_ctcp(network, sender, body + 1);
			return NULL;
		}
		kind = MESSAGE_ACTION;
		memmove(body, body + 8, strlen(body + 8) + 1);
	}
	CrimsonMessage *line = create_message(kind, is_from_server(message) ? NULL : sender, body,
		read_message_time(message));
	line->id = g_strdup(get_irc_tag(message, "msgid"));
	const char *reply = get_irc_tag(message, "+reply");
	line->reply_to = g_strdup(reply ? reply : get_irc_tag(message, "+draft/reply"));
	line->is_bot = has_irc_tag(message, "bot");
	IrcState *state = get_network_state(network);
	line->is_own = state && is_own_nick(state, sender);
	return line;
}


/* An echo of something this client sent fills in the waiting copy instead of adding another */
static gboolean confirm_own_message(CrimsonNetwork *network, CrimsonRoom *room, CrimsonMessage *line,
	const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	const char *label = get_irc_tag(message, "label");
	CrimsonMessage *pending = label ? g_hash_table_lookup(work->pending, label) : NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
	for (guint index = 0; !pending && index < count; index++) {
		g_autoptr(CrimsonMessage) candidate = g_list_model_get_item(G_LIST_MODEL(room->messages), index);
		if (candidate->is_pending && g_str_equal(candidate->text, line->text))
			pending = candidate;
	}
	if (!pending)
		return FALSE;
	pending->is_pending = FALSE;
	pending->time = line->time;
	g_free(pending->id);
	pending->id = g_strdup(line->id);
	if (pending->id)
		g_hash_table_insert(room->by_id, g_strdup(pending->id), pending);
	announce_message_change(pending);
	if (pending->label)
		g_hash_table_remove(work->pending, pending->label);
	return TRUE;
}


static void stop_typing(CrimsonRoom *room, const char *nick) {
	if (nick && g_hash_table_remove(room->typing, nick))
		announce_room_change(room);
}


/* NickServ asking to identify means a login is missing, which the app offers to save instead of typed commands */
static void offer_login(CrimsonNetwork *network, const CrimsonMessage *line) {
	NetworkWork *work = get_network_work(network);
	const char *login = network->bouncer_id ? g_hash_table_lookup(network->attributes, "sasl-username")
		: network->session->record->sasl_username;
	if (work->has_offered_login || (login && *login) || !line->sender || !line->text
		|| g_ascii_strcasecmp(line->sender, "NickServ") != 0)
		return;
	g_autofree char *text = g_ascii_strdown(line->text, -1);
	/* Only the prompt about the nickname itself, not help text that merely mentions identifying */
	if (!strstr(text, "nickname is registered") && !strstr(text, "nickname is reserved")
		&& !strstr(text, "nick is registered"))
		return;
	work->has_offered_login = TRUE;
	network->wants_login = TRUE;
	announce_network_change(network);
}


/* Whether the message comes from someone the user ignores, whose messages, reactions, typing and invites stay out
 * of view while the bouncer still keeps them */
static gboolean is_from_ignored(CrimsonNetwork *network, const IrcMessage *message) {
	g_autofree char *sender = message->source ? get_source_nick(message->source) : NULL;
	return sender && find_room_override(get_settings()->ignored_people, network->key, sender);
}


static void deliver_text(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
	if (is_from_ignored(network, message))
		return;
	g_autoptr(CrimsonMessage) line = read_text_message(network, message, load != NULL);
	if (!line)
		return;
	CrimsonRoom *room = load ? load->room : find_conversation(network, message);
	if (line->is_own && !load && confirm_own_message(network, room, line, message))
		return;
	/* The server's own messages, such as a service naming the user's last login, are never a mention */
	line->is_highlight = !line->is_own && line->sender && room->kind != ROOM_SERVER
		&& (room->kind == ROOM_PRIVATE || is_mention(network, line->text));
	stop_typing(room, line->sender);
	/* A live message is marked new before it joins the room, since the open chat reacts while it is added */
	line->arrival_time = load ? 0 : g_get_monotonic_time();
	if (!insert_room_message(room, line))
		return;
	if (line->is_own && line->time > room->read_time)
		room->read_time = line->time;
	if (load)
		return;
	offer_login(network, line);
	Question *question = room == network->server_room && !line->is_own ? find_question(network, message) : NULL;
	if (question)
		show_answer(network, question);
	count_unread(room);
	announce_room_change(room);
	save_chat_cache_later(network->session->chat);
	if (line->sender && line->time > room->read_time && wants_notification(room, line))
		network->session->chat->events.notify(room, line, network->session->chat->data);
}


static gboolean expire_typing(gpointer data) {
	CrimsonNetwork *network = data;
	NetworkWork *work = get_network_work(network);
	gint64 now = g_get_monotonic_time();
	gboolean is_anyone_typing = FALSE;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		GHashTableIter iterator;
		gpointer until;
		gboolean has_changed = FALSE;
		g_hash_table_iter_init(&iterator, room->typing);
		while (g_hash_table_iter_next(&iterator, NULL, &until)) {
			if (*(gint64 *)until <= now) {
				g_hash_table_iter_remove(&iterator);
				has_changed = TRUE;
			}
		}
		if (has_changed)
			announce_room_change(room);
		is_anyone_typing |= is_room_typing(room);
	}
	if (is_anyone_typing)
		return G_SOURCE_CONTINUE;
	work->typing_source = 0;
	return G_SOURCE_REMOVE;
}


static void note_typing(CrimsonNetwork *network, CrimsonRoom *room, const char *nick, const char *state) {
	if (g_str_equal(state, "done")) {
		stop_typing(room, nick);
		return;
	}
	gint64 seconds = g_str_equal(state, "paused") ? TYPING_PAUSED_SECONDS : TYPING_ACTIVE_SECONDS;
	gint64 *until = g_new(gint64, 1);
	*until = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;
	g_hash_table_insert(room->typing, g_strdup(nick), until);
	announce_room_change(room);
	NetworkWork *work = get_network_work(network);
	if (!work->typing_source)
		work->typing_source = g_timeout_add_seconds(1, expire_typing, network);
}


/* Someone reacting to the user's own message shows in the list and the tray like a mention, and taking the reaction
 * back takes that away again */
static void note_new_reaction(CrimsonNetwork *network, CrimsonRoom *room, CrimsonMessage *target, const char *sender,
	const char *reaction, const char *removal) {
	if (removal) {
		if (room->new_reaction && g_str_equal(room->new_reaction, removal) && clear_new_reaction(room))
			announce_room_change(room);
		return;
	}
	g_autofree char *plain = strip_irc_formatting(target->text);
	g_autofree char *start = g_utf8_substring(plain, 0, REACTION_EXCERPT_CHARS);
	g_autofree char *excerpt = g_utf8_strlen(plain, -1) > REACTION_EXCERPT_CHARS ? g_strconcat(start, TEXT_CUT_OFF, NULL)
		: g_strdup(plain);
	g_free(room->new_reaction);
	g_free(room->new_reaction_text);
	room->new_reaction = g_strdup(reaction);
	room->new_reaction_text = g_strdup_printf(TEXT_REACTED_TO, sender, reaction, excerpt);
	room->new_reaction_time = g_get_real_time();
	announce_room_change(room);
	g_autoptr(CrimsonMessage) notice = create_message(MESSAGE_TEXT, sender, room->new_reaction_text,
		room->new_reaction_time);
	notice->is_highlight = TRUE;
	if (wants_notification(room, notice))
		network->session->chat->events.notify(room, notice, network->session->chat->data);
}


static void deliver_tag_message(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
	if (is_from_ignored(network, message))
		return;
	g_autofree char *sender = get_source_nick(message->source);
	CrimsonRoom *room = load ? load->room : find_conversation(network, message);
	IrcState *state = get_network_state(network);
	const char *typing = get_irc_tag(message, "+typing");
	if (typing && !load && !is_own_nick(state, sender)) {
		note_typing(network, room, sender, typing);
		return;
	}
	const char *reply = get_irc_tag(message, "+reply");
	CrimsonMessage *target = find_room_message(room, reply ? reply : get_irc_tag(message, "+draft/reply"));
	/* The registry names the tags without the draft prefix, which some clients already send */
	const char *reaction = get_irc_tag(message, "+draft/react");
	const char *removal = get_irc_tag(message, "+draft/unreact");
	if (!reaction && !removal) {
		reaction = get_irc_tag(message, "+react");
		removal = get_irc_tag(message, "+unreact");
	}
	if (!target || (!reaction && !removal))
		return;
	if (change_reaction(target, reaction ? reaction : removal, sender, reaction != NULL))
		announce_message_change(target);
	if (!load && target->is_own && !is_own_nick(state, sender))
		note_new_reaction(network, room, target, sender, reaction, removal);
}


static void deliver_redaction(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
	CrimsonRoom *room = load ? load->room : find_conversation(network, message);
	CrimsonMessage *target = find_room_message(room, get_irc_parameter(message, 1));
	if (!target || target->is_redacted)
		return;
	target->is_redacted = TRUE;
	announce_message_change(target);
	count_unread(room);
	announce_room_change(room);
}


static void add_event(CrimsonRoom *room, const IrcMessage *message, char *text, gboolean is_membership,
	const char *action) {
	g_autofree char *owned = text;
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_EVENT, NULL, text, read_message_time(message));
	line->id = g_strdup(get_irc_tag(message, "msgid"));
	line->is_membership = is_membership;
	line->action = action;
	if (insert_room_message(room, line))
		announce_room_change(room);
}


void save_network_channels(CrimsonNetwork *network) {
	AccountRecord *record = network->session->record;
	if (record->is_bouncer)
		return;
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (room->kind == ROOM_CHANNEL && room->is_joined)
			g_strv_builder_add(builder, room->name);
	}
	g_strfreev(record->channels);
	record->channels = g_strv_builder_end(builder);
	save_chat_accounts(network->session->chat);
}


static void handle_own_join(CrimsonNetwork *network, CrimsonRoom *room) {
	room->is_joined = TRUE;
	g_clear_pointer(&room->refusal, g_free);
	/* A server reached directly is asked who the room's people are, which a bouncer asks for itself */
	IrcState *joined_state = get_network_state(network);
	if (!network->bouncer_id && joined_state && get_irc_support(joined_state, "WHOX"))
		send_irc_client_command(network->client, "WHO", room->name, WHO_FIELDS, NULL);
	save_network_channels(network);
	request_latest_history(room);
	announce_room_change(room);
	NetworkWork *work = get_network_work(network);
	IrcState *state = get_network_state(network);
	if (work->opening_room && are_irc_names_equal(state->casemap, work->opening_room, room->name)) {
		g_clear_pointer(&work->opening_room, g_free);
		network->session->chat->events.room_opened(room, network->session->chat->data);
	}
}


/* Joins, parts and similar events, which change membership live and only draw a note in history */
static void deliver_membership(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
	IrcState *state = get_network_state(network);
	g_autofree char *sender = get_source_nick(message->source);
	const char *channel = get_irc_parameter(message, 0);
	const char *command = message->command;
	CrimsonRoom *room = load ? load->room : channel ? find_room(network, channel) : NULL;
	gboolean is_self = is_own_nick(state, sender);
	if (g_str_equal(command, "JOIN")) {
		if (!room && is_self && !load)
			room = ensure_room(network, ROOM_CHANNEL, channel);
		if (room && is_self && !load)
			handle_own_join(network, room);
		else if (room)
			add_event(room, message, g_strdup_printf(TEXT_JOINED, sender), TRUE, NULL);
	} else if (g_str_equal(command, "PART") && room) {
		const char *reason = get_irc_parameter(message, 1);
		stop_typing(room, sender);
		if (is_self && !load) {
			room->is_joined = FALSE;
			save_network_channels(network);
		}
		add_event(room, message, describe_leaving(sender, reason), !is_self, NULL);
	} else if (g_str_equal(command, "KICK") && room) {
		const char *kicked = get_irc_parameter(message, 1);
		if (kicked && is_own_nick(state, kicked) && !load)
			room->is_joined = FALSE;
		add_event(room, message, describe_kick(sender, kicked, get_irc_parameter(message, 2)),
			FALSE, NULL);
	} else if (g_str_equal(command, "TOPIC") && room) {
		g_free(room->topic);
		room->topic = g_strdup(get_irc_parameter(message, 1));
		add_event(room, message, g_strdup_printf(TEXT_TOPIC_CHANGED, sender, get_last_irc_parameter(message)),
			FALSE, NULL);
	} else if (g_str_equal(command, "MODE") && room) {
		/* Changes to the room and its people stay in view, unlike the comings and goings that are hidden by choice */
		char *text = describe_room_modes(state, message, 1);
		if (text)
			add_event(room, message, text, FALSE, NULL);
	}
}


/* QUIT and NICK reach every room the person shared, or only the history room being loaded */
static void deliver_departure(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected,
	HistoryLoad *load) {
	g_autofree char *sender = get_source_nick(message->source);
	gboolean is_nick = g_str_equal(message->command, "NICK");
	const char *new_nick = get_irc_parameter(message, 0);
	IrcState *state = get_network_state(network);
	gboolean is_own_rename = is_nick && state && is_own_nick(state, new_nick);
	g_autoptr(GPtrArray) rooms = g_ptr_array_new();
	if (load)
		g_ptr_array_add(rooms, load->room);
	for (guint index = 0; !load && affected && index < affected->len; index++) {
		CrimsonRoom *room = find_room(network, g_ptr_array_index(affected, index));
		if (room)
			g_ptr_array_add(rooms, room);
	}
	CrimsonRoom *private_room = load ? NULL : find_room(network, sender);
	if (private_room && private_room->kind == ROOM_PRIVATE) {
		g_ptr_array_add(rooms, private_room);
		if (is_nick && new_nick) {
			g_free(private_room->name);
			private_room->name = g_strdup(new_nick);
		}
	}
	for (guint index = 0; index < rooms->len; index++) {
		char *text = is_nick ? g_strdup_printf(TEXT_RENAMED, sender, new_nick)
			: describe_leaving(sender, get_irc_parameter(message, 0));
		add_event(g_ptr_array_index(rooms, index), message, text, !is_own_rename, NULL);
	}
	for (guint index = 0; index < rooms->len; index++) {
		stop_typing(g_ptr_array_index(rooms, index), sender);
		announce_room_change(g_ptr_array_index(rooms, index));
	}
}


static void deliver_read_marker(CrimsonNetwork *network, const IrcMessage *message) {
	CrimsonRoom *room = find_room(network, get_irc_parameter(message, 0));
	const char *value = get_irc_parameter(message, 1);
	if (!room || !value || !g_str_has_prefix(value, "timestamp="))
		return;
	g_autoptr(GDateTime) time = parse_irc_time(value + strlen("timestamp="));
	gint64 read_time = time ? g_date_time_to_unix_usec(time) : 0;
	if (read_time <= room->read_time)
		return;
	room->read_time = read_time;
	count_unread(room);
	clear_new_reaction(room);
	announce_room_change(room);
	if (!room->unread_count)
		network->session->chat->events.room_read(room, network->session->chat->data);
	save_chat_cache_later(network->session->chat);
}


static void deliver_presence(CrimsonNetwork *network, const IrcMessage *message, gboolean is_online) {
	g_auto(GStrv) entries = g_strsplit(get_last_irc_parameter(message), ",", -1);
	for (int index = 0; entries[index]; index++) {
		g_autofree char *nick = get_source_nick(entries[index]);
		CrimsonRoom *room = find_room(network, nick);
		if (!room || room->kind != ROOM_PRIVATE)
			continue;
		room->presence = is_online ? PRESENCE_ONLINE : PRESENCE_OFFLINE;
		announce_room_change(room);
	}
}


static void forget_search(NetworkWork *work) {
	g_clear_pointer(&work->search_room, g_free);
	g_clear_pointer(&work->search_words, g_free);
	g_clear_pointer(&work->search_batch, g_free);
	g_clear_pointer(&work->search_results, g_ptr_array_unref);
	work->search_ready = NULL;
	work->search_data = NULL;
}


static void finish_search(NetworkWork *work) {
	SearchReady ready = work->search_ready;
	gpointer data = work->search_data;
	g_autoptr(GPtrArray) results = g_steal_pointer(&work->search_results);
	forget_search(work);
	if (ready)
		ready(results, data);
}


void cancel_message_search(CrimsonNetwork *network, gpointer data) {
	NetworkWork *work = get_network_work(network);
	if (work->search_data == data)
		forget_search(work);
}


/* A network without the bouncer's history searches what is loaded, answering from the main loop as the bouncer
 * would */
static void answer_loaded_search(gpointer data) {
	g_autoptr(CrimsonNetwork) network = data;
	finish_search(get_network_work(network));
}


void search_room_messages(CrimsonRoom *room, const char *words, SearchReady ready, gpointer data) {
	NetworkWork *work = get_network_work(room->network);
	forget_search(work);
	work->search_room = g_strdup(room->name);
	work->search_words = g_strdup(words);
	work->search_results = g_ptr_array_new_with_free_func(g_object_unref);
	work->search_ready = ready;
	work->search_data = data;
	if (room->network->bouncer_id && room->network->client) {
		send_irc_client_command(room->network->client, "CRIMSON", "SEARCH", room->name, words, NULL);
		return;
	}
	g_autofree char *folded_words = g_utf8_casefold(words, -1);
	guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
	for (guint index = count; index-- > 0;) {
		g_autoptr(CrimsonMessage) line = g_list_model_get_item(G_LIST_MODEL(room->messages), index);
		g_autofree char *plain = line->text && line->kind != MESSAGE_EVENT && line->kind != MESSAGE_STATUS
			? strip_irc_formatting(line->text) : NULL;
		g_autofree char *folded = plain ? g_utf8_casefold(plain, -1) : NULL;
		if (folded && strstr(folded, folded_words))
			g_ptr_array_add(work->search_results, g_object_ref(line));
	}
	g_idle_add_once(answer_loaded_search, g_object_ref(room->network));
}


/* The bouncer's answer names the room and words it searched, so a late answer to an older search is left out */
static void start_search(NetworkWork *work, const char *reference, const IrcMessage *message) {
	if (!work->search_ready || work->search_batch)
		return;
	if (g_strcmp0(get_irc_parameter(message, 2), work->search_room) == 0
		&& g_strcmp0(get_irc_parameter(message, 3), work->search_words) == 0)
		work->search_batch = g_strdup(reference);
}


/* Messages a search found are collected for it rather than shown in their room */
static gboolean collect_search_line(CrimsonNetwork *network, const IrcMessage *message, const char *batch) {
	NetworkWork *work = get_network_work(network);
	IrcBatch *search = find_enclosing_batch(network->client, message, SEARCH_BATCH);
	if (!search)
		return FALSE;
	if (g_strcmp0(batch, work->search_batch) != 0 || !work->search_results)
		return TRUE;
	CrimsonMessage *line = is_from_ignored(network, message) ? NULL : read_text_message(network, message, TRUE);
	if (line)
		g_ptr_array_add(work->search_results, line);
	return TRUE;
}


static void start_batch(CrimsonNetwork *network, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	const char *type = get_irc_parameter(message, 1);
	NetworkWork *work = get_network_work(network);
	if (g_strcmp0(type, SEARCH_BATCH) == 0) {
		start_search(work, reference + 1, message);
	} else if (g_strcmp0(type, "chathistory") == 0) {
		HistoryLoad *load = g_new0(HistoryLoad, 1);
		const char *target = get_irc_parameter(message, 2);
		load->room = g_strcmp0(target, SERVER_TARGET) == 0 ? network->server_room : target ? find_room(network,
			target) : NULL;
		/* A service's history belongs with the server's messages, like what it says live */
		if (!load->room && is_service_nick(target))
			load->room = network->server_room;
		if (!load->room && target)
			load->room = ensure_room(network, is_irc_channel_name(get_network_state(network), target) ? ROOM_CHANNEL
				: ROOM_PRIVATE, target);
		load->is_end = has_irc_tag(message, "draft/chathistory-end");
		load->is_batching = load->room != NULL;
		if (load->room)
			begin_room_batch(load->room);
		g_hash_table_insert(work->history, g_strdup(reference + 1), load);
	} else if (g_strcmp0(type, "netsplit") == 0 || g_strcmp0(type, "netjoin") == 0) {
		/* Room names to how many people the split took or brought back, with the batch's kind under an empty
		 * name */
		GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		g_hash_table_insert(counts, g_strdup(""), GINT_TO_POINTER(g_str_equal(type, "netjoin")));
		g_hash_table_insert(work->splits, g_strdup(reference + 1), counts);
	} else if (g_strcmp0(type, "draft/multiline") == 0) {
		IrcMessage *gathered = copy_irc_message(message);
		g_ptr_array_set_size(gathered->parameters, 0);
		add_irc_parameter(gathered, get_irc_parameter(message, 2));
		add_irc_parameter(gathered, "");
		g_free(gathered->command);
		gathered->command = NULL;
		remove_irc_tag(gathered, "batch");
		g_hash_table_insert(work->multiline, g_strdup(reference + 1), gathered);
	}
}


void free_history_load(gpointer data) {
	HistoryLoad *load = data;
	if (load->is_batching)
		end_room_batch(load->room);
	g_free(load);
}


/* A netsplit or netjoin shows as one line per room with how many people it took or brought back */
static void finish_split(CrimsonNetwork *network, const IrcMessage *message, GHashTable *counts) {
	gboolean is_join = GPOINTER_TO_INT(g_hash_table_lookup(counts, ""));
	GHashTableIter iterator;
	gpointer name, count;
	g_hash_table_iter_init(&iterator, counts);
	while (g_hash_table_iter_next(&iterator, &name, &count)) {
		CrimsonRoom *room = *(char *)name ? find_room(network, name) : NULL;
		if (!room)
			continue;
		int people = GPOINTER_TO_INT(count);
		char *text = is_join ? g_strdup_printf(people == 1 ? TEXT_NETJOIN_ONE : TEXT_NETJOIN, people)
			: g_strdup_printf(people == 1 ? TEXT_NETSPLIT_ONE : TEXT_NETSPLIT, people);
		add_event(room, message, text, TRUE, NULL);
		announce_room_change(room);
	}
}


/* Inside a netsplit or netjoin, a leave or join only counts toward its room's line, while the members still change */
static gboolean count_split(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected) {
	const char *batch = get_irc_tag(message, "batch");
	GHashTable *counts = batch ? g_hash_table_lookup(get_network_work(network)->splits, batch) : NULL;
	if (!counts)
		return FALSE;
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	if (g_str_equal(message->command, "JOIN") && get_irc_parameter(message, 0))
		g_ptr_array_add(names, (gpointer)get_irc_parameter(message, 0));
	for (guint index = 0; g_str_equal(message->command, "QUIT") && affected && index < affected->len; index++)
		g_ptr_array_add(names, g_ptr_array_index(affected, index));
	for (guint index = 0; index < names->len; index++) {
		const char *name = g_ptr_array_index(names, index);
		int count = GPOINTER_TO_INT(g_hash_table_lookup(counts, name));
		g_hash_table_replace(counts, g_strdup(name), GINT_TO_POINTER(count + 1));
	}
	return TRUE;
}


static void finish_batch(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *outer) {
	const char *reference = get_irc_parameter(message, 0) + 1;
	NetworkWork *work = get_network_work(network);
	if (g_strcmp0(reference, work->search_batch) == 0) {
		finish_search(work);
		return;
	}
	GHashTable *counts = g_hash_table_lookup(work->splits, reference);
	if (counts) {
		finish_split(network, message, counts);
		g_hash_table_remove(work->splits, reference);
		return;
	}
	HistoryLoad *load = g_hash_table_lookup(work->history, reference);
	if (load && load->is_batching) {
		load->is_batching = FALSE;
		end_room_batch(load->room);
	}
	if (load && load->room) {
		CrimsonRoom *room = load->room;
		/* An empty page also ends history on servers that never mark the end, while catching up after a
		 * reconnect only reaches the newest end */
		room->has_all_history |= room->history_request != HISTORY_NEWER && (load->is_end || !load->lines);
		gboolean has_more_newer = room->history_request == HISTORY_NEWER && load->lines && !load->is_end;
		room->history_request = HISTORY_NONE;
		if (has_more_newer)
			request_latest_history(room);
		room->has_history = TRUE;
		count_unread(room);
		announce_room_change(room);
		save_chat_cache_later(network->session->chat);
	}
	g_hash_table_remove(work->history, reference);
	IrcMessage *gathered = g_hash_table_lookup(work->multiline, reference);
	if (gathered && gathered->command)
		handle_message_in(network, gathered, NULL, outer);
	g_hash_table_remove(work->multiline, reference);
}


/* A multiline line adds to its gathered message, joined by a line break unless it continues the line before */
static gboolean gather_multiline(CrimsonNetwork *network, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	IrcMessage *gathered = g_hash_table_lookup(work->multiline, get_irc_tag(message, "batch"));
	if (!gathered)
		return FALSE;
	gboolean is_first = !gathered->command;
	if (is_first)
		gathered->command = g_strdup(message->command);
	char **text = (char **)&g_ptr_array_index(gathered->parameters, 1);
	const char *separator = has_irc_tag(message, "draft/multiline-concat") || is_first ? "" : "\n";
	char *joined = g_strconcat(*text, separator, get_last_irc_parameter(message), NULL);
	g_free(*text);
	*text = joined;
	return TRUE;
}


static void deliver_targets(CrimsonNetwork *network, const IrcMessage *message) {
	const char *target = get_irc_parameter(message, 1);
	IrcState *state = get_network_state(network);
	if (!target || !state || is_irc_channel_name(state, target) || is_service_nick(target))
		return;
	/* A deleted chat only comes back with a message newer than its deletion */
	g_autoptr(GDateTime) latest = parse_irc_time(get_irc_parameter(message, 2));
	gint64 deleted = find_deleted_chat(network->key, target);
	if (deleted && (!latest || g_date_time_to_unix_usec(latest) <= deleted))
		return;
	open_private_room(network, target);
}


/* The message of the day arrives a line at a time and shows as one message in the server's chat, in a fixed width
 * font since servers draw pictures with its characters */
static gboolean collect_welcome_message(CrimsonNetwork *network, int number, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	if (number == 375) {
		g_clear_pointer(&work->welcome_message, g_free);
		work->welcome_message = g_string_new(NULL);
		return TRUE;
	}
	if (number == 372 && work->welcome_message) {
		const char *line = get_last_irc_parameter(message);
		if (g_str_has_prefix(line, "- "))
			line += 2;
		g_string_append_printf(work->welcome_message, "%s" FIXED_WIDTH_LINE "%s", work->welcome_message->len ? "\n"
			: "", line);
		return TRUE;
	}
	if (number != 376 || !work->welcome_message)
		return number == 372 || number == 376;
	g_autofree char *text = g_string_free(g_steal_pointer(&work->welcome_message), FALSE);
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_NOTICE, NULL, text, read_message_time(message));
	insert_room_message(network->server_room, line);
	Question *question = find_question(network, message);
	if (question)
		show_answer(network, question);
	else
		announce_room_change(network->server_room);
	return TRUE;
}


/* Whether a room's newest line already says this, as when the same refusal comes again */
static gboolean is_latest_line(CrimsonRoom *room, const char *text) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
	g_autoptr(CrimsonMessage) latest = count ? g_list_model_get_item(G_LIST_MODEL(room->messages), count - 1) : NULL;
	return latest && g_strcmp0(latest->text, text) == 0;
}


/* A problem with something the user just did shows at once, or what helps with it opens, a room that would not let
 * the user back in keeps a note saying why, problems with what the app asked by itself stay with the server's
 * messages, and other replies go with the room they concern */
static void show_reply(CrimsonNetwork *network, const IrcMessage *message, ReplyKind kind, const char *words,
	const char *subject, const char *action) {
	CrimsonRoom *room = subject ? find_room(network, subject) : NULL;
	gboolean is_problem = kind == REPLY_PROBLEM || kind == REPLY_REFUSED_JOIN;
	ChatEvents *events = &network->session->chat->events;
	gboolean is_asked = is_problem && find_question(network, message);
	if (is_asked && action && subject)
		events->help_offered(network, action, subject, network->session->chat->data);
	else if (is_asked)
		events->show_message(words, network->session->chat->data);
	else if (kind == REPLY_PROBLEM)
		add_status(network->server_room, message, words);
	else if (room && !is_latest_line(room, words))
		add_event(room, message, g_strdup(words), FALSE, action);
	else if (!room)
		add_server_line(network, message, words);
}


/* A room that would not let the user in or speak takes no messages until a join or login succeeds */
static void refuse_room(CrimsonRoom *room, const char *words) {
	if (!room || room->kind != ROOM_CHANNEL)
		return;
	g_free(room->refusal);
	room->refusal = g_strdup(words);
	announce_room_change(room);
}


/* Logging in can open the rooms that refused the user, so they take messages again and the ones left are joined */
static void forgive_refused_rooms(CrimsonNetwork *network) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (!room->refusal)
			continue;
		g_clear_pointer(&room->refusal, g_free);
		if (!room->is_joined)
			send_irc_client_command(network->client, "JOIN", room->name, NULL);
		announce_room_change(room);
	}
}


/* A message of the day the bouncer kept whole, from history or as the answer to /motd */
static void deliver_welcome_message(CrimsonNetwork *network, const IrcMessage *message, gboolean is_live) {
	const char *text = get_irc_tag(message, WELCOME_MESSAGE_TAG);
	if (!text)
		return;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	GString *shown = g_string_new(NULL);
	for (int index = 0; lines[index]; index++) {
		const char *line = g_str_has_prefix(lines[index], "- ") ? lines[index] + 2 : lines[index];
		g_string_append_printf(shown, "%s" FIXED_WIDTH_LINE "%s", shown->len ? "\n" : "", line);
	}
	g_autofree char *joined = g_string_free(shown, FALSE);
	g_autoptr(CrimsonMessage) kept = create_message(MESSAGE_NOTICE, NULL, joined, read_message_time(message));
	kept->id = g_strdup(get_irc_tag(message, "msgid"));
	kept->arrival_time = is_live ? g_get_monotonic_time() : 0;
	if (!insert_room_message(network->server_room, kept))
		return;
	Question *question = find_question(network, message);
	if (question)
		show_answer(network, question);
	else
		announce_room_change(network->server_room);
}


/* A reply kept in the server's chat shows in plain words, without anything a live one would also do */
static void deliver_kept_reply(CrimsonNetwork *network, const IrcMessage *message) {
	ReplyKind kind = REPLY_NOTE;
	const char *subject = NULL, *action = NULL;
	g_autofree char *words = describe_reply(message, &kind, &subject, &action);
	if (kind != REPLY_HIDDEN && words && *words)
		add_status(network->server_room, message, words);
}


static void deliver_numeric(CrimsonNetwork *network, const IrcMessage *message) {
	const char *command = message->command;
	int number = (int)g_ascii_strtoll(command, NULL, 10);
	ReplyKind kind = REPLY_NOTE;
	const char *subject = NULL, *action = NULL;
	g_autofree char *words = describe_reply(message, &kind, &subject, &action);
	if (subject && (number == 401 || number == 404 || number == 477))
		{
		CrimsonRoom *refusing = find_room(network, subject);
		if (refusing)
			fail_pending_messages(refusing, words);
	}
	if ((g_strv_contains(WHOIS_REPLIES, command) || number == 401 || number == 402)
		&& collect_profile(network, number, message))
		return;
	if (number == 332) {
		CrimsonRoom *room = find_room(network, get_irc_parameter(message, 1));
		if (room) {
			g_free(room->topic);
			room->topic = g_strdup(get_last_irc_parameter(message));
			announce_room_change(room);
		}
		return;
	}
	if (number == 366) {
		CrimsonRoom *room = find_room(network, get_irc_parameter(message, 1));
		if (room)
			announce_room_change(room);
		return;
	}
	if (collect_room_listing(network, number, message))
		return;
	if (number == 730 || number == 731) {
		deliver_presence(network, message, number == 730);
		return;
	}
	if (collect_welcome_message(network, number, message))
		return;
	/* A room's settings and the ends of its lists show on its settings page */
	if (number == 324 || number == 347 || number == 349 || number == 368) {
		CrimsonRoom *room = find_room(network, get_irc_parameter(message, 1));
		if (room)
			announce_room_change(room);
		return;
	}
	if (number == 900)
		forgive_refused_rooms(network);
	if (kind == REPLY_REFUSED_JOIN && subject)
		refuse_room(find_room(network, subject), words);
	if (kind == REPLY_HIDDEN)
		return;
	/* A person or room that refuses messages, such as one missing or open only to registered accounts, is left
	 * alone for a while by what the app sends on its own, such as typing notices */
	const char *target = get_irc_parameter(message, 1);
	if ((number == 401 || number == 403 || number == 404 || number == 477) && target)
		refuse_target(network, target);
	show_reply(network, message, kind, words, subject, action);
}


static void deliver_standard_reply(CrimsonNetwork *network, const IrcMessage *message) {
	/* Profile details the app asks for and sets on every connection are quietly left out where the server lacks
	 * them, since the user did not do anything */
	if (g_strcmp0(get_irc_parameter(message, 0), "METADATA") == 0)
		return;
	add_server_line(network, message, get_last_irc_parameter(message));
}


static void deliver_rename(CrimsonNetwork *network, const IrcMessage *message) {
	CrimsonRoom *room = find_room(network, get_irc_parameter(message, 0));
	const char *new_name = get_irc_parameter(message, 1);
	if (!room || !new_name)
		return;
	g_free(room->name);
	room->name = g_strdup(new_name);
	save_network_channels(network);
	announce_room_change(room);
}


/* Keeps the avatar the server now gives a person or room for the next start, or forgets one it took away */
static void remember_avatar(CrimsonNetwork *network, const char *target) {
	IrcState *state = get_network_state(network);
	if (!target || !state)
		return;
	IrcChannel *channel = is_irc_channel_name(state, target) ? find_irc_channel(state, target) : NULL;
	IrcUser *user = channel ? NULL : find_irc_user(state, target);
	GHashTable *metadata = channel ? channel->metadata : user ? user->metadata : NULL;
	const char *url = metadata ? g_hash_table_lookup(metadata, "avatar") : NULL;
	g_autofree char *lowered = g_utf8_strdown(target, -1);
	if (g_strcmp0(url, g_hash_table_lookup(network->known_avatars, lowered)) == 0)
		return;
	if (url)
		g_hash_table_replace(network->known_avatars, g_steal_pointer(&lowered), g_strdup(url));
	else
		g_hash_table_remove(network->known_avatars, lowered);
	save_chat_cache_later(network->session->chat);
}


/* Replies name the target after the user's nick, and a removed key comes back as a key that is not set */
static void deliver_metadata(CrimsonNetwork *network, const IrcMessage *message) {
	gboolean is_reply = !g_str_equal(message->command, "METADATA");
	const char *target = get_irc_parameter(message, is_reply ? 1 : 0);
	IrcState *state = get_network_state(network);
	remember_avatar(network, state && target && g_str_equal(target, "*") ? state->nick : target);
	CrimsonRoom *room = find_room(network, target);
	if (room)
		announce_room_change(room);
	/* The user's own details change how the profile and the network show them */
	if (target && state && (g_str_equal(target, "*") || is_own_nick(state, target)))
		announce_network_change(network);
}


static void deliver_invite(CrimsonNetwork *network, const IrcMessage *message) {
	if (is_from_ignored(network, message))
		return;
	g_autofree char *sender = get_source_nick(message->source);
	const char *room_name = get_irc_parameter(message, 1);
	g_autofree char *text = g_strdup_printf(TEXT_INVITED, sender, room_name);
	add_status(network->server_room, message, text);
	if (room_name && !find_room(network, room_name))
		network->session->chat->events.room_invited(network, room_name, network->session->chat->data);
}


static void handle_message_in(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected,
	HistoryLoad *load) {
	const char *command = message->command;
	if (g_str_equal(command, "PRIVMSG") || g_str_equal(command, "NOTICE"))
		deliver_text(network, message, load);
	else if (g_str_equal(command, "TAGMSG"))
		deliver_tag_message(network, message, load);
	else if (g_str_equal(command, "REDACT"))
		deliver_redaction(network, message, load);
	else if (g_str_equal(command, "JOIN") || g_str_equal(command, "PART") || g_str_equal(command, "KICK")
		|| g_str_equal(command, "TOPIC") || g_str_equal(command, "MODE"))
		deliver_membership(network, message, load);
	else if (g_str_equal(command, "QUIT") || g_str_equal(command, "NICK"))
		deliver_departure(network, message, affected, load);
	else if (g_str_equal(command, "CRIMSON") && g_strcmp0(get_irc_parameter(message, 0), "MOTD") == 0)
		deliver_welcome_message(network, message, load == NULL);
	else if (load && load->room == network->server_room && g_ascii_isdigit(*command))
		deliver_kept_reply(network, message);
	else if (load)
		return;
	else if (g_str_equal(command, "MARKREAD"))
		deliver_read_marker(network, message);
	else if (g_str_equal(command, "CHATHISTORY"))
		deliver_targets(network, message);
	else if (g_str_equal(command, "FAIL") || g_str_equal(command, "WARN") || g_str_equal(command, "NOTE"))
		deliver_standard_reply(network, message);
	else if (g_str_equal(command, "RENAME"))
		deliver_rename(network, message);
	else if (g_str_equal(command, "METADATA") || g_str_equal(command, "761") || g_str_equal(command, "766"))
		deliver_metadata(network, message);
	else if (g_str_equal(command, "INVITE"))
		deliver_invite(network, message);
	else if (g_str_equal(command, "ERROR"))
		add_status(network->server_room, message, get_last_irc_parameter(message));
	else if (g_ascii_isdigit(*command))
		deliver_numeric(network, message);
}


void handle_network_message(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected) {
	NetworkWork *work = get_network_work(network);
	const char *batch = get_irc_tag(message, "batch");
	HistoryLoad *load = batch ? g_hash_table_lookup(work->history, batch) : NULL;
	if (g_str_equal(message->command, "BATCH")) {
		follow_answers(network, message);
		const char *reference = get_irc_parameter(message, 0);
		if (reference && *reference == '+')
			start_batch(network, message);
		else if (reference && *reference == '-')
			finish_batch(network, message, load);
		return;
	}
	follow_answers(network, message);
	if (batch && collect_search_line(network, message, batch))
		return;
	if (batch && gather_multiline(network, message))
		return;
	if (load && !load->room)
		return;
	if (load)
		load->lines++;
	if (!load && collect_registration(network, message))
		return;
	if (!load && count_split(network, message, affected))
		return;
	handle_message_in(network, message, affected, load);
}


static void ask_for_history(CrimsonRoom *room, HistoryRequest request, const char *subcommand, const char *point,
	int count) {
	g_autofree char *limit = g_strdup_printf("%d", count);
	room->history_request = request;
	const char *target = room->kind == ROOM_SERVER ? SERVER_TARGET : room->name;
	send_irc_client_command(room->network->client, "CHATHISTORY", subcommand, target, point, limit, NULL);
}


/* A bouncer keeps the server's own chat too, while a server reached directly keeps no history of it */
static gboolean has_room_history(CrimsonRoom *room) {
	return (room->kind != ROOM_SERVER || room->network->bouncer_id) && can_network(room->network, "draft/chathistory");
}


void request_latest_history(CrimsonRoom *room) {
	if (room->history_request || !has_room_history(room))
		return;
	CrimsonMessage *last = get_last_stored_message(room);
	if (!last || !room->has_history) {
		ask_for_history(room, HISTORY_LATEST, "LATEST", "*", HISTORY_PAGE);
		return;
	}
	g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(last->time);
	g_autofree char *moment = format_irc_time(time);
	const char *types = get_irc_support(get_network_state(room->network), "MSGREFTYPES");
	gboolean takes_ids = types && strstr(types, "msgid");
	g_autofree char *after = takes_ids ? g_strconcat("msgid=", last->id, NULL) : g_strconcat("timestamp=", moment,
		NULL);
	/* What came while the app was away is caught up from the newest message it has, page after page until none
	 * are left, so a long absence leaves no gap */
	ask_for_history(room, HISTORY_NEWER, "AFTER", after, HISTORY_JUMP_PAGE);
}


void load_older_history(CrimsonRoom *room, int count) {
	if (room->history_request || room->has_all_history || !has_room_history(room))
		return;
	g_autoptr(CrimsonMessage) first = g_list_model_get_item(G_LIST_MODEL(room->messages), 0);
	if (!first)
		return;
	g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(first->time);
	g_autofree char *moment = format_irc_time(time);
	g_autofree char *before = first->id ? g_strconcat("msgid=", first->id, NULL)
		: g_strconcat("timestamp=", moment, NULL);
	ask_for_history(room, HISTORY_OLDER, "BEFORE", before, count);
}


void request_private_targets(CrimsonNetwork *network) {
	if (!can_network(network, "draft/chathistory"))
		return;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) start = g_date_time_add_days(now, -TARGETS_DAYS);
	g_autoptr(GDateTime) end = g_date_time_add_days(now, 1);
	g_autofree char *from = format_irc_time(start);
	g_autofree char *to = format_irc_time(end);
	g_autofree char *from_text = g_strconcat("timestamp=", from, NULL);
	g_autofree char *to_text = g_strconcat("timestamp=", to, NULL);
	g_autofree char *limit = g_strdup_printf("%d", TARGETS_LIMIT);
	send_irc_client_command(network->client, "CHATHISTORY", "TARGETS", from_text, to_text, limit, NULL);
}
