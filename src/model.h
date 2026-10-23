#pragma once

#include <gio/gio.h>

#include "irc/client.h"

typedef struct Session Session;

/* What a line in a conversation is, which decides how it is drawn */
typedef enum {
	MESSAGE_TEXT,
	MESSAGE_ACTION,
	MESSAGE_NOTICE,
	/* Joins, parts, nick changes and topic changes, drawn as small centred notes */
	MESSAGE_EVENT,
	/* Replies from the server and failures, drawn as notes in the server room */
	MESSAGE_STATUS,
} MessageKind;

/* Whether the person of a private chat is on the network, as MONITOR reports */
typedef enum {
	PRESENCE_UNKNOWN,
	PRESENCE_ONLINE,
	PRESENCE_OFFLINE,
} Presence;

/* What the server last said about the person in a private chat */
typedef enum {
	ACTIVITY_UNKNOWN,
	ACTIVITY_SEEN,
	ACTIVITY_AWAY,
	ACTIVITY_GONE,
} Activity;

typedef enum {
	ROOM_SERVER,
	ROOM_CHANNEL,
	ROOM_PRIVATE,
} RoomKind;

#define CRIMSON_TYPE_MESSAGE (crimson_message_get_type())
G_DECLARE_FINAL_TYPE(CrimsonMessage, crimson_message, CRIMSON, MESSAGE, GObject)

/* One line of a conversation, which signals changed when its reactions, delivery or redaction change */
struct _CrimsonMessage {
	GObject parent;
	MessageKind kind;
	char *id, *label, *sender, *text, *reply_to;
	/* Microseconds since the epoch, from the server's time tag */
	gint64 time;
	/* Reactions to the arrays of nicks who sent them, in the order they arrived */
	GHashTable *reactions;
	GPtrArray *reaction_order;
	gboolean is_own, is_pending, is_redacted, is_highlight, is_bot;
	/* Arrived while the room was live, so its bubble pops in once when first drawn */
	gboolean is_fresh;
};

#define CRIMSON_TYPE_ROOM (crimson_room_get_type())
G_DECLARE_FINAL_TYPE(CrimsonRoom, crimson_room, CRIMSON, ROOM, GObject)

typedef struct _CrimsonNetwork CrimsonNetwork;

/* A channel, a private chat or a network's server room, which signals changed when its list entry changes */
struct _CrimsonRoom {
	GObject parent;
	CrimsonNetwork *network;
	RoomKind kind;
	char *name, *topic;
	GListStore *messages;
	/* Message ids to messages, for replies, reactions, redactions and repeats from history */
	GHashTable *by_id;
	/* The latest line and who wrote it, for the room list */
	char *preview, *preview_sender;
	gboolean is_preview_own;
	gint64 last_time, read_time;
	guint unread_count;
	gboolean has_mention, is_joined, is_loading_history, has_all_history, has_history;
	Presence presence;
	/* For a private chat, when the person was last active and when the server was last asked, in real time */
	Activity activity;
	gint64 last_active, activity_checked;
	/* Nicks to the monotonic moment their typing notice runs out */
	GHashTable *typing;
};

#define CRIMSON_TYPE_NETWORK (crimson_network_get_type())
G_DECLARE_FINAL_TYPE(CrimsonNetwork, crimson_network, CRIMSON, NETWORK, GObject)

typedef enum {
	NETWORK_DISCONNECTED,
	NETWORK_CONNECTING,
	NETWORK_CONNECTED,
} NetworkState;

/* One IRC network, reached directly or through a bouncer, which signals changed when its name or state changes */
struct _CrimsonNetwork {
	GObject parent;
	Session *session;
	/* The account id, followed by the bouncer's network id for a network on a bouncer */
	char *key;
	char *name, *bouncer_id, *error, *icon_url;
	NetworkState state;
	IrcClient *client;
	GListStore *rooms;
	CrimsonRoom *server_room;
	/* Attributes the bouncer reported, kept to fill the network editor */
	GHashTable *attributes;
};

#define CRIMSON_TYPE_ACCOUNT (crimson_account_get_type())
G_DECLARE_FINAL_TYPE(CrimsonAccount, crimson_account, CRIMSON, ACCOUNT, GObject)

/* A server or bouncer the user added, as the rail lists it, which signals changed when its name, state or
 * networks change */
struct _CrimsonAccount {
	GObject parent;
	Session *session;
};

CrimsonAccount *create_account_item(Session *session);
void announce_account_change(CrimsonAccount *account);

CrimsonMessage *create_message(MessageKind kind, const char *sender, const char *text, gint64 time);
void announce_message_change(CrimsonMessage *message);
/* Adds or removes one nick's reaction, returning whether anything changed */
gboolean change_reaction(CrimsonMessage *message, const char *reaction, const char *nick, gboolean is_adding);
gboolean has_reacted(CrimsonMessage *message, const char *reaction, const char *nick);

CrimsonRoom *create_room(CrimsonNetwork *network, RoomKind kind, const char *name);
void announce_room_change(CrimsonRoom *room);
/* Puts a message in time order, or returns FALSE when the room already holds its id */
gboolean insert_room_message(CrimsonRoom *room, CrimsonMessage *message);
CrimsonMessage *find_room_message(CrimsonRoom *room, const char *id);
CrimsonMessage *get_last_message(CrimsonRoom *room);
/* Counts the unread messages again after the read marker or the messages changed */
void count_unread(CrimsonRoom *room);
gboolean is_room_typing(CrimsonRoom *room);

CrimsonNetwork *create_network(Session *session, const char *key, const char *name);
void announce_network_change(CrimsonNetwork *network);
/* Network services such as NickServ and SaslServ, whose messages belong with the server's own */
gboolean is_service_nick(const char *nick);
CrimsonRoom *find_room(CrimsonNetwork *network, const char *name);
CrimsonRoom *ensure_room(CrimsonNetwork *network, RoomKind kind, const char *name);
void remove_room(CrimsonNetwork *network, CrimsonRoom *room);
guint count_network_unread(CrimsonNetwork *network, gboolean *has_mention);
IrcState *get_network_state(CrimsonNetwork *network);
