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

/* Which page of history a room waits for, if any, since catching up after a reconnect never reaches the start */
typedef enum {
	HISTORY_NONE,
	HISTORY_LATEST,
	HISTORY_NEWER,
	HISTORY_OLDER,
} HistoryRequest;

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
	/* Why an own message never arrived, as the server put it in plain words, or NULL */
	char *problem;
	/* Whether the message glows for a moment after a search or a reply took the user to it */
	gboolean is_spotlit;
	/* Someone else joining, leaving, quitting, renaming or changing modes, kept always and shown by the setting */
	gboolean is_membership;
	/* The window action clicking the line runs for its server, such as opening the login, or NULL */
	const char *action;
	/* The monotonic moment a live message arrived, or 0 for one loaded from history */
	gint64 arrival_time;
	/* A file of this many bytes on its way to the bouncer, shown as a card under its name until it is sent, and how
	 * much of it has gone from 0 to 1 */
	gboolean is_uploading;
	goffset upload_size;
	double upload_progress;
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
	gboolean has_mention, is_joined, has_all_history, has_history;
	/* Why the room refused the user on this connection, such as needing a login, until a join or login, or NULL */
	char *refusal;
	HistoryRequest history_request;
	Presence presence;
	/* For a private chat, when the person was last active and when the server was last asked, in real time */
	Activity activity;
	gint64 last_active, activity_checked;
	/* For a server room, whether the user opened its messages, which lists it among the chats until hidden */
	gboolean is_listed;
	/* Nicks to the monotonic moment their typing notice runs out */
	GHashTable *typing;
	/* Someone's newest reaction to one of the user's own messages, unseen until the room is read: the reaction, the
	 * line the list shows for it and when it came */
	char *new_reaction, *new_reaction_text;
	gint64 new_reaction_time;
	/* The messages of the history pages being received, added together once the last page ends, and whether the
	 * room changed meanwhile */
	GPtrArray *batch;
	guint batch_depth;
	gboolean has_batch_change;
};

/* The profile keys of the IRCv3 metadata registry that people set for themselves, which the app keeps and sets
 * again on each connection */
#define OWN_PROFILE_KEYS "display-name", "avatar", "status", "pronouns", "homepage"

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
	/* The server was connected at some point since the app started, so losing it later is a reconnection */
	gboolean has_connected;
	/* When the connection last came up, in monotonic time, so the offer to register waits a moment after it */
	gint64 connected_since;
	/* Whether the server has a NickServ to register with, for servers without the IRCv3 commands, and whether its
	 * NickServ said the nickname belongs to an account the user is not logged into */
	gboolean has_nickserv, wants_login;
	IrcClient *client;
	GListStore *rooms;
	CrimsonRoom *server_room;
	/* Attributes the bouncer reported, kept to fill the network editor */
	GHashTable *attributes;
	/* Avatar addresses of people and rooms by lowercase name as last seen, so their pictures show before the server
	 * sends them again */
	GHashTable *known_avatars;
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
/* Holds the messages of a history page and the room's changes until the page ends, so a chat takes the page in one
 * change */
void begin_room_batch(CrimsonRoom *room);
void end_room_batch(CrimsonRoom *room);
CrimsonMessage *find_room_message(CrimsonRoom *room, const char *id);
/* An upload's card goes at the end of the room without becoming the room's latest line, and goes away once sent */
void add_upload_message(CrimsonRoom *room, CrimsonMessage *message);
/* Marks the user's messages still waiting in the room as not sent, for the reason in plain words */
void fail_pending_messages(CrimsonRoom *room, const char *problem);
void fail_pending_message(CrimsonMessage *message, const char *problem);
void remove_room_message(CrimsonRoom *room, CrimsonMessage *message);
/* The newest message that arrived, leaving out the user's messages still on their way or never sent */
CrimsonMessage *get_last_message(CrimsonRoom *room);
/* The newest message the server keeps, with its own id, where catching up on history starts */
CrimsonMessage *get_last_stored_message(CrimsonRoom *room);
/* Counts the unread messages again after the read marker or the messages changed */
void count_unread(CrimsonRoom *room);
gboolean is_room_typing(CrimsonRoom *room);
/* Whether a message arrived just now, which however often its row is drawn in that moment makes it rise in */
gboolean is_message_arriving(CrimsonMessage *message);

CrimsonNetwork *create_network(Session *session, const char *key, const char *name);
void announce_network_change(CrimsonNetwork *network);
/* Network services such as NickServ and SaslServ, whose messages belong with the server's own */
gboolean is_service_nick(const char *nick);
CrimsonRoom *find_room(CrimsonNetwork *network, const char *name);
CrimsonRoom *ensure_room(CrimsonNetwork *network, RoomKind kind, const char *name);
void remove_room(CrimsonNetwork *network, CrimsonRoom *room);
/* Forgets the unseen reaction once the room is read, and says whether there was one */
gboolean clear_new_reaction(CrimsonRoom *room);
IrcState *get_network_state(CrimsonNetwork *network);
