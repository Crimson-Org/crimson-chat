#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

#include "common/identity.h"
#include "images.h"
#include "window.h"

#define LARGEST_IMAGE_BYTES (10 * 1024 * 1024)
#define SIZE_PLACEHOLDER "{size}"
#define TIMEOUT_SECONDS 20
/* No picture shows larger than this on a sharp screen, so a bigger one is scaled down while it is decoded instead
 * of handed at full size to the window to upload while it draws */
#define LARGEST_SHOWN_PIXELS 640
/* Some picture hosts refuse requests that do not name the program asking */
#define USER_AGENT "CrimsonChat/" APPLICATION_VERSION
#define CACHE_FOLDER "pictures"
/* Next to each kept picture, the request header and value that ask its server whether it changed */
#define VERSION_SUFFIX ".version"

static SoupSession *web;
/* Addresses to their loaded textures, to the widgets showing or waiting for each, and those being read or
 * downloaded */
static GHashTable *loaded, *watchers, *fetching;

/* A picture being read or downloaded, and whether it is kept on disk for the next start */
typedef struct {
	char *url, *path;
	gboolean is_kept, is_cached;
	GBytes *bytes;
	char *version_header, *version;
} Fetch;


static void free_weak_reference(GWeakRef *reference) {
	g_weak_ref_clear(reference);
	g_free(reference);
}


static void free_fetch(Fetch *fetch) {
	g_free(fetch->url);
	g_free(fetch->path);
	g_clear_pointer(&fetch->bytes, g_bytes_unref);
	g_free(fetch->version_header);
	g_free(fetch->version);
	g_free(fetch);
}


static void ensure_tables(void) {
	if (web)
		return;
	web = soup_session_new_with_options("timeout", TIMEOUT_SECONDS, "user-agent", USER_AGENT, NULL);
	loaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	watchers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	fetching = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}


/* Kept pictures are named by a checksum of their address, which changes whenever a bouncer stores a new one */
static char *build_cache_path(const char *url, const char *suffix) {
	g_autofree char *name = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
	g_autofree char *file = g_strconcat(name, suffix, NULL);
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, CACHE_FOLDER, file, NULL);
}


static void apply_texture(GtkWidget *widget, GdkTexture *texture) {
	if (ADW_IS_AVATAR(widget)) {
		adw_avatar_set_custom_image(ADW_AVATAR(widget), GDK_PAINTABLE(texture));
	} else if (GTK_IS_PICTURE(widget)) {
		gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
	}
}


static gboolean is_showing(GtkWidget *widget, const char *url) {
	return widget && g_strcmp0(g_object_get_data(G_OBJECT(widget), "image-url"), url) == 0;
}


/* Widgets stay listed while they show the address, so a newer version of the picture gets to them too */
static void watch_image(GtkWidget *widget, const char *url) {
	GPtrArray *targets = g_hash_table_lookup(watchers, url);
	if (!targets) {
		targets = g_ptr_array_new_with_free_func((GDestroyNotify)free_weak_reference);
		g_hash_table_insert(watchers, g_strdup(url), targets);
	}
	for (guint index = targets->len; index > 0; index--) {
		g_autoptr(GtkWidget) target = g_weak_ref_get(g_ptr_array_index(targets, index - 1));
		if (target == widget)
			return;
		if (!is_showing(target, url))
			g_ptr_array_remove_index_fast(targets, index - 1);
	}
	GWeakRef *reference = g_new0(GWeakRef, 1);
	g_weak_ref_init(reference, widget);
	g_ptr_array_add(targets, reference);
}


static void hand_out_texture(const char *url, GdkTexture *texture) {
	/* The texture is shared first, so a widget rebuilt on arrival finds it at once */
	if (texture)
		g_hash_table_insert(loaded, g_strdup(url), g_object_ref(texture));
	GPtrArray *targets = g_hash_table_lookup(watchers, url);
	for (guint index = targets ? targets->len : 0; index > 0; index--) {
		g_autoptr(GtkWidget) widget = g_weak_ref_get(g_ptr_array_index(targets, index - 1));
		if (!is_showing(widget, url))
			g_ptr_array_remove_index_fast(targets, index - 1);
		else if (texture)
			apply_texture(widget, texture);
		else if (g_object_get_data(G_OBJECT(widget), "ready"))
			g_object_notify(G_OBJECT(widget), "paintable");
	}
	if (targets && !targets->len)
		g_hash_table_remove(watchers, url);
}


static void fit_picture(GdkPixbufLoader *loader, int width, int height, gpointer data) {
	double scale = (double)LARGEST_SHOWN_PIXELS / MAX(width, height);
	if (scale < 1)
		gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(width * scale)), MAX(1, (int)(height * scale)));
}


static GdkTexture *decode_fitted_picture(GBytes *bytes) {
	g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
	g_signal_connect(loader, "size-prepared", G_CALLBACK(fit_picture), NULL);
	gboolean is_written = gdk_pixbuf_loader_write_bytes(loader, bytes, NULL);
	if (!gdk_pixbuf_loader_close(loader, NULL) || !is_written)
		return NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
	if (!pixbuf)
		return NULL;
	g_autoptr(GBytes) pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
	return gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
		gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, pixels,
		(gsize)gdk_pixbuf_get_rowstride(pixbuf));
}


/* Decoding a large picture takes long enough to hold up drawing, so it happens on a worker thread, and a format the
 * picture loaders lack is still read at full size by the toolkit */
static GdkTexture *decode_picture(GBytes *bytes, GError **error) {
	GdkTexture *texture = decode_fitted_picture(bytes);
	return texture ? texture : gdk_texture_new_from_bytes(bytes, error);
}


/* A downloaded picture that can be read is written to disk with the version its server named, on the same thread */
static void keep_picture(Fetch *fetch) {
	g_autofree char *folder = g_path_get_dirname(fetch->path);
	g_autofree char *version_path = g_strconcat(fetch->path, VERSION_SUFFIX, NULL);
	g_mkdir_with_parents(folder, 0700);
	gsize length = 0;
	const char *contents = g_bytes_get_data(fetch->bytes, &length);
	g_file_set_contents(fetch->path, contents, (gssize)length, NULL);
	if (!fetch->version) {
		g_unlink(version_path);
		return;
	}
	g_autofree char *version = g_strconcat(fetch->version_header, "\n", fetch->version, NULL);
	g_file_set_contents(version_path, version, -1, NULL);
}


static void decode_download(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	Fetch *fetch = data;
	GError *error = NULL;
	GdkTexture *texture = decode_picture(fetch->bytes, &error);
	if (!texture) {
		g_task_return_error(task, error);
		return;
	}
	if (fetch->is_kept)
		keep_picture(fetch);
	g_task_return_pointer(task, texture, g_object_unref);
}


/* A picture on disk, the user's own or one kept from an earlier start, is read on a thread like a download */
static void read_picture_file(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	Fetch *fetch = data;
	GError *error = NULL;
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(fetch->path, &contents, &length, &error)) {
		g_task_return_error(task, error);
		return;
	}
	g_autoptr(GBytes) bytes = g_bytes_new_take(contents, length);
	GdkTexture *texture = decode_picture(bytes, &error);
	if (texture)
		g_task_return_pointer(task, texture, g_object_unref);
	else
		g_task_return_error(task, error);
}


static void finish_fetch(Fetch *fetch, GdkTexture *texture) {
	if (texture || !fetch->is_cached)
		hand_out_texture(fetch->url, texture);
	g_hash_table_remove(fetching, fetch->url);
	free_fetch(fetch);
}


static void finish_decoding(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GdkTexture) texture = g_task_propagate_pointer(G_TASK(result), NULL);
	finish_fetch(data, texture);
}


static void finish_download(GObject *source, GAsyncResult *result, gpointer data) {
	Fetch *fetch = data;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	gboolean is_usable = bytes && message && soup_message_get_status(message) == SOUP_STATUS_OK
		&& g_bytes_get_size(bytes) <= LARGEST_IMAGE_BYTES;
	if (!is_usable) {
		finish_fetch(fetch, NULL);
		return;
	}
	SoupMessageHeaders *headers = soup_message_get_response_headers(message);
	const char *tag = soup_message_headers_get_one(headers, "ETag");
	const char *modified = soup_message_headers_get_one(headers, "Last-Modified");
	fetch->version_header = g_strdup(tag ? "If-None-Match" : "If-Modified-Since");
	fetch->version = g_strdup(tag ? tag : modified);
	fetch->bytes = g_bytes_ref(bytes);
	GTask *task = g_task_new(NULL, NULL, finish_decoding, fetch);
	g_task_set_task_data(task, fetch, NULL);
	g_task_run_in_thread(task, decode_download);
	g_object_unref(task);
}


static gboolean accept_trusted_certificate(SoupMessage *message, GTlsCertificate *certificate,
	GTlsCertificateFlags errors, gpointer data) {
	Window *window = get_main_window();
	return window && is_trusted_certificate(window->chat, certificate);
}


/* A kept picture is requested again only where its server named a version, and an unchanged one costs no
 * download, while a server that named none changes the address for a new picture */
static void download_image(Fetch *fetch) {
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, fetch->url);
	g_autofree char *version_path = fetch->is_cached ? g_strconcat(fetch->path, VERSION_SUFFIX, NULL) : NULL;
	g_autofree char *version = NULL;
	if (version_path && !g_file_get_contents(version_path, &version, NULL, NULL))
		g_clear_pointer(&version, g_free);
	g_auto(GStrv) parts = version ? g_strsplit(version, "\n", 2) : NULL;
	if (!message || (fetch->is_cached && (!parts || g_strv_length(parts) != 2))) {
		finish_fetch(fetch, NULL);
		return;
	}
	if (parts)
		soup_message_headers_replace(soup_message_get_request_headers(message), parts[0], parts[1]);
	g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_trusted_certificate), NULL);
	soup_session_send_and_read_async(web, message, G_PRIORITY_LOW, NULL, finish_download, fetch);
}


/* A kept picture shows from disk at once and is then checked for a newer version, and a missing one downloads */
static void finish_reading(GObject *source, GAsyncResult *result, gpointer data) {
	Fetch *fetch = data;
	g_autoptr(GdkTexture) texture = g_task_propagate_pointer(G_TASK(result), NULL);
	if (!fetch->is_kept) {
		finish_fetch(fetch, texture);
		return;
	}
	fetch->is_cached = texture != NULL;
	if (texture)
		hand_out_texture(fetch->url, texture);
	download_image(fetch);
}


static void read_image(Fetch *fetch) {
	GTask *task = g_task_new(NULL, NULL, finish_reading, fetch);
	g_task_set_task_data(task, fetch, NULL);
	g_task_run_in_thread(task, read_picture_file);
	g_object_unref(task);
}


static void request_image(GtkWidget *widget, const char *url, gboolean is_kept) {
	ensure_tables();
	g_object_set_data_full(G_OBJECT(widget), "image-url", g_strdup(url), g_free);
	watch_image(widget, url);
	GdkTexture *texture = g_hash_table_lookup(loaded, url);
	if (texture) {
		apply_texture(widget, texture);
		return;
	}
	if (g_hash_table_contains(fetching, url))
		return;
	g_hash_table_add(fetching, g_strdup(url));
	Fetch *fetch = g_new0(Fetch, 1);
	fetch->url = g_strdup(url);
	gboolean is_own = g_str_has_prefix(url, "file://");
	fetch->is_kept = is_kept && !is_own;
	fetch->path = is_own ? g_filename_from_uri(url, NULL, NULL) : is_kept ? build_cache_path(url, "") : NULL;
	if (fetch->path)
		read_image(fetch);
	else if (is_own)
		finish_fetch(fetch, NULL);
	else
		download_image(fetch);
}


/* Pictures only come over HTTPS, since a plain address would show who reads the chat to anyone on the way, or
 * from the user's own chosen pictures on disk */
static gboolean is_secure_link(const char *url) {
	return url && (g_str_has_prefix(url, "https://") || g_str_has_prefix(url, "file://"));
}


void show_avatar_image(AdwAvatar *avatar, const char *url) {
	if (g_strcmp0(g_object_get_data(G_OBJECT(avatar), "image-url"), url) == 0)
		return;
	adw_avatar_set_custom_image(avatar, NULL);
	g_object_set_data(G_OBJECT(avatar), "image-url", NULL);
	/* An icon the app ships, drawn from the icon theme so it stays sharp at every size */
	if (url && g_str_has_prefix(url, ICON_PICTURE_PREFIX)) {
		GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(GTK_WIDGET(avatar)));
		int size = adw_avatar_get_size(avatar);
		g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon(theme, url + strlen(ICON_PICTURE_PREFIX), NULL,
			size, gtk_widget_get_scale_factor(GTK_WIDGET(avatar)), GTK_TEXT_DIR_NONE, 0);
		adw_avatar_set_custom_image(avatar, GDK_PAINTABLE(icon));
		g_object_set_data_full(G_OBJECT(avatar), "image-url", g_strdup(url), g_free);
		return;
	}
	if (is_secure_link(url))
		request_image(GTK_WIDGET(avatar), url, TRUE);
}


GdkTexture *get_loaded_image(const char *url) {
	ensure_tables();
	return g_hash_table_lookup(loaded, url);
}


/* Who waits for a preloaded picture and what to call when it is ready */
typedef struct {
	ImageReady ready;
	GWeakRef owner;
} Preload;


static void tell_owner(GObject *helper, GParamSpec *spec, gpointer data) {
	Preload *preload = data;
	g_autoptr(GObject) owner = g_weak_ref_get(&preload->owner);
	if (owner && gtk_picture_get_paintable(GTK_PICTURE(helper)))
		preload->ready(owner);
	g_weak_ref_clear(&preload->owner);
	g_free(preload);
	g_object_unref(helper);
}


/* A hidden picture stands in for the owner while loading, which reuses the loader's list of watchers */
void preload_image(const char *url, ImageReady ready, GObject *owner) {
	if (!is_secure_link(url) || get_loaded_image(url))
		return;
	GtkWidget *helper = g_object_ref_sink(gtk_picture_new());
	Preload *preload = g_new0(Preload, 1);
	preload->ready = ready;
	g_weak_ref_init(&preload->owner, owner);
	g_object_set_data(G_OBJECT(helper), "ready", preload);
	g_signal_connect(helper, "notify::paintable", G_CALLBACK(tell_owner), preload);
	request_image(helper, url, FALSE);
}


char *fill_image_size(const char *url, int size) {
	if (!url || !strstr(url, SIZE_PLACEHOLDER))
		return g_strdup(url);
	g_autofree char *number = g_strdup_printf("%d", size);
	g_auto(GStrv) parts = g_strsplit(url, SIZE_PLACEHOLDER, -1);
	return g_strjoinv(number, parts);
}
