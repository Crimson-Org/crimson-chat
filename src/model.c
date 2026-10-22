#include "model.h"

enum {
	SIGNAL_CHANGED,
	SIGNAL_COUNT,
};

static guint message_signals[SIGNAL_COUNT], room_signals[SIGNAL_COUNT], network_signals[SIGNAL_COUNT];
static guint account_signals[SIGNAL_COUNT];

G_DEFINE_FINAL_TYPE(CrimsonMessage, crimson_message, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(CrimsonRoom, crimson_room, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(CrimsonNetwork, crimson_network, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(CrimsonAccount, crimson_account, G_TYPE_OBJECT)


static guint add_changed_signal(GType type) {
	return g_signal_new("changed", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void crimson_account_class_init(CrimsonAccountClass *class) {
	account_signals[SIGNAL_CHANGED] = add_changed_signal(CRIMSON_TYPE_ACCOUNT);
}


static void crimson_account_init(CrimsonAccount *account) {
}


CrimsonAccount *create_account_item(Session *session) {
	CrimsonAccount *account = g_object_new(CRIMSON_TYPE_ACCOUNT, NULL);
	account->session = session;
	return account;
}


void announce_account_change(CrimsonAccount *account) {
	g_signal_emit(account, account_signals[SIGNAL_CHANGED], 0);
}


static void finalize_message(GObject *object) {
	CrimsonMessage *message = CRIMSON_MESSAGE(object);
	g_free(message->id);
	g_free(message->label);
	g_free(message->sender);
	g_free(message->text);
	g_free(message->reply_to);
	g_hash_table_unref(message->reactions);
	g_ptr_array_unref(message->reaction_order);
	G_OBJECT_CLASS(crimson_message_parent_class)->finalize(object);
}


static void crimson_message_class_init(CrimsonMessageClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_message;
	message_signals[SIGNAL_CHANGED] = add_changed_signal(CRIMSON_TYPE_MESSAGE);
}


static void crimson_message_init(CrimsonMessage *message) {
	message->reactions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	message->reaction_order = g_ptr_array_new_with_free_func(g_free);
}


CrimsonMessage *create_message(MessageKind kind, const char *sender, const char *text, gint64 time) {
	CrimsonMessage *message = g_object_new(CRIMSON_TYPE_MESSAGE, NULL);
	message->kind = kind;
	message->sender = g_strdup(sender);
	message->text = g_strdup(text ? text : "");
	message->time = time;
	return message;
}


void announce_message_change(CrimsonMessage *message) {
	g_signal_emit(message, message_signals[SIGNAL_CHANGED], 0);
}


gboolean has_reacted(CrimsonMessage *message, const char *reaction, const char *nick) {
	GPtrArray *nicks = g_hash_table_lookup(message->reactions, reaction);
	return nicks && g_ptr_array_find_with_equal_func(nicks, nick, (GEqualFunc)g_str_equal, NULL);
}


gboolean change_reaction(CrimsonMessage *message, const char *reaction, const char *nick, gboolean is_adding) {
	GPtrArray *nicks = g_hash_table_lookup(message->reactions, reaction);
	guint index = 0;
	gboolean is_present = nicks && g_ptr_array_find_with_equal_func(nicks, nick, (GEqualFunc)g_str_equal, &index);
	if (is_adding == is_present)
		return FALSE;
	if (!is_adding) {
		g_ptr_array_remove_index(nicks, index);
		if (!nicks->len) {
			g_hash_table_remove(message->reactions, reaction);
			guint position = 0;
			if (g_ptr_array_find_with_equal_func(message->reaction_order, reaction, (GEqualFunc)g_str_equal,
				&position))
				g_ptr_array_remove_index(message->reaction_order, position);
		}
		return TRUE;
	}
	if (!nicks) {
		nicks = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(message->reactions, g_strdup(reaction), nicks);
		g_ptr_array_add(message->reaction_order, g_strdup(reaction));
	}
	g_ptr_array_add(nicks, g_strdup(nick));
	return TRUE;
}


static void finalize_room(GObject *object) {
	CrimsonRoom *room = CRIMSON_ROOM(object);
	g_free(room->name);
	g_free(room->topic);
	g_free(room->preview);
	g_free(room->preview_sender);
	g_object_unref(room->messages);
	g_hash_table_unref(room->by_id);
	g_hash_table_unref(room->typing);
	G_OBJECT_CLASS(crimson_room_parent_class)->finalize(object);
}


static void crimson_room_class_init(CrimsonRoomClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_room;
	room_signals[SIGNAL_CHANGED] = add_changed_signal(CRIMSON_TYPE_ROOM);
}


static void crimson_room_init(CrimsonRoom *room) {
	room->messages = g_list_store_new(CRIMSON_TYPE_MESSAGE);
	room->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	room->typing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}


CrimsonRoom *create_room(CrimsonNetwork *network, RoomKind kind, const char *name) {
	CrimsonRoom *room = g_object_new(CRIMSON_TYPE_ROOM, NULL);
	room->network = network;
	room->kind = kind;
	room->name = g_strdup(name);
	return room;
}


void announce_room_change(CrimsonRoom *room) {
	g_signal_emit(room, room_signals[SIGNAL_CHANGED], 0);
}


static gboolean counts_as_unread(CrimsonMessage *message) {
	return !message->is_own && message->kind != MESSAGE_EVENT && message->kind != MESSAGE_STATUS
		&& !message->is_redacted;
}


void count_unread(CrimsonRoom *room) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
	room->unread_count = 0;
	room->has_mention = FALSE;
	for (guint index = count; index > 0; index--) {
		g_autoptr(CrimsonMessage) message = g_list_model_get_item(G_LIST_MODEL(room->messages), index - 1);
		if (message->time <= room->read_time)
			break;
		if (!counts_as_unread(message))
			continue;
		room->unread_count++;
		room->has_mention |= message->is_highlight || room->kind == ROOM_PRIVATE;
	}
}


/* Finds where a message belongs, after every message at the same moment or earlier */
static guint find_insert_position(CrimsonRoom *room, gint64 time) {
	GListModel *messages = G_LIST_MODEL(room->messages);
	guint low = 0, high = g_list_model_get_n_items(messages);
	if (high) {
		g_autoptr(CrimsonMessage) last = g_list_model_get_item(messages, high - 1);
		if (last->time <= time)
			return high;
	}
	while (low < high) {
		guint middle = low + (high - low) / 2;
		g_autoptr(CrimsonMessage) message = g_list_model_get_item(messages, middle);
		if (message->time <= time)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}


gboolean insert_room_message(CrimsonRoom *room, CrimsonMessage *message) {
	if (message->id && g_hash_table_contains(room->by_id, message->id))
		return FALSE;
	if (message->id)
		g_hash_table_insert(room->by_id, g_strdup(message->id), message);
	guint position = find_insert_position(room, message->time);
	g_list_store_insert(room->messages, position, message);
	/* The message after a new one may now continue its group or lose its date line, so it is drawn again */
	g_autoptr(CrimsonMessage) next = g_list_model_get_item(G_LIST_MODEL(room->messages), position + 1);
	if (next)
		g_list_store_splice(room->messages, position + 1, 1, (gpointer *)&next, 1);
	if (message->time >= room->last_time && message->kind != MESSAGE_EVENT) {
		room->last_time = message->time;
		g_free(room->preview);
		room->preview = g_strdup(message->text);
		g_free(room->preview_sender);
		room->preview_sender = message->kind == MESSAGE_STATUS ? NULL : g_strdup(message->sender);
		room->is_preview_own = message->is_own;
	}
	return TRUE;
}


CrimsonMessage *find_room_message(CrimsonRoom *room, const char *id) {
	return id ? g_hash_table_lookup(room->by_id, id) : NULL;
}


CrimsonMessage *get_last_message(CrimsonRoom *room) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
	if (!count)
		return NULL;
	CrimsonMessage *message = g_list_model_get_item(G_LIST_MODEL(room->messages), count - 1);
	g_object_unref(message);
	return message;
}


gboolean is_room_typing(CrimsonRoom *room) {
	return g_hash_table_size(room->typing) > 0;
}


static void finalize_network(GObject *object) {
	CrimsonNetwork *network = CRIMSON_NETWORK(object);
	g_free(network->key);
	g_free(network->name);
	g_free(network->bouncer_id);
	g_free(network->error);
	g_free(network->icon_url);
	g_clear_pointer(&network->client, free_irc_client);
	g_object_unref(network->rooms);
	g_clear_object(&network->server_room);
	g_hash_table_unref(network->attributes);
	G_OBJECT_CLASS(crimson_network_parent_class)->finalize(object);
}


static void crimson_network_class_init(CrimsonNetworkClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_network;
	network_signals[SIGNAL_CHANGED] = add_changed_signal(CRIMSON_TYPE_NETWORK);
}


static void crimson_network_init(CrimsonNetwork *network) {
	network->rooms = g_list_store_new(CRIMSON_TYPE_ROOM);
	network->attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}


CrimsonNetwork *create_network(Session *session, const char *key, const char *name) {
	CrimsonNetwork *network = g_object_new(CRIMSON_TYPE_NETWORK, NULL);
	network->session = session;
	network->key = g_strdup(key);
	network->name = g_strdup(name);
	network->server_room = create_room(network, ROOM_SERVER, name);
	g_list_store_append(network->rooms, network->server_room);
	return network;
}


void announce_network_change(CrimsonNetwork *network) {
	g_signal_emit(network, network_signals[SIGNAL_CHANGED], 0);
}


IrcState *get_network_state(CrimsonNetwork *network) {
	return network->client ? get_irc_client_state(network->client) : NULL;
}


CrimsonRoom *find_room(CrimsonNetwork *network, const char *name) {
	IrcState *state = get_network_state(network);
	IrcCasemap casemap = state ? state->casemap : CASEMAP_RFC1459;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (room->kind != ROOM_SERVER && are_irc_names_equal(casemap, room->name, name))
			return room;
	}
	return NULL;
}


CrimsonRoom *ensure_room(CrimsonNetwork *network, RoomKind kind, const char *name) {
	CrimsonRoom *room = find_room(network, name);
	if (room)
		return room;
	room = create_room(network, kind, name);
	g_list_store_append(network->rooms, room);
	g_object_unref(room);
	return room;
}


void remove_room(CrimsonNetwork *network, CrimsonRoom *room) {
	guint position = 0;
	if (room->kind != ROOM_SERVER && g_list_store_find(network->rooms, room, &position))
		g_list_store_remove(network->rooms, position);
}


guint count_network_unread(CrimsonNetwork *network, gboolean *has_mention) {
	guint total = 0;
	*has_mention = FALSE;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		/* What the server says stays out of the list, so it adds nothing to the badges either */
		if (room->kind == ROOM_SERVER)
			continue;
		total += room->unread_count;
		*has_mention |= room->has_mention;
	}
	return total;
}
