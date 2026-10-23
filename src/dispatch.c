#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "session.h"
#include "settings.h"

#define TYPING_ACTIVE_SECONDS 6
#define TYPING_PAUSED_SECONDS 30
#define TARGETS_DAYS 30
/* How much of the user's message the list quotes when someone reacts to it */
#define REACTION_EXCERPT_CHARS 30
#define TARGETS_LIMIT 100
#define CTCP_MARK '\001'
/* A page of history should show this many lines, so pages of hidden joins and parts lead to the next page */
#define SHOWN_PER_PAGE 20

/* A history batch being received for one room */
typedef struct {
	CrimsonRoom *room;
	gboolean is_end;
	guint count_before;
	/* The oldest line of the page, shown or not, where the next page starts */
	gint64 oldest_time;
	char *oldest_id;
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


static const char *get_last_parameter(const IrcMessage *message) {
	guint count = count_irc_parameters(message);
	return count ? get_irc_parameter(message, count - 1) : "";
}


static void add_status(CrimsonRoom *room, const IrcMessage *message, const char *text) {
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_STATUS, NULL, text, read_message_time(message));
	insert_room_message(room, line);
	announce_room_change(room);
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


static void answer_ctcp(CrimsonNetwork *network, const char *sender, const char *request) {
	if (g_ascii_strncasecmp(request, "VERSION", 7) != 0)
		return;
	g_autofree char *answer = g_strdup_printf("%cVERSION %s %s%c", CTCP_MARK, TEXT_APPLICATION_NAME,
		APPLICATION_VERSION, CTCP_MARK);
	send_irc_client_command(network->client, "NOTICE", sender, answer, NULL);
}


/* Turns PRIVMSG and NOTICE into a message, or NULL for a client query such as VERSION */
static CrimsonMessage *read_text_message(CrimsonNetwork *network, const IrcMessage *message, gboolean is_history) {
	g_autofree char *sender = get_source_nick(message->source);
	const char *text = get_last_parameter(message);
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
	network->session->chat->events.login_wanted(network, network->session->chat->data);
}


static void deliver_text(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
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
	if (!insert_room_message(room, line))
		return;
	if (line->is_own && line->time > room->read_time)
		room->read_time = line->time;
	if (load)
		return;
	line->is_fresh = TRUE;
	offer_login(network, line);
	count_unread(room);
	announce_room_change(room);
	save_chat_cache_later(network->session->chat);
	Settings *settings = get_settings();
	gboolean wants_notice = room->kind == ROOM_PRIVATE ? settings->notifies_private : settings->notifies_mentions;
	if (line->is_highlight && wants_notice && line->time > room->read_time)
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
	g_autofree char *excerpt = g_utf8_strlen(plain, -1) > REACTION_EXCERPT_CHARS ? g_strconcat(start, "…", NULL)
		: g_strdup(plain);
	g_free(room->new_reaction);
	g_free(room->new_reaction_text);
	room->new_reaction = g_strdup(reaction);
	room->new_reaction_text = g_strdup_printf(TEXT_REACTED_TO, sender, reaction, excerpt);
	room->new_reaction_time = g_get_real_time();
	announce_room_change(room);
	if (get_settings()->notifies_mentions) {
		g_autoptr(CrimsonMessage) notice = create_message(MESSAGE_TEXT, sender, room->new_reaction_text,
			room->new_reaction_time);
		network->session->chat->events.notify(room, notice, network->session->chat->data);
	}
}


static void deliver_tag_message(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *load) {
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
	const char *reaction = get_irc_tag(message, "+draft/react");
	const char *removal = get_irc_tag(message, "+draft/unreact");
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


static void add_event(CrimsonRoom *room, const IrcMessage *message, char *text) {
	g_autofree char *owned = text;
	g_autoptr(CrimsonMessage) line = create_message(MESSAGE_EVENT, NULL, text, read_message_time(message));
	line->id = g_strdup(get_irc_tag(message, "msgid"));
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
	gboolean shows = get_settings()->shows_events;
	if (g_str_equal(command, "JOIN")) {
		if (!room && is_self && !load)
			room = ensure_room(network, ROOM_CHANNEL, channel);
		if (room && is_self && !load)
			handle_own_join(network, room);
		else if (room && shows)
			add_event(room, message, g_strdup_printf(TEXT_JOINED, sender));
	} else if (g_str_equal(command, "PART") && room) {
		const char *reason = get_irc_parameter(message, 1);
		stop_typing(room, sender);
		if (is_self && !load) {
			room->is_joined = FALSE;
			save_network_channels(network);
		}
		if (shows || is_self)
			add_event(room, message, reason && *reason ? g_strdup_printf(TEXT_LEFT_SAYING, sender, reason)
				: g_strdup_printf(TEXT_LEFT, sender));
	} else if (g_str_equal(command, "KICK") && room) {
		const char *kicked = get_irc_parameter(message, 1);
		if (kicked && is_own_nick(state, kicked) && !load)
			room->is_joined = FALSE;
		add_event(room, message, g_strdup_printf(TEXT_KICKED, sender, kicked, get_last_parameter(message)));
	} else if (g_str_equal(command, "TOPIC") && room) {
		g_free(room->topic);
		room->topic = g_strdup(get_irc_parameter(message, 1));
		add_event(room, message, g_strdup_printf(TEXT_TOPIC_CHANGED, sender, get_last_parameter(message)));
	} else if (g_str_equal(command, "MODE") && room && shows) {
		g_autoptr(GPtrArray) parts = g_ptr_array_new();
		for (guint index = 1; index < count_irc_parameters(message); index++)
			g_ptr_array_add(parts, (gpointer)get_irc_parameter(message, index));
		g_ptr_array_add(parts, NULL);
		g_autofree char *modes = g_strjoinv(" ", (char **)parts->pdata);
		add_event(room, message, g_strdup_printf(TEXT_MODE_CHANGED, sender, modes));
	}
}


/* QUIT and NICK reach every room the person shared, or only the history room being loaded */
static void deliver_departure(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected,
	HistoryLoad *load) {
	g_autofree char *sender = get_source_nick(message->source);
	gboolean is_nick = g_str_equal(message->command, "NICK");
	const char *new_nick = get_irc_parameter(message, 0);
	IrcState *state = get_network_state(network);
	gboolean shows = get_settings()->shows_events || (is_nick && state && is_own_nick(state, new_nick));
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
	for (guint index = 0; shows && index < rooms->len; index++) {
		char *text = is_nick ? g_strdup_printf(TEXT_RENAMED, sender, new_nick)
			: g_strdup_printf(TEXT_QUIT_EVENT, sender, get_last_parameter(message));
		add_event(g_ptr_array_index(rooms, index), message, text);
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
	g_auto(GStrv) entries = g_strsplit(get_last_parameter(message), ",", -1);
	for (int index = 0; entries[index]; index++) {
		g_autofree char *nick = get_source_nick(entries[index]);
		CrimsonRoom *room = find_room(network, nick);
		if (!room || room->kind != ROOM_PRIVATE)
			continue;
		room->presence = is_online ? PRESENCE_ONLINE : PRESENCE_OFFLINE;
		announce_room_change(room);
	}
}


static void start_batch(CrimsonNetwork *network, const IrcMessage *message) {
	const char *reference = get_irc_parameter(message, 0);
	const char *type = get_irc_parameter(message, 1);
	NetworkWork *work = get_network_work(network);
	if (g_strcmp0(type, "chathistory") == 0) {
		HistoryLoad *load = g_new0(HistoryLoad, 1);
		const char *target = get_irc_parameter(message, 2);
		load->room = target ? find_room(network, target) : NULL;
		/* A service's history belongs with the server's messages, like what it says live */
		if (!load->room && is_service_nick(target))
			load->room = network->server_room;
		if (!load->room && target)
			load->room = ensure_room(network, is_irc_channel_name(get_network_state(network), target) ? ROOM_CHANNEL
				: ROOM_PRIVATE, target);
		load->is_end = has_irc_tag(message, "draft/chathistory-end");
		load->count_before = load->room ? g_list_model_get_n_items(G_LIST_MODEL(load->room->messages)) : 0;
		g_hash_table_insert(work->history, g_strdup(reference + 1), load);
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
	g_free(load->oldest_id);
	g_free(load);
}


static void note_oldest_line(HistoryLoad *load, const IrcMessage *message) {
	gint64 time = read_message_time(message);
	if (load->oldest_time && time >= load->oldest_time)
		return;
	load->oldest_time = time;
	g_free(load->oldest_id);
	load->oldest_id = g_strdup(get_irc_tag(message, "msgid"));
}


/* Asks for the page before this one when this page showed too little, returning whether it did */
static gboolean continue_history(HistoryLoad *load) {
	CrimsonRoom *room = load->room;
	guint shown = g_list_model_get_n_items(G_LIST_MODEL(room->messages)) - load->count_before;
	if (load->is_end || shown >= SHOWN_PER_PAGE || !load->oldest_time)
		return FALSE;
	g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(load->oldest_time);
	g_autofree char *moment = format_irc_time(time);
	g_autofree char *before = load->oldest_id ? g_strconcat("msgid=", load->oldest_id, NULL)
		: g_strconcat("timestamp=", moment, NULL);
	g_autofree char *limit = g_strdup_printf("%d", HISTORY_PAGE);
	send_irc_client_command(room->network->client, "CHATHISTORY", "BEFORE", room->name, before, limit, NULL);
	return TRUE;
}


static void finish_batch(CrimsonNetwork *network, const IrcMessage *message, HistoryLoad *outer) {
	const char *reference = get_irc_parameter(message, 0) + 1;
	NetworkWork *work = get_network_work(network);
	HistoryLoad *load = g_hash_table_lookup(work->history, reference);
	if (load && load->room && continue_history(load)) {
		g_hash_table_remove(work->history, reference);
		return;
	}
	if (load && load->room) {
		CrimsonRoom *room = load->room;
		room->is_loading_history = FALSE;
		room->has_history = TRUE;
		room->has_all_history |= load->is_end;
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
	char *joined = g_strconcat(*text, separator, get_last_parameter(message), NULL);
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


static char *join_parameters(const IrcMessage *message, guint first) {
	g_autoptr(GPtrArray) parts = g_ptr_array_new();
	for (guint index = first; index < count_irc_parameters(message); index++)
		g_ptr_array_add(parts, (gpointer)get_irc_parameter(message, index));
	g_ptr_array_add(parts, NULL);
	return g_strjoinv(" ", (char **)parts->pdata);
}


static void deliver_numeric(CrimsonNetwork *network, const IrcMessage *message) {
	const char *command = message->command;
	NetworkWork *work = get_network_work(network);
	int number = (int)g_ascii_strtoll(command, NULL, 10);
	g_autofree char *text = join_parameters(message, 1);
	if ((g_strv_contains(WHOIS_REPLIES, command) || number == 401) && collect_profile(network, number, message))
		return;
	if (g_strv_contains(WHOIS_REPLIES, command)) {
		add_status(work->whois_room ? work->whois_room : network->server_room, message, text);
		if (g_str_equal(command, "318"))
			work->whois_room = NULL;
		return;
	}
	if (number == 332) {
		CrimsonRoom *room = find_room(network, get_irc_parameter(message, 1));
		if (room) {
			g_free(room->topic);
			room->topic = g_strdup(get_last_parameter(message));
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
	/* Name lists, topic details and metadata replies only update the state */
	static const int QUIET[] = {5, 333, 353, 761, 766, 770, 771, 772, 774, 900, 903};
	for (guint index = 0; index < G_N_ELEMENTS(QUIET); index++) {
		if (number == QUIET[index])
			return;
	}
	add_status(network->server_room, message, text);
	/* A missing message of the day and a taken nick during registration are routine, and an unknown command
	 * answers something the app sent on its own, not something the user did */
	if (number >= 400 && number < 600 && number != 421 && number != 422 && number != 433)
		network->session->chat->events.show_problem(get_last_parameter(message), network->session->chat->data);
}


static void deliver_standard_reply(CrimsonNetwork *network, const IrcMessage *message) {
	/* Profile details the app asks for and sets on every connection are quietly left out where the server lacks
	 * them, since the user did not do anything */
	if (g_strcmp0(get_irc_parameter(message, 0), "METADATA") == 0)
		return;
	add_status(network->server_room, message, get_last_parameter(message));
	if (g_str_equal(message->command, "FAIL"))
		network->session->chat->events.show_problem(get_last_parameter(message), network->session->chat->data);
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


static void deliver_metadata(CrimsonNetwork *network, const IrcMessage *message) {
	gboolean is_reply = g_str_equal(message->command, "761");
	const char *target = get_irc_parameter(message, is_reply ? 1 : 0);
	CrimsonRoom *room = find_room(network, target);
	if (room)
		announce_room_change(room);
	/* The user's own details change how the profile and the network show them */
	IrcState *state = get_network_state(network);
	if (target && state && (g_str_equal(target, "*") || is_own_nick(state, target)))
		announce_network_change(network);
}


static void deliver_invite(CrimsonNetwork *network, const IrcMessage *message) {
	g_autofree char *sender = get_source_nick(message->source);
	g_autofree char *text = g_strdup_printf(TEXT_INVITED, sender, get_irc_parameter(message, 1));
	add_status(network->server_room, message, text);
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
	else if (g_str_equal(command, "METADATA") || g_str_equal(command, "761"))
		deliver_metadata(network, message);
	else if (g_str_equal(command, "INVITE"))
		deliver_invite(network, message);
	else if (g_str_equal(command, "ERROR"))
		add_status(network->server_room, message, get_last_parameter(message));
	else if (g_ascii_isdigit(*command))
		deliver_numeric(network, message);
}


void handle_network_message(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected) {
	NetworkWork *work = get_network_work(network);
	const char *batch = get_irc_tag(message, "batch");
	HistoryLoad *load = batch ? g_hash_table_lookup(work->history, batch) : NULL;
	if (g_str_equal(message->command, "BATCH")) {
		const char *reference = get_irc_parameter(message, 0);
		if (reference && *reference == '+')
			start_batch(network, message);
		else if (reference && *reference == '-')
			finish_batch(network, message, load);
		return;
	}
	if (batch && gather_multiline(network, message))
		return;
	if (load && !load->room)
		return;
	if (load)
		note_oldest_line(load, message);
	handle_message_in(network, message, affected, load);
}


void request_latest_history(CrimsonRoom *room) {
	CrimsonNetwork *network = room->network;
	if (room->kind == ROOM_SERVER || room->is_loading_history || !can_network(network, "draft/chathistory"))
		return;
	CrimsonMessage *last = get_last_message(room);
	g_autofree char *after = NULL;
	if (last && room->has_history) {
		g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(last->time);
		g_autofree char *moment = format_irc_time(time);
		after = g_strconcat("timestamp=", moment, NULL);
	}
	g_autofree char *limit = g_strdup_printf("%d", HISTORY_PAGE);
	room->is_loading_history = TRUE;
	send_irc_client_command(network->client, "CHATHISTORY", "LATEST", room->name, after ? after : "*", limit, NULL);
}


void load_older_history(CrimsonRoom *room) {
	CrimsonNetwork *network = room->network;
	if (room->kind == ROOM_SERVER || room->is_loading_history || room->has_all_history
		|| !can_network(network, "draft/chathistory"))
		return;
	g_autoptr(CrimsonMessage) first = g_list_model_get_item(G_LIST_MODEL(room->messages), 0);
	if (!first)
		return;
	g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc_usec(first->time);
	g_autofree char *moment = format_irc_time(time);
	g_autofree char *before = first->id ? g_strconcat("msgid=", first->id, NULL)
		: g_strconcat("timestamp=", moment, NULL);
	g_autofree char *limit = g_strdup_printf("%d", HISTORY_PAGE);
	room->is_loading_history = TRUE;
	send_irc_client_command(network->client, "CHATHISTORY", "BEFORE", room->name, before, limit, NULL);
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
