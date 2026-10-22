#include <libsoup/soup.h>
#include <string.h>

#include "images.h"

#define LARGEST_IMAGE_BYTES (10 * 1024 * 1024)
#define SIZE_PLACEHOLDER "{size}"
#define TIMEOUT_SECONDS 20
/* Some picture hosts refuse requests that do not name the program asking */
#define USER_AGENT "CrimsonChat/" APPLICATION_VERSION

static SoupSession *web;
/* Addresses to their loaded textures, and to the widgets waiting while one downloads */
static GHashTable *loaded, *waiting;


static void free_weak_reference(GWeakRef *reference) {
	g_weak_ref_clear(reference);
	g_free(reference);
}


static void ensure_tables(void) {
	if (web)
		return;
	web = soup_session_new_with_options("timeout", TIMEOUT_SECONDS, "user-agent", USER_AGENT, NULL);
	loaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
}


static void apply_texture(GtkWidget *widget, GdkTexture *texture) {
	if (ADW_IS_AVATAR(widget)) {
		adw_avatar_set_custom_image(ADW_AVATAR(widget), GDK_PAINTABLE(texture));
	} else if (GTK_IS_PICTURE(widget)) {
		gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
	}
}


static void hand_out_texture(const char *url, GdkTexture *texture) {
	/* The texture is shared first, so a widget rebuilt on arrival finds it at once */
	if (texture)
		g_hash_table_insert(loaded, g_strdup(url), g_object_ref(texture));
	GPtrArray *targets = g_hash_table_lookup(waiting, url);
	for (guint index = 0; targets && index < targets->len; index++) {
		g_autoptr(GtkWidget) widget = g_weak_ref_get(g_ptr_array_index(targets, index));
		if (!widget)
			continue;
		if (texture && g_strcmp0(g_object_get_data(G_OBJECT(widget), "image-url"), url) == 0)
			apply_texture(widget, texture);
		else if (!texture && g_object_get_data(G_OBJECT(widget), "ready"))
			g_object_notify(G_OBJECT(widget), "paintable");
	}
	g_hash_table_remove(waiting, url);
}


/* Decoding a large picture takes long enough to hold up drawing, so it happens on a worker thread */
static void decode_texture(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	GError *error = NULL;
	GdkTexture *texture = gdk_texture_new_from_bytes(data, &error);
	if (texture)
		g_task_return_pointer(task, texture, g_object_unref);
	else
		g_task_return_error(task, error);
}


static void finish_decoding(GObject *source, GAsyncResult *result, gpointer data) {
	g_autofree char *url = data;
	g_autoptr(GdkTexture) texture = g_task_propagate_pointer(G_TASK(result), NULL);
	hand_out_texture(url, texture);
}


static void finish_download(GObject *source, GAsyncResult *result, gpointer data) {
	char *url = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	gboolean is_usable = bytes && message && soup_message_get_status(message) == SOUP_STATUS_OK
		&& g_bytes_get_size(bytes) <= LARGEST_IMAGE_BYTES;
	if (!is_usable) {
		hand_out_texture(url, NULL);
		g_free(url);
		return;
	}
	GTask *task = g_task_new(NULL, NULL, finish_decoding, url);
	g_task_set_task_data(task, g_bytes_ref(bytes), (GDestroyNotify)g_bytes_unref);
	g_task_run_in_thread(task, decode_texture);
	g_object_unref(task);
}


static void request_image(GtkWidget *widget, const char *url) {
	ensure_tables();
	g_object_set_data_full(G_OBJECT(widget), "image-url", g_strdup(url), g_free);
	GdkTexture *texture = g_hash_table_lookup(loaded, url);
	if (texture) {
		apply_texture(widget, texture);
		return;
	}
	GWeakRef *reference = g_new0(GWeakRef, 1);
	g_weak_ref_init(reference, widget);
	GPtrArray *targets = g_hash_table_lookup(waiting, url);
	if (targets) {
		g_ptr_array_add(targets, reference);
		return;
	}
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, url);
	if (!message) {
		free_weak_reference(reference);
		return;
	}
	targets = g_ptr_array_new_with_free_func((GDestroyNotify)free_weak_reference);
	g_ptr_array_add(targets, reference);
	g_hash_table_insert(waiting, g_strdup(url), targets);
	soup_session_send_and_read_async(web, message, G_PRIORITY_LOW, NULL, finish_download, g_strdup(url));
}


/* Pictures only come over HTTPS, since a plain address would show who reads the chat to anyone on the way */
static gboolean is_secure_link(const char *url) {
	return url && g_str_has_prefix(url, "https://");
}


void show_avatar_image(AdwAvatar *avatar, const char *url) {
	if (g_strcmp0(g_object_get_data(G_OBJECT(avatar), "image-url"), url) == 0)
		return;
	adw_avatar_set_custom_image(avatar, NULL);
	g_object_set_data(G_OBJECT(avatar), "image-url", NULL);
	if (is_secure_link(url))
		request_image(GTK_WIDGET(avatar), url);
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


/* A hidden picture stands in for the owner while loading, which reuses the loader's waiting list */
void preload_image(const char *url, ImageReady ready, GObject *owner) {
	if (!is_secure_link(url) || get_loaded_image(url))
		return;
	GtkWidget *helper = g_object_ref_sink(gtk_picture_new());
	Preload *preload = g_new0(Preload, 1);
	preload->ready = ready;
	g_weak_ref_init(&preload->owner, owner);
	g_object_set_data(G_OBJECT(helper), "ready", preload);
	g_signal_connect(helper, "notify::paintable", G_CALLBACK(tell_owner), preload);
	request_image(helper, url);
}


char *fill_image_size(const char *url, int size) {
	if (!url || !strstr(url, SIZE_PLACEHOLDER))
		return g_strdup(url);
	g_autofree char *number = g_strdup_printf("%d", size);
	g_auto(GStrv) parts = g_strsplit(url, SIZE_PLACEHOLDER, -1);
	return g_strjoinv(number, parts);
}
