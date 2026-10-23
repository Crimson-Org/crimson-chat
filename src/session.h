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
	/* A bouncer created an invite, with its token, its uses or * for unlimited, and its website's port */
	void (*invite_ready)(Session *session, const char *token, const char *uses, const char *port, gpointer data);
	/* A network is about to go, whatever removed it, so nothing keeps pointing into it */
	void (*network_removed)(CrimsonNetwork *network, gpointer data);
	/* The network's nickname service asked the user to log in, and the app has no login for it */
	void (*login_wanted)(CrimsonNetwork *network, gpointer data);
} ChatEvents;

/* Every account and the networks they reach, in the order the rail shows them */
struct Chat {
	GListStore *networks;
	/* The servers and bouncers the user added, as CrimsonAccount items in the order the rail shows them */
	GListStore *accounts;
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
	char *control_error;
	GPtrArray *networks;
	CrimsonAccount *item;
	/* Whether the account ever got through, after which dropped connections only reconnect quietly */
	gboolean has_reported_failure, has_connected;
	/* A bouncer admin can invite people and open the bouncer's admin page on this port */
	gboolean is_admin;
	char *web_port;
	/* The bouncer's full list of networks while it arrives, by batch reference and the ids it named */
	char *listing_batch;
	GHashTable *listed_ids;
};

/* A channel a server listed, for the room picker */
typedef struct {
	char *name, *topic;
	guint users;
} ListedRoom;

/* Hands over the listed rooms, most people first, or NULL when the network went away before the end */
typedef void (*RoomListingReady)(GPtrArray *rooms, gpointer data);
/* The server lists its rooms only after the user has been connected a while, and is asked again in this many seconds */
typedef void (*RoomListingWait)(guint seconds, gpointer data);

/* What WHOIS tells about a person, for their profile */
typedef struct {
	char *nick, *user, *host, *realname, *server, *account, *away, *channels;
	gint64 idle_seconds;
	gboolean is_found, is_secure;
} Profile;

/* Hands over the profile, or NULL when the network went away before the answer */
typedef void (*ProfileReady)(Profile *profile, gpointer data);

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
	/* The channel list being collected for the room picker, and who waits for it */
	GPtrArray *listing;
	RoomListingReady listing_ready;
	RoomListingWait listing_wait;
	gpointer listing_data;
	/* How long the server said to wait before listing, and the timer that asks again */
	guint listing_delay, listing_retry_source;
	/* Whether the list asked only for busy channels, so a short answer is asked again in full */
	gboolean is_listing_busy;
	/* Whether the user was already offered to log in on this connection */
	gboolean has_offered_login;
	/* The profile being collected from WHOIS replies, and who waits for it */
	Profile *profile;
	ProfileReady profile_ready;
	gpointer profile_data;
} NetworkWork;

/* Frees what a network collects while a history batch arrives */
void free_history_load(gpointer load);
Chat *create_chat(const ChatEvents *events, gpointer data);
void start_chat(Chat *chat);
void stop_chat(Chat *chat);
Session *add_account(Chat *chat, AccountRecord *record);
void change_account(Session *session, const AccountRecord *record);
void remove_account(Session *session);
void connect_session(Session *session);
const char *describe_account_name(Session *session);
NetworkState read_account_state(Session *session);
const char *read_account_error(Session *session);
guint count_account_unread(Session *session, gboolean *has_mention);
void trust_certificate(Session *session, const char *hash);
void save_chat_accounts(Chat *chat);
/* Saves the room list soon, so many changes in a row write the file once */
void save_chat_cache_later(Chat *chat);
NetworkWork *get_network_work(CrimsonNetwork *network);
/* The one bouncer the user is connected to, if any */
Session *find_bouncer(Chat *chat);
/* Asks the server for its channels, the busy ones only when it can filter them */
void request_room_listing(CrimsonNetwork *network, RoomListingReady ready, RoomListingWait wait, gpointer data);
void cancel_room_listing(CrimsonNetwork *network);
/* Takes the server's channel list replies while a listing is wanted, returning whether it took the line */
gboolean collect_room_listing(CrimsonNetwork *network, int number, const IrcMessage *message);
void request_profile(CrimsonNetwork *network, const char *nick, ProfileReady ready, gpointer data);
/* Stops waiting for a profile, if the one asked for is still this caller's */
void cancel_profile(CrimsonNetwork *network, gpointer data);
/* Takes WHOIS replies about the person a profile was asked for, returning whether it took the line */
gboolean collect_profile(CrimsonNetwork *network, int number, const IrcMessage *message);

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
/* Why a network that is not connected went down, from the bouncer for a network on one */
const char *read_network_error(CrimsonNetwork *network);
/* Whether a network that is not connected will be tried again by itself, as its bouncer or its connection says */
gboolean is_network_retrying(CrimsonNetwork *network);
gint64 read_message_time(const IrcMessage *message);
