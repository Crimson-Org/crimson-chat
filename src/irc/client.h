#pragma once

#include <gio/gio.h>

#include "message.h"
#include "state.h"

/* The tag that names which of a bouncer's networks a line between Crimson Chat and its bouncer is about */
#define IRC_NETWORK_TAG "crimson-chat/network"
/* The capabilities a bouncer's network has only where the server behind it supports them */
#define BOUNCER_NETWORK_CAPS "labeled-response", "account-notify", "away-notify", "extended-join", \
	"userhost-in-names", "chghost", "setname", "invite-notify", "account-tag", "draft/message-redaction", \
	"draft/metadata-2", "draft/channel-rename", "extended-monitor", "draft/account-registration"

/* One connection to an IRC server or bouncer, which registers, negotiates capabilities and reconnects, or one of a
 * bouncer's networks carried over such a connection */
typedef struct IrcClient IrcClient;

typedef enum {
	IRC_CLIENT_DISCONNECTED,
	IRC_CLIENT_CONNECTING,
	IRC_CLIENT_REGISTERED,
} IrcClientStatus;

/* Why the last connection ended, when it ended on something only the user can fix */
typedef enum {
	IRC_FAILURE_NONE,
	IRC_FAILURE_NETWORK,
	IRC_FAILURE_CERTIFICATE,
	IRC_FAILURE_LOGIN,
} IrcFailure;

typedef struct {
	char *host;
	int port;
	gboolean uses_tls;
	char *nick, *username, *realname, *password;
	char *sasl_username, *sasl_password;
	/* SHA-256 of a certificate the user chose to trust, as lowercase hex */
	char *trusted_certificate;
	/* A bouncer paces lines to the network itself, so lines to it need no flood pacing */
	gboolean is_bouncer;
} IrcClientSettings;

/* An open BATCH, kept until its end line has been handed on */
typedef struct {
	char *reference, *type;
	GPtrArray *parameters;
	char *outer;
} IrcBatch;

typedef struct {
	/* Every message after the state took it in, with the channels a QUIT or NICK touched */
	void (*received)(IrcClient *client, const IrcMessage *message, GPtrArray *affected, gpointer data);
	void (*status_changed)(IrcClient *client, IrcClientStatus status, const char *reason, gpointer data);
} IrcClientEvents;

IrcClient *create_irc_client(const IrcClientSettings *settings, const char *const *wanted_caps,
	const IrcClientEvents *events, gpointer data);
/* A network of the bouncer the carrier is connected to, which has its own state and registers when the bouncer
 * welcomes it, while its lines travel over the carrier tagged with its id */
IrcClient *create_irc_network_client(IrcClient *carrier, const char *network_id, const IrcClientEvents *events,
	gpointer data);
void free_irc_client(IrcClient *client);

void connect_irc_client(IrcClient *client);
/* Sends QUIT with the reason and stops reconnecting */
void disconnect_irc_client(IrcClient *client, const char *reason);
/* Replaces the settings for the next connection */
void change_irc_client_settings(IrcClient *client, const IrcClientSettings *settings);
/* Checks a connection at once after the computer woke or changed networks, and retries a lost one without waiting */
void check_irc_client_link(IrcClient *client);

void send_irc_client_message(IrcClient *client, const IrcMessage *message);
void send_irc_client_command(IrcClient *client, const char *command, ...) G_GNUC_NULL_TERMINATED;
/* Tags every line sent from now on with this label, until it is cleared with NULL */
void label_irc_client_lines(IrcClient *client, const char *label);

IrcState *get_irc_client_state(IrcClient *client);
const IrcClientSettings *get_irc_client_settings(IrcClient *client);
IrcClientStatus get_irc_client_status(IrcClient *client);
IrcFailure get_irc_client_failure(IrcClient *client);
/* Whether a lost connection will be tried again by itself, which a refused certificate or login never is */
gboolean will_irc_client_retry(IrcClient *client);
/* The SHA-256 of the certificate the last connection was refused over, as lowercase hex */
const char *get_refused_certificate(IrcClient *client);
IrcBatch *find_irc_batch(IrcClient *client, const char *reference);
/* The batch a message belongs to, through any outer batches, matching the type */
IrcBatch *find_enclosing_batch(IrcClient *client, const IrcMessage *message, const char *type);

void copy_irc_client_settings(IrcClientSettings *target, const IrcClientSettings *source);
void clear_irc_client_settings(IrcClientSettings *settings);
char *hash_certificate(GTlsCertificate *certificate);
/* A certificate hash as colon separated uppercase pairs, the way people compare fingerprints */
char *format_certificate_hash(const char *hash);
