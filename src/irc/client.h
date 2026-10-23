#pragma once

#include <gio/gio.h>

#include "message.h"
#include "state.h"

/* One connection to an IRC server or bouncer, which registers, negotiates capabilities and reconnects */
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
	/* The bouncer network to bind to, for a connection that goes through a bouncer */
	char *bound_network;
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
void free_irc_client(IrcClient *client);

void connect_irc_client(IrcClient *client);
/* Sends QUIT with the reason and stops reconnecting */
void disconnect_irc_client(IrcClient *client, const char *reason);
/* Replaces the settings for the next connection */
void change_irc_client_settings(IrcClient *client, const IrcClientSettings *settings);

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
