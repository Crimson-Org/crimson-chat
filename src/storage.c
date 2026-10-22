#include "common/key-store.h"
#include "storage.h"

#define ACCOUNTS_FILE "accounts.ini"
#define ROOMS_FILE "rooms.ini"
#define PROFILES_FILE "profiles.ini"
#define DELETED_FILE "deleted-chats.ini"
#define ACCOUNT_PREFIX "account "
#define DEFAULT_PORT 6697
#define CACHED_ROOMS_PER_NETWORK 200

static GKeyFile *room_cache;


void free_account_record(AccountRecord *record) {
	if (!record)
		return;
	g_free(record->id);
	g_free(record->name);
	g_free(record->host);
	g_free(record->nick);
	g_free(record->username);
	g_free(record->realname);
	g_free(record->password);
	g_free(record->sasl_username);
	g_free(record->sasl_password);
	g_free(record->trusted_certificate);
	g_strfreev(record->channels);
	g_free(record);
}


AccountRecord *copy_account_record(const AccountRecord *source) {
	AccountRecord *record = g_new0(AccountRecord, 1);
	*record = (AccountRecord){
		.id = g_strdup(source->id),
		.name = g_strdup(source->name),
		.host = g_strdup(source->host),
		.port = source->port,
		.uses_tls = source->uses_tls,
		.is_bouncer = source->is_bouncer,
		.nick = g_strdup(source->nick),
		.username = g_strdup(source->username),
		.realname = g_strdup(source->realname),
		.password = g_strdup(source->password),
		.sasl_username = g_strdup(source->sasl_username),
		.sasl_password = g_strdup(source->sasl_password),
		.trusted_certificate = g_strdup(source->trusted_certificate),
		.channels = g_strdupv(source->channels),
	};
	return record;
}


char *create_account_id(void) {
	return g_strdup_printf("%08x", g_random_int());
}


static char *read_text(GKeyFile *file, const char *group, const char *key) {
	char *value = g_key_file_get_string(file, group, key, NULL);
	if (value && !*value)
		g_clear_pointer(&value, g_free);
	return value;
}


GPtrArray *load_accounts(void) {
	GPtrArray *accounts = g_ptr_array_new_with_free_func((GDestroyNotify)free_account_record);
	g_autofree char *path = get_config_file_path(ACCOUNTS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
	for (int index = 0; groups[index]; index++) {
		const char *group = groups[index];
		if (!g_str_has_prefix(group, ACCOUNT_PREFIX))
			continue;
		AccountRecord *record = g_new0(AccountRecord, 1);
		record->id = g_strdup(group + strlen(ACCOUNT_PREFIX));
		record->name = read_text(file, group, "name");
		record->host = read_text(file, group, "host");
		record->port = read_key_integer(file, group, "port", DEFAULT_PORT, 1, G_MAXUINT16);
		record->uses_tls = !g_key_file_has_key(file, group, "tls", NULL)
			|| g_key_file_get_boolean(file, group, "tls", NULL);
		record->is_bouncer = g_key_file_get_boolean(file, group, "bouncer", NULL);
		record->nick = read_text(file, group, "nick");
		record->username = read_text(file, group, "username");
		record->realname = read_text(file, group, "realname");
		record->password = read_text(file, group, "password");
		record->sasl_username = read_text(file, group, "sasl-username");
		record->sasl_password = read_text(file, group, "sasl-password");
		record->trusted_certificate = read_text(file, group, "certificate");
		record->channels = g_key_file_get_string_list(file, group, "channels", NULL, NULL);
		if (record->host)
			g_ptr_array_add(accounts, record);
		else
			free_account_record(record);
	}
	return accounts;
}


static void write_text(GKeyFile *file, const char *group, const char *key, const char *value) {
	if (value && *value)
		g_key_file_set_string(file, group, key, value);
}


/* The file holds passwords, so only its owner may read it */
void save_accounts(GPtrArray *accounts) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	for (guint index = 0; index < accounts->len; index++) {
		AccountRecord *record = g_ptr_array_index(accounts, index);
		g_autofree char *group = g_strconcat(ACCOUNT_PREFIX, record->id, NULL);
		write_text(file, group, "name", record->name);
		write_text(file, group, "host", record->host);
		g_key_file_set_integer(file, group, "port", record->port);
		g_key_file_set_boolean(file, group, "tls", record->uses_tls);
		g_key_file_set_boolean(file, group, "bouncer", record->is_bouncer);
		write_text(file, group, "nick", record->nick);
		write_text(file, group, "username", record->username);
		write_text(file, group, "realname", record->realname);
		write_text(file, group, "password", record->password);
		write_text(file, group, "sasl-username", record->sasl_username);
		write_text(file, group, "sasl-password", record->sasl_password);
		write_text(file, group, "certificate", record->trusted_certificate);
		if (record->channels && *record->channels)
			g_key_file_set_string_list(file, group, "channels", (const char *const *)record->channels,
				g_strv_length(record->channels));
	}
	g_autofree char *path = get_config_file_path(ACCOUNTS_FILE);
	g_autofree char *folder = g_path_get_dirname(path);
	g_mkdir_with_parents(folder, 0700);
	g_autofree char *text = g_key_file_to_data(file, NULL, NULL);
	g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}


static GKeyFile *get_room_cache(void) {
	if (!room_cache) {
		g_autofree char *path = get_state_file_path(ROOMS_FILE);
		room_cache = load_key_file(path, G_KEY_FILE_NONE);
	}
	return room_cache;
}


/* Each room is one list entry of kind, name, time, unread count, read time, own flag, sender and preview,
 * split by tabs */
void load_room_cache(CrimsonNetwork *network) {
	g_auto(GStrv) entries = g_key_file_get_string_list(get_room_cache(), network->key, "rooms", NULL, NULL);
	g_autofree char *name = g_key_file_get_string(get_room_cache(), network->key, "name", NULL);
	if (name) {
		g_free(network->name);
		network->name = g_steal_pointer(&name);
	}
	for (int index = 0; entries && entries[index]; index++) {
		g_auto(GStrv) fields = g_strsplit(entries[index], "\t", 8);
		if (g_strv_length(fields) != 8)
			continue;
		RoomKind kind = (RoomKind)g_ascii_strtoll(fields[0], NULL, 10);
		/* Chats with services from before they went to the server's messages are left behind */
		if (kind == ROOM_PRIVATE && is_service_nick(fields[1]))
			continue;
		CrimsonRoom *room = kind == ROOM_SERVER ? network->server_room : ensure_room(network, kind, fields[1]);
		room->last_time = g_ascii_strtoll(fields[2], NULL, 10);
		room->unread_count = (guint)g_ascii_strtoull(fields[3], NULL, 10);
		room->read_time = g_ascii_strtoll(fields[4], NULL, 10);
		room->is_preview_own = g_str_equal(fields[5], "1");
		g_free(room->preview_sender);
		room->preview_sender = *fields[6] ? g_strdup(fields[6]) : NULL;
		g_free(room->preview);
		room->preview = g_strdup(fields[7]);
	}
}


GPtrArray *list_cached_networks(const char *account_id, GPtrArray **names) {
	GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
	*names = g_ptr_array_new_with_free_func(g_free);
	g_autofree char *prefix = g_strconcat(account_id, "/", NULL);
	g_auto(GStrv) groups = g_key_file_get_groups(get_room_cache(), NULL);
	for (int index = 0; groups[index]; index++) {
		if (!g_str_has_prefix(groups[index], prefix))
			continue;
		g_ptr_array_add(keys, g_strdup(groups[index]));
		g_ptr_array_add(*names, g_key_file_get_string(get_room_cache(), groups[index], "name", NULL));
	}
	return keys;
}


static char *describe_cached_room(CrimsonRoom *room) {
	g_autofree char *preview = g_strdup(room->preview ? room->preview : "");
	g_strdelimit(preview, "\t\n\r", ' ');
	return g_strdup_printf("%d\t%s\t%" G_GINT64_FORMAT "\t%u\t%" G_GINT64_FORMAT "\t%d\t%s\t%s", room->kind,
		room->name, room->last_time, room->unread_count, room->read_time, room->is_preview_own,
		room->preview_sender ? room->preview_sender : "", preview);
}


void save_room_cache(GListModel *networks) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	guint count = g_list_model_get_n_items(networks);
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(networks, index);
		g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_free);
		guint room_count = MIN(g_list_model_get_n_items(G_LIST_MODEL(network->rooms)), CACHED_ROOMS_PER_NETWORK);
		for (guint room_index = 0; room_index < room_count; room_index++) {
			g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), room_index);
			g_ptr_array_add(entries, describe_cached_room(room));
		}
		g_key_file_set_string(file, network->key, "name", network->name);
		g_key_file_set_string_list(file, network->key, "rooms", (const char *const *)entries->pdata, entries->len);
	}
	g_autofree char *path = get_state_file_path(ROOMS_FILE);
	save_key_file(file, path);
	g_clear_pointer(&room_cache, g_key_file_unref);
}


void load_network_profile(const char *key, char **display_name, char **picture) {
	g_autofree char *path = get_config_file_path(PROFILES_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	*display_name = g_key_file_get_string(file, key, "display-name", NULL);
	*picture = g_key_file_get_string(file, key, "picture", NULL);
}


void save_network_profile(const char *key, const char *display_name, const char *picture) {
	g_autofree char *path = get_config_file_path(PROFILES_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	g_key_file_remove_group(file, key, NULL);
	if (display_name && *display_name)
		g_key_file_set_string(file, key, "display-name", display_name);
	if (picture && *picture)
		g_key_file_set_string(file, key, "picture", picture);
	save_key_file(file, path);
}


void note_deleted_chat(const char *key, const char *name, gint64 time) {
	g_autofree char *path = get_state_file_path(DELETED_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	g_key_file_set_int64(file, key, name, time);
	save_key_file(file, path);
}


gint64 find_deleted_chat(const char *key, const char *name) {
	g_autofree char *path = get_state_file_path(DELETED_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	return g_key_file_get_int64(file, key, name, NULL);
}
