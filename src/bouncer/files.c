#include <glib/gstdio.h>
#include <string.h>

#include "bouncer.h"

#define NAME_RANDOM_BYTES 16
#define TOKEN_CHARS 20
/* Long names are cut, keeping the end where the extension is */
#define KEPT_NAME_CHARS 80
#define CLEANUP_SECONDS (60 * 60)
#define FALLBACK_NAME "file"

/* Files of these types show in the browser, and every other file is saved instead, so a page or script someone
 * shared never runs on the bouncer's website */
static const char *const SHOWN_TYPES[][2] = {
	{".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
	{".webp", "image/webp"}, {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".mp3", "audio/mpeg"},
	{".ogg", "audio/ogg"}, {".opus", "audio/ogg"}, {".flac", "audio/flac"}, {".wav", "audio/wav"},
	{".txt", "text/plain; charset=utf-8"}, {".log", "text/plain; charset=utf-8"}, {".pdf", "application/pdf"},
};


/* Only letters, digits, dots, dashes and underscores, so a name never reaches outside the folder or needs escaping
 * in an address */
static gboolean is_safe_letter(char letter) {
	return g_ascii_isalnum(letter) || letter == '.' || letter == '-' || letter == '_';
}


static char *make_safe_name(const char *file_name) {
	g_autofree char *base = g_path_get_basename(file_name && *file_name ? file_name : FALLBACK_NAME);
	/* Letters outside ASCII become their nearest plain letters, such as å turning into a */
	g_autofree char *plain = g_str_to_ascii(base, "C");
	GString *safe = g_string_new(NULL);
	for (const char *letter = plain; *letter; letter++)
		g_string_append_c(safe, is_safe_letter(*letter) ? *letter : '_');
	while (safe->len && safe->str[0] == '.')
		g_string_erase(safe, 0, 1);
	if (!safe->len || g_str_equal(safe->str, "_"))
		g_string_assign(safe, FALLBACK_NAME);
	if (safe->len > KEPT_NAME_CHARS)
		g_string_erase(safe, 0, (gssize)(safe->len - KEPT_NAME_CHARS));
	return g_string_free(safe, FALSE);
}


static gboolean is_file_name(const char *name) {
	for (const char *letter = name; *letter; letter++)
		if (!is_safe_letter(*letter))
			return FALSE;
	return *name && name[0] != '.' && !strstr(name, "..");
}


/* Names start with the user's id and a random token, so nobody can guess another file's address */
char *store_file(Bouncer *bouncer, BouncerUser *user, GBytes *data, const char *file_name, const char *base_url,
	GError **error) {
	const FileSettings *settings = &bouncer->files;
	if (g_bytes_get_size(data) > settings->largest_bytes) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Files can be at most %.0f MB",
			settings->largest_bytes / (1024.0 * 1024.0));
		return NULL;
	}
	guint8 random[NAME_RANDOM_BYTES];
	for (int index = 0; index < NAME_RANDOM_BYTES; index++)
		random[index] = (guint8)g_random_int_range(0, 256);
	g_autofree char *token = g_compute_checksum_for_data(G_CHECKSUM_SHA256, random, sizeof random);
	g_autofree char *safe = make_safe_name(file_name);
	g_autofree char *name = g_strdup_printf("%" G_GINT64_FORMAT "-%.*s-%s", user->id, TOKEN_CHARS, token, safe);
	g_mkdir_with_parents(settings->folder, 0700);
	g_autofree char *path = g_build_filename(settings->folder, name, NULL);
	if (!g_file_set_contents_full(path, g_bytes_get_data(data, NULL), (gssize)g_bytes_get_size(data),
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
		return NULL;
	g_message("%s shared %s", user->name, name);
	return g_strdup_printf("%s/files/%s", base_url, name);
}


static const char *find_shown_type(const char *name) {
	g_autofree char *lower = g_ascii_strdown(name, -1);
	for (guint index = 0; index < G_N_ELEMENTS(SHOWN_TYPES); index++)
		if (g_str_has_suffix(lower, SHOWN_TYPES[index][0]))
			return SHOWN_TYPES[index][1];
	return NULL;
}


GBytes *load_file(Bouncer *bouncer, const char *name, const char **content_type, gboolean *is_download) {
	if (!is_file_name(name))
		return NULL;
	g_autofree char *path = g_build_filename(bouncer->files.folder, name, NULL);
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(path, &contents, &length, NULL))
		return NULL;
	const char *shown = find_shown_type(name);
	*content_type = shown ? shown : "application/octet-stream";
	*is_download = shown == NULL;
	return g_bytes_new_take(contents, length);
}


/* Goes through every kept file, removing those the check picks */
static void remove_files_where(Bouncer *bouncer, gboolean (*is_picked)(const char *name, GStatBuf *details,
	gpointer data), gpointer data) {
	g_autoptr(GDir) directory = g_dir_open(bouncer->files.folder, 0, NULL);
	const char *name;
	while (directory && (name = g_dir_read_name(directory))) {
		g_autofree char *path = g_build_filename(bouncer->files.folder, name, NULL);
		GStatBuf details;
		if (is_file_name(name) && g_stat(path, &details) == 0 && is_picked(name, &details, data))
			g_remove(path);
	}
}


static gboolean is_users_file(const char *name, GStatBuf *details, gpointer data) {
	return g_ascii_strtoll(name, NULL, 10) == *(gint64 *)data;
}


void remove_user_files(Bouncer *bouncer, gint64 user_id) {
	remove_files_where(bouncer, is_users_file, &user_id);
}


static gboolean is_old_file(const char *name, GStatBuf *details, gpointer data) {
	return details->st_mtime < *(gint64 *)data;
}


static gboolean remove_old_files(gpointer data) {
	Bouncer *bouncer = data;
	gint64 oldest = g_get_real_time() / G_USEC_PER_SEC - bouncer->files.kept_seconds;
	remove_files_where(bouncer, is_old_file, &oldest);
	return G_SOURCE_CONTINUE;
}


void start_file_cleanup(Bouncer *bouncer) {
	remove_old_files(bouncer);
	g_timeout_add_seconds(CLEANUP_SECONDS, remove_old_files, bouncer);
}
