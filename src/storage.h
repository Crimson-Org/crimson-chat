#pragma once

#include <glib.h>

#include "model.h"

/* A server the user added, or a bouncer whose networks come from the bouncer itself */
typedef struct {
	char *id, *name, *host;
	int port;
	gboolean uses_tls, is_bouncer;
	char *nick, *username, *realname, *password, *sasl_username, *sasl_password;
	char *trusted_certificate;
	/* Channels a direct connection joins again, kept in the order they were joined */
	GStrv channels;
} AccountRecord;

GPtrArray *load_accounts(void);
void save_accounts(GPtrArray *accounts);
AccountRecord *copy_account_record(const AccountRecord *record);
void free_account_record(AccountRecord *record);
char *create_account_id(void);

/* Rooms and their latest line, shown the instant the window opens until the server answers */
void load_room_cache(CrimsonNetwork *network);
/* The bouncer networks of an account seen on the last run, as network keys and their names */
GPtrArray *list_cached_networks(const char *account_id, GPtrArray **names);
void save_room_cache(GListModel *networks);
/* The display name and picture the user chose on a server, which servers forget when the connection ends */
void load_network_profile(const char *key, char **display_name, char **picture);
void save_network_profile(const char *key, const char *display_name, const char *picture);
/* A private chat the user deleted, with when, so a bouncer listing older history does not bring it back */
void note_deleted_chat(const char *key, const char *name, gint64 time);
gint64 find_deleted_chat(const char *key, const char *name);
