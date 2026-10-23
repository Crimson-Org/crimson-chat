#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "bouncer.h"

/* Pictures are decoded at no more than this many times their final size, so a huge one never fills the memory */
#define DECODE_FACTOR 2
#define NAME_RANDOM_BYTES 8

/* Only the common picture formats are decoded, which keeps rarer and riskier decoders away from uploads */
static const char *const ACCEPTED_TYPES[] = {"image/png", "image/jpeg", "image/webp", "image/gif", NULL};


static void limit_decoding(GdkPixbufLoader *loader, int width, int height, gpointer data) {
	int largest = GPOINTER_TO_INT(data) * DECODE_FACTOR;
	if (width <= largest && height <= largest)
		return;
	/* The shorter side keeps enough pixels for the square that is cut from the middle */
	double scale = (double)largest / MIN(width, height);
	gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(width * scale)), MAX(1, (int)(height * scale)));
}


static GdkPixbuf *decode_picture(GBytes *data, const char *content_type, int size, GError **error) {
	g_autofree char *type = content_type ? g_ascii_strdown(content_type, (gssize)strcspn(content_type, ";")) : NULL;
	if (!type || !g_strv_contains(ACCEPTED_TYPES, g_strstrip(type))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Send a PNG, JPEG, WebP or GIF picture");
		return NULL;
	}
	g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_mime_type(type, error);
	if (!loader)
		return NULL;
	g_signal_connect(loader, "size-prepared", G_CALLBACK(limit_decoding), GINT_TO_POINTER(size));
	gboolean is_read = gdk_pixbuf_loader_write_bytes(loader, data, error);
	gboolean is_closed = gdk_pixbuf_loader_close(loader, is_read ? error : NULL);
	if (!is_read || !is_closed)
		return NULL;
	GdkPixbuf *picture = gdk_pixbuf_loader_get_pixbuf(loader);
	return picture ? gdk_pixbuf_apply_embedded_orientation(picture) : NULL;
}


/* The middle square of the picture at the configured size, since avatars are shown square or round */
static GdkPixbuf *shape_picture(GdkPixbuf *picture, int size) {
	int width = gdk_pixbuf_get_width(picture), height = gdk_pixbuf_get_height(picture);
	int side = MIN(width, height);
	g_autoptr(GdkPixbuf) square = gdk_pixbuf_new_subpixbuf(picture, (width - side) / 2, (height - side) / 2, side,
		side);
	int final = MIN(size, side);
	return gdk_pixbuf_scale_simple(square, final, final, GDK_INTERP_HYPER);
}


static gboolean save_picture(GdkPixbuf *picture, const AvatarSettings *settings, const char *path, GError **error) {
	if (g_str_equal(settings->format, "jpeg")) {
		g_autofree char *quality = g_strdup_printf("%d", CLAMP(settings->quality, 1, 100));
		/* JPEG has no transparency, so a transparent picture keeps its look over white */
		g_autoptr(GdkPixbuf) flat = gdk_pixbuf_get_has_alpha(picture) ? gdk_pixbuf_composite_color_simple(picture,
			gdk_pixbuf_get_width(picture), gdk_pixbuf_get_height(picture), GDK_INTERP_NEAREST, 255, 1, 0xffffff,
			0xffffff) : g_object_ref(picture);
		return gdk_pixbuf_save(flat, path, "jpeg", error, "quality", quality, NULL);
	}
	g_autofree char *compression = g_strdup_printf("%d", 9 - CLAMP(settings->quality, 0, 100) * 9 / 100);
	return gdk_pixbuf_save(picture, path, "png", error, "compression", compression, NULL);
}


/* Names start with the user and what the picture is for, so a new picture can replace the earlier one */
static char *describe_name_start(BouncerUser *user, Upstream *upstream, const char *target) {
	g_autofree char *for_whom = g_compute_checksum_for_string(G_CHECKSUM_SHA256, target, -1);
	return g_strdup_printf("%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT "-%.12s-", user->id, upstream->record.id,
		for_whom);
}


static void remove_earlier_pictures(const char *folder, const char *start, const char *kept) {
	g_autoptr(GDir) directory = g_dir_open(folder, 0, NULL);
	const char *name;
	while (directory && (name = g_dir_read_name(directory))) {
		if (g_str_has_prefix(name, start) && !g_str_equal(name, kept)) {
			g_autofree char *path = g_build_filename(folder, name, NULL);
			g_remove(path);
		}
	}
}


char *store_avatar(Bouncer *bouncer, BouncerUser *user, Upstream *upstream, const char *target, GBytes *data,
	const char *content_type, const char *base_url, GError **error) {
	const AvatarSettings *settings = &bouncer->avatars;
	if (g_bytes_get_size(data) > settings->largest_bytes) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Pictures can be at most %.1f MB",
			settings->largest_bytes / (1024.0 * 1024.0));
		return NULL;
	}
	g_autoptr(GdkPixbuf) decoded = decode_picture(data, content_type, settings->size, error);
	if (!decoded)
		return NULL;
	g_autoptr(GdkPixbuf) shaped = shape_picture(decoded, settings->size);
	g_mkdir_with_parents(settings->folder, 0700);
	g_autofree char *start = describe_name_start(user, upstream, target);
	guint8 random[NAME_RANDOM_BYTES];
	for (int index = 0; index < NAME_RANDOM_BYTES; index++)
		random[index] = (guint8)g_random_int_range(0, 256);
	g_autofree char *token = g_compute_checksum_for_data(G_CHECKSUM_SHA256, random, sizeof random);
	g_autofree char *name = g_strdup_printf("%s%.16s.%s", start, token, g_str_equal(settings->format, "jpeg")
		? "jpg" : "png");
	g_autofree char *path = g_build_filename(settings->folder, name, NULL);
	if (!save_picture(shaped, settings, path, error))
		return NULL;
	remove_earlier_pictures(settings->folder, start, name);
	char *url = g_strdup_printf("%s/avatars/%s", base_url, name);
	/* The bouncer sets the picture on the network itself, for the user or for the room it was sent for */
	send_irc_client_command(upstream->client, "METADATA", target, "SET", "avatar", url, NULL);
	return url;
}


/* Only names the bouncer gave out, so a request can never reach outside the folder */
static gboolean is_avatar_name(const char *name) {
	for (const char *letter = name; *letter; letter++)
		if (!g_ascii_isalnum(*letter) && *letter != '-' && *letter != '.')
			return FALSE;
	return *name && name[0] != '.' && !strstr(name, "..");
}


GBytes *load_avatar(Bouncer *bouncer, const char *name, const char **content_type) {
	if (!is_avatar_name(name))
		return NULL;
	g_autofree char *path = g_build_filename(bouncer->avatars.folder, name, NULL);
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(path, &contents, &length, NULL))
		return NULL;
	*content_type = g_str_has_suffix(name, ".jpg") ? "image/jpeg" : "image/png";
	return g_bytes_new_take(contents, length);
}


void free_avatar_file(AvatarFile *file) {
	g_free(file->name);
	g_free(file);
}


static int compare_avatar_names(gconstpointer first, gconstpointer second) {
	return g_strcmp0((*(AvatarFile *const *)first)->name, (*(AvatarFile *const *)second)->name);
}


GPtrArray *list_avatars(Bouncer *bouncer) {
	GPtrArray *files = g_ptr_array_new_with_free_func((GDestroyNotify)free_avatar_file);
	g_autoptr(GDir) directory = g_dir_open(bouncer->avatars.folder, 0, NULL);
	const char *name;
	while (directory && (name = g_dir_read_name(directory))) {
		if (!is_avatar_name(name))
			continue;
		g_autofree char *path = g_build_filename(bouncer->avatars.folder, name, NULL);
		GStatBuf details;
		AvatarFile *file = g_new0(AvatarFile, 1);
		file->name = g_strdup(name);
		/* Names start with the id of the user the picture belongs to */
		file->user_id = g_ascii_strtoll(name, NULL, 10);
		file->bytes = g_stat(path, &details) == 0 ? details.st_size : 0;
		g_ptr_array_add(files, file);
	}
	g_ptr_array_sort(files, compare_avatar_names);
	return files;
}


gboolean remove_avatar(Bouncer *bouncer, const char *name) {
	if (!is_avatar_name(name))
		return FALSE;
	g_autofree char *path = g_build_filename(bouncer->avatars.folder, name, NULL);
	return g_remove(path) == 0;
}


void remove_user_avatars(Bouncer *bouncer, gint64 user_id) {
	g_autoptr(GPtrArray) files = list_avatars(bouncer);
	for (guint index = 0; index < files->len; index++) {
		AvatarFile *file = g_ptr_array_index(files, index);
		if (file->user_id == user_id)
			remove_avatar(bouncer, file->name);
	}
}
