#include <sqlite3.h>

#include "../irc/message.h"
#include "database.h"

#define BUSY_TIMEOUT_MILLISECONDS 5000

struct Database {
	sqlite3 *connection;
};

static const char SCHEMA[] =
	"PRAGMA journal_mode = WAL;"
	"PRAGMA foreign_keys = ON;"
	"CREATE TABLE IF NOT EXISTS users ("
	" id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, password TEXT NOT NULL, is_admin INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS invites ("
	" token TEXT PRIMARY KEY, uses_left INTEGER NOT NULL, created_by INTEGER REFERENCES users(id) ON DELETE CASCADE,"
	" created TEXT NOT NULL);"
	"CREATE TABLE IF NOT EXISTS networks ("
	" id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
	" name TEXT, host TEXT NOT NULL, port INTEGER NOT NULL, tls INTEGER NOT NULL,"
	" nickname TEXT, username TEXT, realname TEXT, password TEXT, sasl_username TEXT, sasl_password TEXT);"
	"CREATE TABLE IF NOT EXISTS channels ("
	" network_id INTEGER NOT NULL REFERENCES networks(id) ON DELETE CASCADE,"
	" name TEXT NOT NULL, folded TEXT NOT NULL, key TEXT, PRIMARY KEY (network_id, folded));"
	"CREATE TABLE IF NOT EXISTS history ("
	" id INTEGER PRIMARY KEY, network_id INTEGER NOT NULL REFERENCES networks(id) ON DELETE CASCADE,"
	" target TEXT NOT NULL, folded TEXT NOT NULL, time TEXT NOT NULL, message_id TEXT,"
	" kind INTEGER NOT NULL, line TEXT NOT NULL);"
	"CREATE INDEX IF NOT EXISTS history_by_target ON history (network_id, folded, time, id);"
	"CREATE INDEX IF NOT EXISTS history_by_message ON history (network_id, message_id);"
	"CREATE TABLE IF NOT EXISTS read_markers ("
	" network_id INTEGER NOT NULL REFERENCES networks(id) ON DELETE CASCADE,"
	" folded TEXT NOT NULL, time TEXT NOT NULL, PRIMARY KEY (network_id, folded));";


Database *open_database(const char *path, GError **error) {
	sqlite3 *connection = NULL;
	if (sqlite3_open(path, &connection) != SQLITE_OK) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s", sqlite3_errmsg(connection));
		sqlite3_close(connection);
		return NULL;
	}
	sqlite3_busy_timeout(connection, BUSY_TIMEOUT_MILLISECONDS);
	char *failure = NULL;
	if (sqlite3_exec(connection, SCHEMA, NULL, NULL, &failure) != SQLITE_OK) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s", failure);
		sqlite3_free(failure);
		sqlite3_close(connection);
		return NULL;
	}
	Database *database = g_new0(Database, 1);
	database->connection = connection;
	return database;
}


void close_database(Database *database) {
	if (!database)
		return;
	sqlite3_close(database->connection);
	g_free(database);
}


static sqlite3_stmt *prepare(Database *database, const char *sql) {
	sqlite3_stmt *statement = NULL;
	if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, NULL) != SQLITE_OK)
		g_warning("Database query failed: %s", sqlite3_errmsg(database->connection));
	return statement;
}


static void bind_text(sqlite3_stmt *statement, int index, const char *text) {
	if (text)
		sqlite3_bind_text(statement, index, text, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(statement, index);
}


static char *read_text(sqlite3_stmt *statement, int column) {
	const unsigned char *text = sqlite3_column_text(statement, column);
	return text ? g_strdup((const char *)text) : NULL;
}


/* Runs a statement that returns no rows and reports a failure in the log */
static void finish_change(Database *database, sqlite3_stmt *statement) {
	if (!statement)
		return;
	if (sqlite3_step(statement) != SQLITE_DONE)
		g_warning("Database change failed: %s", sqlite3_errmsg(database->connection));
	sqlite3_finalize(statement);
}


void free_user_record(UserRecord *record) {
	g_free(record->name);
	g_free(record->password_hash);
	g_free(record);
}


GPtrArray *load_users(Database *database) {
	GPtrArray *users = g_ptr_array_new_with_free_func((GDestroyNotify)free_user_record);
	sqlite3_stmt *statement = prepare(database, "SELECT id, name, password, is_admin FROM users ORDER BY id");
	while (statement && sqlite3_step(statement) == SQLITE_ROW) {
		UserRecord *record = g_new0(UserRecord, 1);
		record->id = sqlite3_column_int64(statement, 0);
		record->name = read_text(statement, 1);
		record->password_hash = read_text(statement, 2);
		record->is_admin = sqlite3_column_int(statement, 3) != 0;
		g_ptr_array_add(users, record);
	}
	sqlite3_finalize(statement);
	return users;
}


gint64 add_user(Database *database, const char *name, const char *password_hash, gboolean is_admin) {
	sqlite3_stmt *statement = prepare(database, "INSERT INTO users (name, password, is_admin) VALUES (?, ?, ?)");
	bind_text(statement, 1, name);
	bind_text(statement, 2, password_hash);
	sqlite3_bind_int(statement, 3, is_admin);
	finish_change(database, statement);
	return sqlite3_last_insert_rowid(database->connection);
}


gboolean change_user_password(Database *database, const char *name, const char *password_hash) {
	sqlite3_stmt *statement = prepare(database, "UPDATE users SET password = ? WHERE name = ?");
	bind_text(statement, 1, password_hash);
	bind_text(statement, 2, name);
	finish_change(database, statement);
	return sqlite3_changes(database->connection) > 0;
}


void save_user_admin(Database *database, gint64 user_id, gboolean is_admin) {
	sqlite3_stmt *statement = prepare(database, "UPDATE users SET is_admin = ? WHERE id = ?");
	sqlite3_bind_int(statement, 1, is_admin);
	sqlite3_bind_int64(statement, 2, user_id);
	finish_change(database, statement);
}


void save_user_password(Database *database, gint64 user_id, const char *password_hash) {
	sqlite3_stmt *statement = prepare(database, "UPDATE users SET password = ? WHERE id = ?");
	bind_text(statement, 1, password_hash);
	sqlite3_bind_int64(statement, 2, user_id);
	finish_change(database, statement);
}


void delete_user(Database *database, gint64 user_id) {
	sqlite3_stmt *statement = prepare(database, "DELETE FROM users WHERE id = ?");
	sqlite3_bind_int64(statement, 1, user_id);
	finish_change(database, statement);
}


void free_invite_record(InviteRecord *record) {
	g_free(record->token);
	g_free(record->created);
	g_free(record);
}


void add_invite(Database *database, const char *token, int uses, gint64 created_by) {
	g_autofree char *now = format_irc_time_now();
	sqlite3_stmt *statement = prepare(database,
		"INSERT INTO invites (token, uses_left, created_by, created) VALUES (?, ?, ?, ?)");
	bind_text(statement, 1, token);
	sqlite3_bind_int(statement, 2, uses);
	sqlite3_bind_int64(statement, 3, created_by);
	bind_text(statement, 4, now);
	finish_change(database, statement);
}


GPtrArray *load_invites(Database *database) {
	GPtrArray *invites = g_ptr_array_new_with_free_func((GDestroyNotify)free_invite_record);
	sqlite3_stmt *statement = prepare(database, "SELECT token, uses_left, created FROM invites ORDER BY created DESC");
	while (statement && sqlite3_step(statement) == SQLITE_ROW) {
		InviteRecord *record = g_new0(InviteRecord, 1);
		record->token = read_text(statement, 0);
		record->uses_left = sqlite3_column_int(statement, 1);
		record->created = read_text(statement, 2);
		g_ptr_array_add(invites, record);
	}
	sqlite3_finalize(statement);
	return invites;
}


/* Returns the uses left, minus one for unlimited, or zero for an unknown or spent invite */
int count_invite_uses(Database *database, const char *token) {
	sqlite3_stmt *statement = prepare(database, "SELECT uses_left FROM invites WHERE token = ?");
	bind_text(statement, 1, token);
	int uses = statement && sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : 0;
	sqlite3_finalize(statement);
	return uses;
}


void use_invite(Database *database, const char *token) {
	sqlite3_stmt *statement = prepare(database,
		"UPDATE invites SET uses_left = uses_left - 1 WHERE token = ? AND uses_left > 0");
	bind_text(statement, 1, token);
	finish_change(database, statement);
	statement = prepare(database, "DELETE FROM invites WHERE token = ? AND uses_left = 0");
	bind_text(statement, 1, token);
	finish_change(database, statement);
}


void delete_invite(Database *database, const char *token) {
	sqlite3_stmt *statement = prepare(database, "DELETE FROM invites WHERE token = ?");
	bind_text(statement, 1, token);
	finish_change(database, statement);
}


void copy_network_record(NetworkRecord *target, const NetworkRecord *source) {
	*target = (NetworkRecord){
		.id = source->id,
		.user_id = source->user_id,
		.name = g_strdup(source->name),
		.host = g_strdup(source->host),
		.port = source->port,
		.uses_tls = source->uses_tls,
		.nickname = g_strdup(source->nickname),
		.username = g_strdup(source->username),
		.realname = g_strdup(source->realname),
		.password = g_strdup(source->password),
		.sasl_username = g_strdup(source->sasl_username),
		.sasl_password = g_strdup(source->sasl_password),
	};
}


void clear_network_record(NetworkRecord *record) {
	g_free(record->name);
	g_free(record->host);
	g_free(record->nickname);
	g_free(record->username);
	g_free(record->realname);
	g_free(record->password);
	g_free(record->sasl_username);
	g_free(record->sasl_password);
	*record = (NetworkRecord){0};
}


static void free_network_record(NetworkRecord *record) {
	clear_network_record(record);
	g_free(record);
}


GPtrArray *load_networks(Database *database, gint64 user_id) {
	GPtrArray *networks = g_ptr_array_new_with_free_func((GDestroyNotify)free_network_record);
	sqlite3_stmt *statement = prepare(database,
		"SELECT id, name, host, port, tls, nickname, username, realname, password, sasl_username, sasl_password"
		" FROM networks WHERE user_id = ? ORDER BY id");
	sqlite3_bind_int64(statement, 1, user_id);
	while (statement && sqlite3_step(statement) == SQLITE_ROW) {
		NetworkRecord *record = g_new0(NetworkRecord, 1);
		record->id = sqlite3_column_int64(statement, 0);
		record->user_id = user_id;
		record->name = read_text(statement, 1);
		record->host = read_text(statement, 2);
		record->port = sqlite3_column_int(statement, 3);
		record->uses_tls = sqlite3_column_int(statement, 4) != 0;
		record->nickname = read_text(statement, 5);
		record->username = read_text(statement, 6);
		record->realname = read_text(statement, 7);
		record->password = read_text(statement, 8);
		record->sasl_username = read_text(statement, 9);
		record->sasl_password = read_text(statement, 10);
		g_ptr_array_add(networks, record);
	}
	sqlite3_finalize(statement);
	return networks;
}


static void bind_network(sqlite3_stmt *statement, const NetworkRecord *record) {
	bind_text(statement, 1, record->name);
	bind_text(statement, 2, record->host);
	sqlite3_bind_int(statement, 3, record->port);
	sqlite3_bind_int(statement, 4, record->uses_tls);
	bind_text(statement, 5, record->nickname);
	bind_text(statement, 6, record->username);
	bind_text(statement, 7, record->realname);
	bind_text(statement, 8, record->password);
	bind_text(statement, 9, record->sasl_username);
	bind_text(statement, 10, record->sasl_password);
}


gint64 add_network(Database *database, const NetworkRecord *record) {
	sqlite3_stmt *statement = prepare(database,
		"INSERT INTO networks (name, host, port, tls, nickname, username, realname, password, sasl_username,"
		" sasl_password, user_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
	bind_network(statement, record);
	sqlite3_bind_int64(statement, 11, record->user_id);
	finish_change(database, statement);
	return sqlite3_last_insert_rowid(database->connection);
}


void save_network(Database *database, const NetworkRecord *record) {
	sqlite3_stmt *statement = prepare(database,
		"UPDATE networks SET name = ?, host = ?, port = ?, tls = ?, nickname = ?, username = ?, realname = ?,"
		" password = ?, sasl_username = ?, sasl_password = ? WHERE id = ?");
	bind_network(statement, record);
	sqlite3_bind_int64(statement, 11, record->id);
	finish_change(database, statement);
}


void delete_network(Database *database, gint64 network_id) {
	sqlite3_stmt *statement = prepare(database, "DELETE FROM networks WHERE id = ?");
	sqlite3_bind_int64(statement, 1, network_id);
	finish_change(database, statement);
}


void free_channel_record(ChannelRecord *record) {
	g_free(record->name);
	g_free(record->key);
	g_free(record);
}


GPtrArray *load_channels(Database *database, gint64 network_id) {
	GPtrArray *channels = g_ptr_array_new_with_free_func((GDestroyNotify)free_channel_record);
	sqlite3_stmt *statement = prepare(database, "SELECT name, key FROM channels WHERE network_id = ? ORDER BY name");
	sqlite3_bind_int64(statement, 1, network_id);
	while (statement && sqlite3_step(statement) == SQLITE_ROW) {
		ChannelRecord *record = g_new0(ChannelRecord, 1);
		record->name = read_text(statement, 0);
		record->key = read_text(statement, 1);
		g_ptr_array_add(channels, record);
	}
	sqlite3_finalize(statement);
	return channels;
}


void save_channel(Database *database, gint64 network_id, const char *name, const char *folded, const char *key) {
	sqlite3_stmt *statement = prepare(database,
		"INSERT INTO channels (network_id, name, folded, key) VALUES (?, ?, ?, ?)"
		" ON CONFLICT (network_id, folded) DO UPDATE SET name = excluded.name,"
		" key = COALESCE(excluded.key, channels.key)");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, name);
	bind_text(statement, 3, folded);
	bind_text(statement, 4, key);
	finish_change(database, statement);
}


void delete_channel(Database *database, gint64 network_id, const char *folded) {
	sqlite3_stmt *statement = prepare(database, "DELETE FROM channels WHERE network_id = ? AND folded = ?");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, folded);
	finish_change(database, statement);
}


void store_history(Database *database, gint64 network_id, const char *target, const char *folded,
	const char *time, const char *message_id, HistoryKind kind, const char *line) {
	sqlite3_stmt *statement = prepare(database,
		"INSERT INTO history (network_id, target, folded, time, message_id, kind, line) VALUES (?, ?, ?, ?, ?, ?, ?)");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, target);
	bind_text(statement, 3, folded);
	bind_text(statement, 4, time);
	bind_text(statement, 5, message_id);
	sqlite3_bind_int(statement, 6, kind);
	bind_text(statement, 7, line);
	finish_change(database, statement);
}


static void bind_position(sqlite3_stmt *statement, int index, const HistoryPosition *position) {
	bind_text(statement, index, position->time);
	bind_text(statement, index + 1, position->time);
	sqlite3_bind_int64(statement, index + 2, position->id);
}


GPtrArray *query_history(Database *database, gint64 network_id, const char *folded, const HistoryPosition *after,
	const HistoryPosition *before, gboolean takes_newest, int limit, int kinds) {
	g_autoptr(GString) sql = g_string_new("SELECT line FROM history WHERE network_id = ? AND folded = ?"
		" AND (kind & ?) != 0");
	if (after)
		g_string_append(sql, " AND (time > ? OR (time = ? AND id > ?))");
	if (before)
		g_string_append(sql, " AND (time < ? OR (time = ? AND id < ?))");
	g_string_append(sql, takes_newest ? " ORDER BY time DESC, id DESC LIMIT ?" : " ORDER BY time, id LIMIT ?");
	sqlite3_stmt *statement = prepare(database, sql->str);
	int index = 1;
	sqlite3_bind_int64(statement, index++, network_id);
	bind_text(statement, index++, folded);
	sqlite3_bind_int(statement, index++, kinds);
	if (after) {
		bind_position(statement, index, after);
		index += 3;
	}
	if (before) {
		bind_position(statement, index, before);
		index += 3;
	}
	sqlite3_bind_int(statement, index, limit);
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	while (statement && sqlite3_step(statement) == SQLITE_ROW)
		g_ptr_array_add(lines, read_text(statement, 0));
	sqlite3_finalize(statement);
	/* Rows taken from the newest end come out newest first, and clients want them in time order */
	for (guint low = 0, high = lines->len; takes_newest && high > 0 && low < high - 1; low++, high--) {
		gpointer swap = lines->pdata[low];
		lines->pdata[low] = lines->pdata[high - 1];
		lines->pdata[high - 1] = swap;
	}
	return lines;
}


gboolean find_history_message(Database *database, gint64 network_id, const char *message_id,
	HistoryPosition *position) {
	sqlite3_stmt *statement = prepare(database,
		"SELECT time, id FROM history WHERE network_id = ? AND message_id = ? ORDER BY id LIMIT 1");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, message_id);
	gboolean is_found = statement && sqlite3_step(statement) == SQLITE_ROW;
	if (is_found) {
		position->time = read_text(statement, 0);
		position->id = sqlite3_column_int64(statement, 1);
	}
	sqlite3_finalize(statement);
	return is_found;
}


void free_history_target(HistoryTarget *target) {
	g_free(target->target);
	g_free(target->time);
	g_free(target);
}


GPtrArray *query_history_targets(Database *database, gint64 network_id, const char *from, const char *to,
	int limit) {
	sqlite3_stmt *statement = prepare(database,
		"SELECT target, MAX(time) AS latest FROM history WHERE network_id = ? AND kind != ?"
		" GROUP BY folded HAVING latest > ? AND latest < ? ORDER BY latest LIMIT ?");
	sqlite3_bind_int64(statement, 1, network_id);
	sqlite3_bind_int(statement, 2, HISTORY_EVENT);
	gboolean is_ascending = g_strcmp0(from, to) < 0;
	bind_text(statement, 3, is_ascending ? from : to);
	bind_text(statement, 4, is_ascending ? to : from);
	sqlite3_bind_int(statement, 5, limit);
	GPtrArray *targets = g_ptr_array_new_with_free_func((GDestroyNotify)free_history_target);
	while (statement && sqlite3_step(statement) == SQLITE_ROW) {
		HistoryTarget *target = g_new0(HistoryTarget, 1);
		target->target = read_text(statement, 0);
		target->time = read_text(statement, 1);
		g_ptr_array_add(targets, target);
	}
	sqlite3_finalize(statement);
	return targets;
}


char *load_read_marker(Database *database, gint64 network_id, const char *folded) {
	sqlite3_stmt *statement = prepare(database, "SELECT time FROM read_markers WHERE network_id = ? AND folded = ?");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, folded);
	char *time = statement && sqlite3_step(statement) == SQLITE_ROW ? read_text(statement, 0) : NULL;
	sqlite3_finalize(statement);
	return time;
}


void save_read_marker(Database *database, gint64 network_id, const char *folded, const char *time) {
	sqlite3_stmt *statement = prepare(database,
		"INSERT INTO read_markers (network_id, folded, time) VALUES (?, ?, ?)"
		" ON CONFLICT (network_id, folded) DO UPDATE SET time = excluded.time");
	sqlite3_bind_int64(statement, 1, network_id);
	bind_text(statement, 2, folded);
	bind_text(statement, 3, time);
	finish_change(database, statement);
}
