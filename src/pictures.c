#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include "../lang/en.h"
#include "common/key-store.h"
#include "images.h"
#include "pictures.h"
#include "room-look.h"

#define PICTURES_FILE "pictures.ini"
#define PICTURES_FOLDER "pictures"
#define GROUP "pictures"
/* Chosen pictures are scaled down to this, enough for the largest avatar on a sharp screen */
#define PICTURE_PIXELS 512

static GKeyFile *pictures;


static GKeyFile *get_pictures(void) {
	if (!pictures) {
		g_autofree char *path = get_state_file_path(PICTURES_FILE);
		pictures = load_key_file(path, G_KEY_FILE_NONE);
	}
	return pictures;
}


/* Names can hold characters a key file cannot, so each picture is found by a checksum of what it belongs to */
static char *describe_picture_key(CrimsonNetwork *network, PictureKind kind, const char *name) {
	g_autofree char *folded = name ? g_utf8_casefold(name, -1) : g_strdup("");
	g_autofree char *whole = g_strdup_printf("%s\n%d\n%s", network->key, kind, folded);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, whole, -1);
}


const char *find_own_picture(CrimsonNetwork *network, PictureKind kind, const char *name) {
	if (!network)
		return NULL;
	g_autofree char *key = describe_picture_key(network, kind, name);
	g_autofree char *file = g_key_file_get_string(get_pictures(), GROUP, key, NULL);
	if (!file)
		return NULL;
	/* The address is kept so callers can hold it like a string the server sent */
	static GHashTable *addresses;
	if (!addresses)
		addresses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *address = g_hash_table_lookup(addresses, file);
	if (!address) {
		g_autofree char *path = get_state_file_path(PICTURES_FOLDER);
		g_autofree char *full = g_build_filename(path, file, NULL);
		char *uri = g_filename_to_uri(full, NULL, NULL);
		g_hash_table_insert(addresses, g_strdup(file), uri);
		address = uri;
	}
	return address;
}


/* The address a server is reached at, kept by the bouncer for its networks and in the account otherwise */
static const char *read_network_host(CrimsonNetwork *network) {
	if (network->bouncer_id)
		return g_hash_table_lookup(network->attributes, "host");
	return network->session ? network->session->record->host : NULL;
}


const char *pick_network_picture(CrimsonNetwork *network) {
	const char *own = find_own_picture(network, PICTURE_SERVER, NULL);
	if (own)
		return own;
	if (network->icon_url)
		return network->icon_url;
	return g_strcmp0(read_network_host(network), CRIMSON_SERVER_HOST) == 0 ? CRIMSON_SERVER_PICTURE : NULL;
}


const char *pick_person_picture(CrimsonRoom *room, const char *nick) {
	const char *own = find_own_picture(room->network, PICTURE_PERSON, nick);
	return own ? own : find_metadata(room, nick, "avatar");
}


static void save_pictures(void) {
	g_autofree char *path = get_state_file_path(PICTURES_FILE);
	g_autofree char *folder = g_path_get_dirname(path);
	g_mkdir_with_parents(folder, 0700);
	g_key_file_save_to_file(get_pictures(), path, NULL);
}


/* What a picture belongs to, kept while the file dialog is open and the picture is scaled */
typedef struct {
	Window *window;
	char *network_key, *name;
	PictureKind kind;
	GWeakRef avatar;
} PictureChoice;


static void free_picture_choice(PictureChoice *choice) {
	g_free(choice->network_key);
	g_free(choice->name);
	g_weak_ref_clear(&choice->avatar);
	g_free(choice);
}


static CrimsonNetwork *find_choice_network(PictureChoice *choice) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(choice->window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(choice->window->chat->networks), index);
		if (g_str_equal(network->key, choice->network_key))
			return network;
	}
	return NULL;
}


/* Everything showing the picture is drawn again: the server's rail icon and messages, the room, or the person's
 * private chat, with the conversation and its members */
static void show_new_picture(PictureChoice *choice, const char *file) {
	CrimsonNetwork *network = find_choice_network(choice);
	if (!network)
		return;
	g_autofree char *key = describe_picture_key(network, choice->kind, choice->name);
	g_autofree char *old = g_key_file_get_string(get_pictures(), GROUP, key, NULL);
	if (old) {
		g_autofree char *folder = get_state_file_path(PICTURES_FOLDER);
		g_autofree char *old_path = g_build_filename(folder, old, NULL);
		g_remove(old_path);
	}
	if (file)
		g_key_file_set_string(get_pictures(), GROUP, key, file);
	else
		g_key_file_remove_key(get_pictures(), GROUP, key, NULL);
	save_pictures();
	g_autoptr(GObject) avatar = g_weak_ref_get(&choice->avatar);
	if (avatar) {
		const char *url = choice->kind == PICTURE_SERVER ? pick_network_picture(network)
			: choice->kind == PICTURE_ROOM ? find_own_picture(network, PICTURE_ROOM, choice->name)
			: find_own_picture(network, PICTURE_PERSON, choice->name);
		CrimsonRoom *room = choice->kind == PICTURE_SERVER ? NULL : find_room(network, choice->name);
		if (room)
			fill_room_avatar(ADW_AVATAR(avatar), room, adw_avatar_get_size(ADW_AVATAR(avatar)));
		else
			show_avatar_image(ADW_AVATAR(avatar), url);
		GtkWidget *remove = g_object_get_data(avatar, "picture-remove");
		if (remove)
			gtk_widget_set_visible(remove, file != NULL);
	}
	announce_network_change(network);
	announce_room_change(network->server_room);
	CrimsonRoom *room = choice->name ? find_room(network, choice->name) : NULL;
	if (room)
		announce_room_change(room);
	refresh_window_later(choice->window);
}


static void scale_picture(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	char **paths = data;
	GError *error = NULL;
	g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file_at_scale(paths[0], PICTURE_PIXELS, PICTURE_PIXELS, TRUE,
		&error);
	if (!pixbuf || !gdk_pixbuf_save(pixbuf, paths[1], "png", &error, NULL)) {
		g_task_return_error(task, error);
		return;
	}
	g_task_return_boolean(task, TRUE);
}


static void finish_scaling(GObject *source, GAsyncResult *result, gpointer data) {
	PictureChoice *choice = data;
	char **paths = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	if (g_task_propagate_boolean(G_TASK(result), &error)) {
		g_autofree char *file = g_path_get_basename(paths[1]);
		show_new_picture(choice, file);
	} else {
		show_toast(choice->window, TEXT_PICTURE_UNREADABLE);
	}
	free_picture_choice(choice);
}


static void finish_choosing(GObject *source, GAsyncResult *result, gpointer data) {
	PictureChoice *choice = data;
	g_autoptr(GFile) chosen = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	g_autofree char *path = chosen ? g_file_get_path(chosen) : NULL;
	if (!path) {
		free_picture_choice(choice);
		return;
	}
	g_autofree char *folder = get_state_file_path(PICTURES_FOLDER);
	g_mkdir_with_parents(folder, 0700);
	/* Each picture gets a new name, so a replaced one is never shown from what was loaded before */
	g_autofree char *id = g_uuid_string_random();
	g_autofree char *file = g_strdup_printf("%s.png", id);
	char **paths = g_new0(char *, 3);
	paths[0] = g_steal_pointer(&path);
	paths[1] = g_build_filename(folder, file, NULL);
	GTask *task = g_task_new(NULL, NULL, finish_scaling, choice);
	g_task_set_task_data(task, paths, (GDestroyNotify)g_strfreev);
	g_task_run_in_thread(task, scale_picture);
	g_object_unref(task);
}


static PictureChoice *copy_choice(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	PictureChoice *choice = g_new0(PictureChoice, 1);
	*choice = (PictureChoice){template->window, g_strdup(template->network_key), g_strdup(template->name),
		template->kind, {{0}}};
	g_weak_ref_init(&choice->avatar, avatar);
	return choice;
}


static void choose_picture(GtkWidget *avatar) {
	PictureChoice *choice = copy_choice(avatar);
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_CHOOSE_PICTURE_TITLE);
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_PICTURES);
	gtk_file_filter_add_mime_type(filter, "image/*");
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, filter);
	gtk_file_dialog_open(dialog, GTK_WINDOW(choice->window->window), NULL, finish_choosing, choice);
}


/* The remove button only shows while the user's own picture is in place */
static void refresh_remove_button(GtkWidget *avatar) {
	GtkWidget *remove = g_object_get_data(G_OBJECT(avatar), "picture-remove");
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	CrimsonNetwork *network = template ? find_choice_network(template) : NULL;
	if (remove)
		gtk_widget_set_visible(remove, network && find_own_picture(network, template->kind, template->name));
}


static void finish_removing(AdwAlertDialog *alert, const char *response, GtkWidget *avatar) {
	if (g_strcmp0(response, "remove") != 0)
		return;
	PictureChoice *choice = copy_choice(avatar);
	show_new_picture(choice, NULL);
	free_picture_choice(choice);
	refresh_remove_button(avatar);
}


static void ask_to_remove(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	AdwDialog *alert = adw_alert_dialog_new(TEXT_REMOVE_PICTURE_TITLE, TEXT_REMOVE_PICTURE_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "remove", TEXT_REMOVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
	g_signal_connect(alert, "response", G_CALLBACK(finish_removing), avatar);
	adw_dialog_present(alert, GTK_WIDGET(template->window->window));
}


GtkWidget *offer_picture_change(GtkWidget *avatar, GCallback change, gpointer data) {
	gtk_widget_set_cursor_from_name(avatar, "pointer");
	gtk_widget_set_tooltip_text(avatar, TEXT_CHANGE_PICTURE);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect_swapped(click, "released", change, data);
	gtk_widget_add_controller(avatar, GTK_EVENT_CONTROLLER(click));
	GtkWidget *overlay = gtk_overlay_new();
	gtk_widget_add_css_class(overlay, "picture-choice");
	gtk_overlay_set_child(GTK_OVERLAY(overlay), avatar);
	GtkWidget *sign = gtk_image_new_from_icon_name("image-x-generic-symbolic");
	gtk_widget_add_css_class(sign, "picture-badge");
	gtk_widget_set_halign(sign, GTK_ALIGN_END);
	gtk_widget_set_valign(sign, GTK_ALIGN_END);
	gtk_widget_set_can_target(sign, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), sign);
	return overlay;
}


GtkWidget *offer_picture_choice(GtkWidget *avatar, Window *window, CrimsonNetwork *network, PictureKind kind,
	const char *name) {
	PictureChoice *choice = g_new0(PictureChoice, 1);
	*choice = (PictureChoice){window, g_strdup(network->key), g_strdup(name), kind, {{0}}};
	g_weak_ref_init(&choice->avatar, NULL);
	g_object_set_data_full(G_OBJECT(avatar), "picture-choice", choice, (GDestroyNotify)free_picture_choice);
	GtkWidget *overlay = offer_picture_change(avatar, G_CALLBACK(choose_picture), avatar);
	/* A remove button sits in the other corner where the user chose a picture */
	GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
	gtk_widget_add_css_class(remove, "picture-badge");
	gtk_widget_add_css_class(remove, "circular");
	gtk_widget_set_tooltip_text(remove, TEXT_REMOVE_PICTURE);
	gtk_widget_set_halign(remove, GTK_ALIGN_END);
	gtk_widget_set_valign(remove, GTK_ALIGN_START);
	g_signal_connect_swapped(remove, "clicked", G_CALLBACK(ask_to_remove), avatar);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), remove);
	g_object_set_data(G_OBJECT(avatar), "picture-remove", remove);
	refresh_remove_button(avatar);
	return overlay;
}
