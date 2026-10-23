#pragma once

#include <glib.h>

/* The bouncer's SQLite file of users, networks, joined channels, history and read markers */
typedef struct Database Database;

typedef struct {
	gint64 id;
	char *name, *password_hash;
	gboolean is_admin;
} UserRecord;

/* A key that lets new people register, with its uses left or minus one for unlimited */
typedef struct {
	char *token, *created;
	int uses_left;
} InviteRecord;

/* A network a user added, with the attributes of the bouncer networks extension */
typedef struct {
	gint64 id, user_id;
	char *name, *host;
	int port;
	gboolean uses_tls;
	char *nickname, *username, *realname, *password;
	char *sasl_username, *sasl_password;
} NetworkRecord;

typedef struct {
	char *name, *key;
} ChannelRecord;

/* What a history line holds, so a client without event playback only gets messages */
typedef enum {
	HISTORY_MESSAGE = 1 << 0,
	HISTORY_TAG_MESSAGE = 1 << 1,
	HISTORY_REDACTION = 1 << 2,
	HISTORY_EVENT = 1 << 3,
} HistoryKind;

/* A place in a conversation's history, ordered by time and then by storage order */
typedef struct {
	char *time;
	gint64 id;
} HistoryPosition;

typedef struct {
	char *target, *time;
} HistoryTarget;

Database *open_database(const char *path, GError **error);
void close_database(Database *database);

GPtrArray *load_users(Database *database);
gint64 add_user(Database *database, const char *name, const char *password_hash, gboolean is_admin);
gboolean change_user_password(Database *database, const char *name, const char *password_hash);
void save_user_admin(Database *database, gint64 user_id, gboolean is_admin);
void save_user_password(Database *database, gint64 user_id, const char *password_hash);
/* Deletes the user with their networks, channels, history and invites */
void delete_user(Database *database, gint64 user_id);
void free_user_record(UserRecord *record);

void add_invite(Database *database, const char *token, int uses, gint64 created_by);
GPtrArray *load_invites(Database *database);
int count_invite_uses(Database *database, const char *token);
void use_invite(Database *database, const char *token);
void delete_invite(Database *database, const char *token);
void free_invite_record(InviteRecord *record);

GPtrArray *load_networks(Database *database, gint64 user_id);
gint64 add_network(Database *database, const NetworkRecord *record);
void save_network(Database *database, const NetworkRecord *record);
void delete_network(Database *database, gint64 network_id);
void copy_network_record(NetworkRecord *target, const NetworkRecord *source);
void clear_network_record(NetworkRecord *record);

GPtrArray *load_channels(Database *database, gint64 network_id);
void save_channel(Database *database, gint64 network_id, const char *name, const char *folded, const char *key);
void delete_channel(Database *database, gint64 network_id, const char *folded);
void free_channel_record(ChannelRecord *record);

void store_history(Database *database, gint64 network_id, const char *target, const char *folded,
	const char *time, const char *message_id, HistoryKind kind, const char *line);
/* Lines strictly between the bounds, either of which may be NULL, oldest first, taken from the newest end
 * when asked */
GPtrArray *query_history(Database *database, gint64 network_id, const char *folded, const HistoryPosition *after,
	const HistoryPosition *before, gboolean takes_newest, int limit, int kinds);
gboolean find_history_message(Database *database, gint64 network_id, const char *message_id,
	HistoryPosition *position);
GPtrArray *query_history_targets(Database *database, gint64 network_id, const char *from, const char *to,
	int limit);
void free_history_target(HistoryTarget *target);

char *load_read_marker(Database *database, gint64 network_id, const char *folded);
void save_read_marker(Database *database, gint64 network_id, const char *folded, const char *time);
