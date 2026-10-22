#pragma once

#include "model.h"
#include "storage.h"

#define HISTORY_PAGE 60

typedef struct Chat Chat;

/* What the interface hears from the protocol side */
typedef struct {
	void (*notify)(CrimsonRoom *room, CrimsonMessage *message, gpointer data);
	void (*certificate_refused)(Session *session, const char *hash, gpointer data);
	void (*show_problem)(const char *text, gpointer data);
	/* A room the user asked for, such as a channel just joined or a private chat just opened */
	void (*room_opened)(CrimsonRoom *room, gpointer data);
	/* A bouncer created an invite, with its token and its uses or * for unlimited */
	void (*invite_ready)(Session *session, const char *token, const char *uses, gpointer data);
} ChatEvents;

/* Every account and the networks they reach, in the order the rail shows them */
struct Chat {
	GListStore *networks;
	GPtrArray *sessions;
	ChatEvents events;
	gpointer data;
	guint cache_source;
	/* Whether the saved accounts are loaded, before which an empty list only means they are on the way */
	gboolean has_started;
};

/* One saved account, with a connection per network and a managing connection for a bouncer */
struct Session {
	Chat *chat;
	AccountRecord *record;
	IrcClient *control;
	GPtrArray *networks;
};

/* Work in progress on a network, kept per connection */
typedef struct {
	/* Batch references to the history loads and multiline messages being collected */
	GHashTable *history, *multiline;
	/* Pending own messages by label, waiting for their echo */
	GHashTable *pending;
	CrimsonRoom *whois_room;
	/* A channel the user asked to join, opened once the server confirms it */
	char *opening_room;
	guint typing_source;
} NetworkWork;

/* Frees what a network collects while a history batch arrives */
void free_history_load(gpointer load);
Chat *create_chat(const ChatEvents *events, gpointer data);
void start_chat(Chat *chat);
void stop_chat(Chat *chat);
Session *add_account(Chat *chat, AccountRecord *record);
void change_account(Session *session, const AccountRecord *record);
void remove_account(Session *session);
void trust_certificate(Session *session, const char *hash);
void save_chat_accounts(Chat *chat);
/* Saves the room list soon, so many changes in a row write the file once */
void save_chat_cache_later(Chat *chat);
NetworkWork *get_network_work(CrimsonNetwork *network);

void add_bouncer_network(Session *session, GHashTable *attributes);
void change_bouncer_network(CrimsonNetwork *network, GHashTable *attributes);
void delete_bouncer_network(CrimsonNetwork *network);
void reconnect_network(CrimsonNetwork *network);

/* Incoming */
void handle_network_message(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected);
void request_latest_history(CrimsonRoom *room);
void load_older_history(CrimsonRoom *room);
void request_private_targets(CrimsonNetwork *network);
/* Keeps the joined channels of a direct connection, which it joins again on the next start */
void save_network_channels(CrimsonNetwork *network);

/* Outgoing */
void send_room_text(CrimsonRoom *room, const char *text, CrimsonMessage *reply_to);
void send_reaction(CrimsonRoom *room, CrimsonMessage *message, const char *reaction);
void redact_message(CrimsonRoom *room, CrimsonMessage *message);
void send_typing(CrimsonRoom *room, const char *state);
void mark_room_read(CrimsonRoom *room);
void join_channel(CrimsonNetwork *network, const char *name, const char *key);
void leave_room(CrimsonRoom *room);
CrimsonRoom *open_private_room(CrimsonNetwork *network, const char *nick);
gboolean can_network(CrimsonNetwork *network, const char *cap);
gint64 read_message_time(const IrcMessage *message);
