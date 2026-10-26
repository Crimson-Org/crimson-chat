#pragma once

#include <gio/gio.h>

#include "../irc/client.h"
#include "../irc/stream.h"
#include "database.h"

#define BOUNCER_NAME "crimson-chat-bouncer"
/* The network attribute that says whether the bouncer will reconnect a disconnected network by itself */
#define RETRY_ATTRIBUTE "crimson-chat/retrying"
/* The network attribute that says what kind of problem stopped it, login, certificate or network */
#define FAILURE_ATTRIBUTE "crimson-chat/failure"
#define HISTORY_PAGE_LIMIT 1000
/* The conversation the server's own chat is kept and asked for under, which no nick or channel can be */
#define SERVER_TARGET "*"
/* The tag a kept message of the day carries its lines in, joined by line breaks */
#define WELCOME_MESSAGE_TAG "crimson-chat/motd"
/* What others see as the user's away message while none of their devices is connected */
#define ABSENT_MESSAGE "Away right now"
/* The capability that tells Crimson Chat it reached its own bouncer, and the batch the network list comes in */
#define BOUNCER_CAP "crimson-chat/bouncer"
#define NETWORK_LISTING_BATCH "crimson-chat/networks"
/* Where the app uploads pictures and files, as a whole address or the website's port, and the largest file */
#define AVATARS_TOKEN "crimson-chat/AVATARS"
#define FILES_TOKEN "crimson-chat/FILES"
#define FILE_LIMIT_TOKEN "crimson-chat/FILE-LIMIT"

typedef struct Bouncer Bouncer;
typedef struct BouncerUser BouncerUser;
typedef struct Upstream Upstream;
typedef struct Downstream Downstream;

/* How the bouncer takes pictures people upload for themselves or their rooms: whether it does, their format,
 * quality from 0 to 100, size in pixels, the largest upload and where they are kept */
typedef struct {
	gboolean is_enabled;
	char *format, *folder;
	int quality, size;
	gsize largest_bytes;
} AvatarSettings;

/* How the bouncer keeps files people share in their chats: whether it does, the largest one, how long each is kept
 * and where */
typedef struct {
	gboolean is_enabled;
	gsize largest_bytes;
	gint64 kept_seconds;
	char *folder;
} FileSettings;

/* The running bouncer, with everyone who may log in and every client connected */
struct Bouncer {
	Database *database;
	AvatarSettings avatars;
	FileSettings files;
	/* The address the website is reached at from outside, such as https://chat.example.com:8443, when set */
	char *public_url;
	GTlsCertificate *certificate;
	GSocketService *irc_service, *web_service;
	char *irc_port, *web_port;
	GPtrArray *users;
	GList *downstreams;
	/* The name invites greet people with, and the certificate fingerprint they compare */
	char *name, *fingerprint;
	/* Web sign-in cookies to the users they belong to */
	GHashTable *web_sessions;
};

/* An account on the bouncer and the networks it keeps connected */
struct BouncerUser {
	Bouncer *bouncer;
	gint64 id;
	char *name, *password_hash;
	gboolean is_admin;
	GPtrArray *upstreams;
};

/* The bouncer's own connection to one network for one user */
struct Upstream {
	BouncerUser *user;
	NetworkRecord record;
	IrcClient *client;
	/* The message of the day while its lines arrive */
	char *welcome_message;
	/* The server's batches that answer a labeled line, by reference to the label they answer */
	GHashTable *answer_batches;
	/* Keys given with JOIN, kept with the channel once the server confirms it */
	GHashTable *channel_keys;
	/* Set while the network is being removed, so its last disconnect announces nothing */
	gboolean is_removed;
	/* Whether the server's welcome on this connection has ended, after which its replies pass on to the apps */
	gboolean is_welcomed;
	/* Whether the bouncer marked the user away because none of their devices is connected, and how many of the
	 * server's answers to those changes are still to come, which stay with the bouncer */
	gboolean is_away_for_absence;
	guint own_away_answers;
	/* Why the network last disconnected, which clients that log in later are told as well */
	char *error;
	/* The account and password a client is registering, kept as the network's login once the server agrees */
	char *registering_account, *registering_password;
};

/* A Crimson Chat app logged in to the bouncer, which reaches all of the user's networks over this one connection */
struct Downstream {
	Bouncer *bouncer;
	/* A number of its own, which labels the app sends up carry so their answers come back to it alone */
	guint id;
	IrcStream *stream;
	/* Where the client connects from, and the address of the bouncer it reached */
	char *address, *local_host;
	GHashTable *caps;
	gboolean is_negotiating, is_registered, has_cap_values;
	char *nick, *username, *password;
	GString *login;
	int failed_logins;
	guint registration_source;
	BouncerUser *user;
};

/* Bouncer */
Bouncer *create_bouncer(Database *database, GTlsCertificate *certificate, const char *name);
/* Registers a new user, returning NULL when the name is taken or not allowed */
BouncerUser *register_user(Bouncer *bouncer, const char *name, const char *password);
gboolean is_valid_user_name(const char *name);
BouncerUser *find_user(Bouncer *bouncer, gint64 id);
void set_user_admin(BouncerUser *user, gboolean is_admin);
void set_user_password(BouncerUser *user, const char *password);
/* Disconnects the user and deletes their account, networks, history and pictures */
void remove_user(BouncerUser *user);
gboolean listen_bouncer(Bouncer *bouncer, const char *irc_address, const char *web_address, GError **error);
BouncerUser *log_in_user(Bouncer *bouncer, const char *name, const char *password);
Upstream *find_upstream(BouncerUser *user, const char *network_id);
void add_upstream(BouncerUser *user, const NetworkRecord *record);
void remove_upstream(Upstream *upstream);
/* Tells every app of the user about changed attributes, or a removal when NULL */
void announce_network(Upstream *upstream, GHashTable *attributes);
void forget_downstream(Bouncer *bouncer, Downstream *downstream);
/* Marks the user away on their networks while none of their devices is connected, and back once one is, leaving an
 * away the user set themselves alone */
void follow_user_presence(BouncerUser *user);

/* Upstream */
Upstream *create_upstream(BouncerUser *user, const NetworkRecord *record);
void free_upstream(Upstream *upstream);
void restart_upstream(Upstream *upstream);
GHashTable *describe_upstream(Upstream *upstream);
const char *describe_upstream_state(Upstream *upstream);
void forward_to_upstream(Upstream *upstream, Downstream *origin, const IrcMessage *message);
char *get_upstream_nick(Upstream *upstream);

/* Downstream */
void accept_downstream(Bouncer *bouncer, GIOStream *connection, const char *address, const char *local_host);

/* Website */
void serve_web(Bouncer *bouncer, GIOStream *connection);

/* Avatars */
/* Shapes an uploaded picture, keeps it, sets it as the avatar of the user or room on the network, and returns its
 * address under the base address */
char *store_avatar(Bouncer *bouncer, BouncerUser *user, Upstream *upstream, const char *target, GBytes *data,
	const char *content_type, const char *base_url, GError **error);
/* A kept picture by its name, or NULL */
GBytes *load_avatar(Bouncer *bouncer, const char *name, const char **content_type);
/* A kept picture's file name, the user it belongs to and its size */
typedef struct {
	char *name;
	gint64 user_id;
	goffset bytes;
} AvatarFile;
void free_avatar_file(AvatarFile *file);
/* Every kept picture, grouped by the user it belongs to */
GPtrArray *list_avatars(Bouncer *bouncer);
gboolean remove_avatar(Bouncer *bouncer, const char *name);
void remove_user_avatars(Bouncer *bouncer, gint64 user_id);
/* Files */
/* Keeps a shared file under its name and returns its address under the base address */
char *store_file(Bouncer *bouncer, BouncerUser *user, GBytes *data, const char *file_name, const char *base_url,
	GError **error);
/* A kept file by its name, with the type it is served as and whether browsers should save it rather than show it */
GBytes *load_file(Bouncer *bouncer, const char *name, const char **content_type, gboolean *is_download);
void remove_user_files(Bouncer *bouncer, gint64 user_id);
/* Removes files older than the settings keep them now and every hour after */
void start_file_cleanup(Bouncer *bouncer);
/* Creates an invite and returns its token */
char *create_invite(Bouncer *bouncer, BouncerUser *creator, int uses);
void close_downstream(Downstream *downstream);
gboolean has_downstream_cap(Downstream *downstream, const char *name);
/* Sends a message shaped to the app's capabilities and tagged with its network, or untagged for the bouncer's own */
void send_to_downstream(Downstream *downstream, Upstream *upstream, const IrcMessage *message);
void send_downstream_numeric(Downstream *downstream, Upstream *upstream, const char *numeric, ...)
	G_GNUC_NULL_TERMINATED;
/* Starts a network afresh in the app with its details, channels and members, when the app logs in and whenever
 * the network connects again */
void welcome_network(Downstream *downstream, gpointer upstream);
void send_network_caps(Downstream *downstream, gpointer upstream);
void replay_upstream(Downstream *downstream, Upstream *upstream);
void send_read_marker(Downstream *downstream, Upstream *upstream, const char *target, const char *time);

/* History */
void store_upstream_message(Upstream *upstream, IrcMessage *message, GPtrArray *affected);
/* Whether a line belongs with the server's own chat, its notices and its replies to what a device asked */
gboolean is_server_line(const IrcMessage *message, gboolean is_answer);
/* Keeps a line of the server's own chat, which apps ask for under SERVER_TARGET */
void store_server_line(Upstream *upstream, IrcMessage *message);
void answer_history_request(Downstream *downstream, Upstream *upstream, const IrcMessage *message);
void answer_read_marker(Downstream *downstream, Upstream *upstream, const IrcMessage *message);
void answer_search(Downstream *downstream, Upstream *upstream, const IrcMessage *message);
