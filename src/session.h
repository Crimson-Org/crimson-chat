#pragma once

#include "model.h"
#include "storage.h"

/* How many messages people wrote one page of history holds, about a tall chat full */
#define HISTORY_PAGE 30
/* How many messages each step of a jump to an old message loads */
#define HISTORY_JUMP_PAGE 400

typedef struct Chat Chat;

/* What the interface hears from the protocol side */
typedef struct {
	void (*notify)(CrimsonRoom *room, CrimsonMessage *message, gpointer data);
	void (*certificate_refused)(Session *session, const char *hash, gpointer data);
	/* A short line for the user, such as a problem the server reports */
	void (*show_message)(const char *text, gpointer data);
	/* A room the user asked for, such as a channel just joined or a private chat just opened */
	void (*room_opened)(CrimsonRoom *room, gpointer data);
	/* A bouncer created an invite, with its token, its uses or * for unlimited, and its website's port */
	void (*invite_ready)(Session *session, const char *token, const char *uses, const char *port, gpointer data);
	/* A network is about to go, whatever removed it, so nothing keeps pointing into it */
	void (*network_removed)(CrimsonNetwork *network, gpointer data);
	/* A room was read, here or on another device, so its desktop notifications are no longer news */
	void (*room_read)(CrimsonRoom *room, gpointer data);
	/* The user asked which rooms the server has, as with /list */
	void (*rooms_requested)(CrimsonNetwork *network, gpointer data);
	/* Someone invited the user to a room, which the user can join from there */
	void (*room_invited)(CrimsonNetwork *network, const char *room_name, gpointer data);
	/* The user asked about someone, as with /whois, which shows their profile */
	void (*profile_requested)(CrimsonRoom *room, const char *nick, gpointer data);
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

/* Hands over the messages that say what was searched for, as CrimsonMessage items newest first, or NULL when the
 * network went away before the answer */
typedef void (*SearchReady)(GPtrArray *messages, gpointer data);

/* How a registration went: done and logged in, waiting for a code the server sent, or refused for a reason */
typedef enum {
	REGISTRATION_DONE,
	REGISTRATION_NEEDS_CODE,
	REGISTRATION_FAILED,
	/* The nickname already belongs to an account, which wants logging into instead */
	REGISTRATION_EXISTS,
} RegistrationResult;

/* Hears how a registration went, with the server's own words, possibly several times as its replies arrive */
typedef void (*RegistrationReply)(RegistrationResult result, const char *message, gpointer data);

/* Something the user sent, whose replies from the server are theirs to hear */
typedef struct {
	/* The label the server repeats on its replies, or the PING token sent around them where it has no labels */
	char *label;
	/* The folded person or room it was about, or NULL */
	char *target;
	/* The monotonic moment after which replies are no longer its answers */
	gint64 until;
	/* PONGs seen for its token, the first opening its replies and the second closing them */
	guint pongs;
	gboolean is_answered, was_typed_in_server, has_shown_answer;
} Question;

/* Work in progress on a network, kept per connection */
typedef struct {
	/* Batch references to the history loads and multiline messages being collected */
	GHashTable *history, *multiline;
	/* Batch references to netsplits and netjoins, each counting the people per room it took or brought back */
	GHashTable *splits;
	/* Pending own messages by label, waiting for their echo */
	GHashTable *pending;
	/* What the user sent that may still get replies, oldest first, the one being sent now and how deeply its
	 * sending nests, and labeled reply batches to the labels they answer */
	GPtrArray *questions;
	Question *asking;
	guint asking_depth;
	GHashTable *answer_batches;
	/* The message of the day while its lines arrive */
	GString *welcome_message;
	/* The search waiting for the bouncer's answer, with its room and words, the batch bringing it, what it found so far
	 * and who waits for it */
	char *search_room, *search_words, *search_batch;
	GPtrArray *search_results;
	SearchReady search_ready;
	gpointer search_data;
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
	/* Whether the quiet WHOIS that looks for NickServ is still out */
	gboolean is_probing_nickserv;
	/* A registration being made, who waits for it, the login it saves once done, and for NickServ the command its
	 * code goes with */
	RegistrationReply registration_reply;
	gpointer registration_data;
	char *registering_account, *registering_password, *code_command;
	gboolean is_nickserv_registration;
	/* People and rooms the server said it cannot reach, by folded name, to the moment requests the app makes on its
	 * own may try them again */
	GHashTable *refused_targets;
	/* Recent WHOIS answers by folded nick, so opening a profile again soon asks the server nothing */
	GHashTable *profiles;
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
void trust_certificate(Session *session, const char *hash);
void save_chat_accounts(Chat *chat);
/* Saves the room list soon, so many changes in a row write the file once */
void save_chat_cache_later(Chat *chat);
NetworkWork *get_network_work(CrimsonNetwork *network);
/* A person or room name as the network compares them */
char *fold_network_name(CrimsonNetwork *network, const char *name);
/* Notes that the server cannot reach a person or room */
void refuse_target(CrimsonNetwork *network, const char *target);
/* Lets requests the app makes on its own try a person or room again */
void forgive_target(CrimsonNetwork *network, const char *target);
/* Whether requests the app makes on its own, such as typing notices, should leave a target alone for now */
gboolean is_target_refused(CrimsonNetwork *network, const char *target);
/* The one bouncer the user is connected to, if any */
Session *find_bouncer(Chat *chat);
/* Whether the user trusted this certificate for one of their accounts, such as a bouncer's own, which also
 * serves its pictures and files */
gboolean is_trusted_certificate(Chat *chat, GTlsCertificate *certificate);
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

/* Whether the user is logged into an account on the server, and whether one can be registered there, through the
 * IRCv3 commands or NickServ */
gboolean is_logged_in(CrimsonNetwork *network);
gboolean can_register_account(CrimsonNetwork *network);
/* Whether the server needs an email address to register, and whether the account may be named apart from the nick */
gboolean is_email_required(CrimsonNetwork *network);
gboolean allows_custom_account_name(CrimsonNetwork *network);
void request_registration(CrimsonNetwork *network, const char *account, const char *email, const char *password,
	RegistrationReply reply, gpointer data);
void send_registration_code(CrimsonNetwork *network, const char *code);
void cancel_registration(CrimsonNetwork *network);
/* Takes the replies a registration and the NickServ probe are waiting for, and says whether to keep them quiet */
gboolean collect_registration(CrimsonNetwork *network, const IrcMessage *message);

void add_bouncer_network(Session *session, GHashTable *attributes);
void change_bouncer_network(CrimsonNetwork *network, GHashTable *attributes);
void delete_bouncer_network(CrimsonNetwork *network);
void reconnect_network(CrimsonNetwork *network);

/* Incoming */
void handle_network_message(CrimsonNetwork *network, const IrcMessage *message, GPtrArray *affected);
void request_latest_history(CrimsonRoom *room);
/* Asks for this many messages before the earliest one loaded, a page for scrolling and many for a far jump */
void load_older_history(CrimsonRoom *room, int count);
void request_private_targets(CrimsonNetwork *network);
/* Keeps the joined channels of a direct connection, which it joins again on the next start */
void save_network_channels(CrimsonNetwork *network);

/* Finds the messages in a room that say the words, through the bouncer's whole history where there is one and in
 * the messages loaded so far otherwise, answering later either way */
void search_room_messages(CrimsonRoom *room, const char *words, SearchReady ready, gpointer data);
/* Stops waiting for a search, if the one asked for is still this caller's */
void cancel_message_search(CrimsonNetwork *network, gpointer data);

/* Outgoing */
void send_room_text(CrimsonRoom *room, const char *text, CrimsonMessage *reply_to);
/* Sends a line as it is, without turning Markdown into formatting, such as one naming a shared file */
void send_room_line(CrimsonRoom *room, const char *text);
void send_reaction(CrimsonRoom *room, CrimsonMessage *message, const char *reaction);
/* Whether the server passes on a client tag such as react or reply, from its CLIENTTAGDENY */
gboolean allows_client_tag(CrimsonNetwork *network, const char *name);
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
/* What kind of problem stopped a network or account that is down, as its bouncer or its connection says */
IrcFailure read_network_failure(CrimsonNetwork *network);
IrcFailure read_account_failure(Session *session);
gint64 read_message_time(const IrcMessage *message);
