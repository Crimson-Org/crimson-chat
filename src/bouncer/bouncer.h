#pragma once

#include <gio/gio.h>

#include "../irc/client.h"
#include "../irc/stream.h"
#include "database.h"

#define BOUNCER_NAME "crimson-chat-bouncer"
/* The network attribute that says whether the bouncer will reconnect a disconnected network by itself */
#define RETRY_ATTRIBUTE "crimson-chat/retrying"
#define HISTORY_PAGE_LIMIT 1000

typedef struct Bouncer Bouncer;
typedef struct BouncerUser BouncerUser;
typedef struct Upstream Upstream;
typedef struct Downstream Downstream;

/* The running bouncer, with everyone who may log in and every client connected */
/* How the bouncer takes pictures people upload for themselves or their rooms: whether it does, the address the
 * pictures are served from when set, their format, quality from 0 to 100, size in pixels, the largest upload and
 * where they are kept */
typedef struct {
	gboolean is_enabled;
	char *public_url, *format, *folder;
	int quality, size;
	gsize largest_bytes;
} AvatarSettings;

struct Bouncer {
	Database *database;
	AvatarSettings avatars;
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

/* A message a client sent, waiting for the server to echo it back */
typedef struct {
	Downstream *origin;
	char *command, *target;
} PendingEcho;

/* The bouncer's own connection to one network for one user */
struct Upstream {
	BouncerUser *user;
	NetworkRecord record;
	IrcClient *client;
	/* Messages sent for clients, in order, so the server's echo can be traced back to the client that sent it */
	GQueue pending_echoes;
	/* Keys given with JOIN, kept with the channel once the server confirms it */
	GHashTable *channel_keys;
	/* Set while the network is being removed, so its last disconnect announces nothing */
	gboolean is_removed;
	/* Why the network last disconnected, which clients that log in later are told as well */
	char *error;
	/* The account and password a client is registering, kept as the network's login once the server agrees */
	char *registering_account, *registering_password;
};

/* A client logged in to the bouncer, bound to one network or managing the network list */
struct Downstream {
	Bouncer *bouncer;
	IrcStream *stream;
	char *address;
	GHashTable *caps;
	gboolean is_negotiating, is_registered, has_cap_values;
	char *nick, *username, *password, *bound_network;
	GString *login;
	int failed_logins;
	guint registration_source;
	BouncerUser *user;
	Upstream *upstream;
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
/* Tells every network managing client of the user about changed attributes, or a removal when NULL */
void announce_network(Upstream *upstream, GHashTable *attributes);
void forget_downstream(Bouncer *bouncer, Downstream *downstream);

/* Upstream */
Upstream *create_upstream(BouncerUser *user, const NetworkRecord *record);
void free_upstream(Upstream *upstream);
void restart_upstream(Upstream *upstream);
GHashTable *describe_upstream(Upstream *upstream);
const char *describe_upstream_state(Upstream *upstream);
void forward_to_upstream(Upstream *upstream, Downstream *origin, const IrcMessage *message);
char *describe_upstream_progress(Upstream *upstream);
char *get_upstream_nick(Upstream *upstream);

/* Downstream */
void accept_downstream(Bouncer *bouncer, GIOStream *connection, const char *address);

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
/* Creates an invite and returns its token */
char *create_invite(Bouncer *bouncer, BouncerUser *creator, int uses);
void close_downstream(Downstream *downstream);
gboolean has_downstream_cap(Downstream *downstream, const char *name);
void drop_missing_caps(Downstream *downstream, gpointer data);
/* Sends a message shaped to the client's capabilities, or nothing when the client cannot take it */
void send_to_downstream(Downstream *downstream, const IrcMessage *message);
/* RPL_ISUPPORT for the bound network, again once the network has registered */
void send_support_tokens(Downstream *downstream);
void send_downstream_numeric(Downstream *downstream, const char *numeric, ...) G_GNUC_NULL_TERMINATED;
/* Repeats the network's welcome, channels and members, for a client that just bound or a reconnected network */
void replay_upstream(Downstream *downstream);
void send_read_marker(Downstream *downstream, const char *target, const char *time);

/* History */
void store_upstream_message(Upstream *upstream, IrcMessage *message, GPtrArray *affected);
void answer_history_request(Downstream *downstream, const IrcMessage *message);
void answer_read_marker(Downstream *downstream, const IrcMessage *message);
