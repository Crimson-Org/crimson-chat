#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../lang/en.h"
#include "common/key-store.h"
#include "images.h"
#include "pictures.h"
#include "room-look.h"
#include "session.h"
#include "uploads.h"

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


/* The address is kept so callers can hold it like a string the server sent */
static const char *describe_picture_address(const char *file) {
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


const char *find_own_picture(CrimsonNetwork *network, PictureKind kind, const char *name) {
	if (!network)
		return NULL;
	g_autofree char *key = describe_picture_key(network, kind, name);
	g_autofree char *file = g_key_file_get_string(get_pictures(), GROUP, key, NULL);
	return file ? describe_picture_address(file) : NULL;
}


/* The address a server is reached at, kept by the bouncer for its networks and in the account otherwise */
static const char *read_network_host(CrimsonNetwork *network) {
	if (network->bouncer_id)
		return g_hash_table_lookup(network->attributes, "host");
	return network->session ? network->session->record->host : NULL;
}


/* The picture the server itself offers, or Crimson's own for the Crimson server */
static const char *find_offered_network_picture(CrimsonNetwork *network) {
	if (network->icon_url)
		return network->icon_url;
	return g_strcmp0(read_network_host(network), CRIMSON_SERVER_HOST) == 0 ? CRIMSON_SERVER_PICTURE : NULL;
}


const char *pick_network_picture(CrimsonNetwork *network) {
	const char *own = find_own_picture(network, PICTURE_SERVER, NULL);
	return own ? own : find_offered_network_picture(network);
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
	/* In a form with a Save button the choice waits there, as a scaled file or as NULL for a removal */
	gboolean waits_for_save, has_pending;
	char *pending;
} PictureChoice;


static void remove_picture_file(const char *file) {
	g_autofree char *folder = get_state_file_path(PICTURES_FOLDER);
	g_autofree char *path = g_build_filename(folder, file, NULL);
	g_remove(path);
}


/* A picture chosen in a form that closed without saving is thrown away */
static void free_picture_choice(PictureChoice *choice) {
	if (choice->pending)
		remove_picture_file(choice->pending);
	g_free(choice->pending);
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


/* Operators, admins and owners of a room may change what the room shows everyone */
static gboolean is_room_operator(CrimsonNetwork *network, const char *name) {
	IrcState *state = get_network_state(network);
	IrcChannel *channel = state && name ? find_irc_channel(state, name) : NULL;
	if (!channel || !state->nick)
		return FALSE;
	g_autofree char *folded = fold_state_name(state, state->nick);
	const char *symbols = g_hash_table_lookup(channel->members, folded);
	return symbols && (strchr(symbols, '@') || strchr(symbols, '&') || strchr(symbols, '~'));
}


/* An operator sets a room's picture for everyone where the bouncer takes uploads, and every other picture the user
 * chooses stays on this computer */
static gboolean is_picture_shared(CrimsonNetwork *network, PictureChoice *choice) {
	if (choice->kind != PICTURE_ROOM || !can_network(network, "draft/metadata-2")
		|| !is_room_operator(network, choice->name))
		return FALSE;
	g_autofree char *upload_url = find_avatar_upload_url(network);
	return upload_url != NULL;
}


static const char *find_shared_room_picture(CrimsonNetwork *network, const char *name) {
	CrimsonRoom *room = find_room(network, name);
	return room ? find_metadata(room, NULL, "avatar") : NULL;
}


/* The user's own picture as the form shows it, with a choice waiting for Save in place of the kept one */
static const char *find_choice_own_picture(CrimsonNetwork *network, PictureChoice *choice) {
	if (choice->has_pending)
		return choice->pending ? describe_picture_address(choice->pending) : NULL;
	return find_own_picture(network, choice->kind, choice->name);
}


static const char *pick_choice_picture(CrimsonNetwork *network, PictureChoice *choice) {
	const char *own = find_choice_own_picture(network, choice);
	if (own)
		return own;
	if (choice->kind == PICTURE_SERVER)
		return find_offered_network_picture(network);
	if (choice->kind == PICTURE_PERSON)
		return find_metadata(network->server_room, choice->name, "avatar");
	return find_shared_room_picture(network, choice->name);
}


/* What clicking a picture or its button does, decided again on each click since pictures and operators change */
typedef enum {
	PICTURE_CHOOSE_OWN,
	PICTURE_CHOOSE_SHARED,
	PICTURE_REMOVE_OWN,
	PICTURE_REMOVE_SHARED,
} PictureAction;

/* The words for each action, kept short so the question fits on one line in a narrow window */
static const struct {
	const char *tooltip, *title, *body, *answer;
	gboolean is_removal;
} ACTIONS[] = {
	[PICTURE_CHOOSE_OWN] = {TEXT_CHOOSE_OWN_PICTURE, TEXT_CHOOSE_OWN_PICTURE_TITLE, TEXT_CHOOSE_OWN_PICTURE_BODY,
		TEXT_CHOOSE_PICTURE, FALSE},
	[PICTURE_CHOOSE_SHARED] = {TEXT_CHOOSE_SHARED_PICTURE, TEXT_CHOOSE_SHARED_PICTURE_TITLE,
		TEXT_CHOOSE_SHARED_PICTURE_BODY, TEXT_CHOOSE_PICTURE, FALSE},
	[PICTURE_REMOVE_OWN] = {TEXT_REMOVE_OWN_PICTURE, TEXT_REMOVE_OWN_PICTURE_TITLE, TEXT_REMOVE_OWN_PICTURE_BODY,
		TEXT_REMOVE, TRUE},
	[PICTURE_REMOVE_SHARED] = {TEXT_REMOVE_SHARED_PICTURE, TEXT_REMOVE_SHARED_PICTURE_TITLE,
		TEXT_REMOVE_SHARED_PICTURE_BODY, TEXT_REMOVE, TRUE},
};


/* The button removes the picture on show, the user's own first, and offers to choose one while none is set */
static PictureAction decide_button_action(CrimsonNetwork *network, PictureChoice *choice) {
	if (find_choice_own_picture(network, choice))
		return PICTURE_REMOVE_OWN;
	if (!is_picture_shared(network, choice))
		return PICTURE_CHOOSE_OWN;
	const char *shared = find_shared_room_picture(network, choice->name);
	return shared && *shared ? PICTURE_REMOVE_SHARED : PICTURE_CHOOSE_SHARED;
}


/* The picture, its button and the sign on a picture only the user sees follow what is in place now */
static void refresh_picture_choice(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	CrimsonNetwork *network = find_choice_network(template);
	if (!network)
		return;
	int size = adw_avatar_get_size(ADW_AVATAR(avatar));
	g_autofree char *sized = fill_image_size(pick_choice_picture(network, template), size * 2);
	show_avatar_image(ADW_AVATAR(avatar), sized);
	PictureAction change = is_picture_shared(network, template) ? PICTURE_CHOOSE_SHARED : PICTURE_CHOOSE_OWN;
	gtk_widget_set_tooltip_text(avatar, ACTIONS[change].tooltip);
	PictureAction action = decide_button_action(network, template);
	GtkWidget *button = g_object_get_data(G_OBJECT(avatar), "picture-button");
	gtk_button_set_icon_name(GTK_BUTTON(button), ACTIONS[action].is_removal ? "user-trash-symbolic"
		: "image-x-generic-symbolic");
	gtk_widget_set_tooltip_text(button, ACTIONS[action].tooltip);
	GtkWidget *local = g_object_get_data(G_OBJECT(avatar), "picture-local");
	gtk_widget_set_visible(local, find_choice_own_picture(network, template) != NULL);
}


/* Everything showing the picture is drawn again: the server's rail icon and messages, the room, or the person's
 * private chat, with the conversation and its members */
static void show_new_picture(PictureChoice *choice, const char *file) {
	CrimsonNetwork *network = find_choice_network(choice);
	if (!network)
		return;
	g_autofree char *key = describe_picture_key(network, choice->kind, choice->name);
	g_autofree char *old = g_key_file_get_string(get_pictures(), GROUP, key, NULL);
	if (old)
		remove_picture_file(old);
	if (file)
		g_key_file_set_string(get_pictures(), GROUP, key, file);
	else
		g_key_file_remove_key(get_pictures(), GROUP, key, NULL);
	save_pictures();
	g_autoptr(GObject) avatar = g_weak_ref_get(&choice->avatar);
	if (avatar)
		refresh_picture_choice(GTK_WIDGET(avatar));
	announce_network_change(network);
	announce_room_change(network->server_room);
	CrimsonRoom *room = choice->name ? find_room(network, choice->name) : NULL;
	if (room)
		announce_room_change(room);
	refresh_window_later(choice->window);
}


static PictureChoice *copy_choice(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	PictureChoice *choice = g_new0(PictureChoice, 1);
	*choice = (PictureChoice){template->window, g_strdup(template->network_key), g_strdup(template->name),
		template->kind, {{0}}, FALSE, FALSE, NULL};
	g_weak_ref_init(&choice->avatar, avatar);
	return choice;
}


/* A form keeps the choice until Save, and anywhere else it takes effect at once */
static void take_picture(PictureChoice *choice, const char *file) {
	g_autoptr(GObject) avatar = g_weak_ref_get(&choice->avatar);
	PictureChoice *template = avatar ? g_object_get_data(avatar, "picture-choice") : NULL;
	if (!template) {
		if (file)
			remove_picture_file(file);
		return;
	}
	if (!template->waits_for_save) {
		show_new_picture(choice, file);
		return;
	}
	if (template->pending)
		remove_picture_file(template->pending);
	g_free(template->pending);
	template->pending = g_strdup(file);
	template->has_pending = TRUE;
	refresh_picture_choice(GTK_WIDGET(avatar));
}


void save_picture_choice(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	if (!template || !template->has_pending)
		return;
	/* The kept picture now owns the file, so closing the form no longer throws it away */
	g_autofree char *file = g_steal_pointer(&template->pending);
	template->has_pending = FALSE;
	PictureChoice *choice = copy_choice(avatar);
	show_new_picture(choice, file);
	free_picture_choice(choice);
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
		take_picture(choice, file);
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


/* Once everyone has the new picture, the user's own one would only hide it from them */
static void finish_sharing(const char *url, const char *problem, gpointer data) {
	PictureChoice *choice = data;
	g_autoptr(GObject) avatar = g_weak_ref_get(&choice->avatar);
	if (avatar)
		gtk_widget_set_sensitive(GTK_WIDGET(avatar), TRUE);
	CrimsonNetwork *network = find_choice_network(choice);
	if (url && network && find_own_picture(network, choice->kind, choice->name))
		show_new_picture(choice, NULL);
	if (problem)
		show_toast(choice->window, problem);
	free_picture_choice(choice);
}


static void finish_choosing_shared(GObject *source, GAsyncResult *result, gpointer data) {
	PictureChoice *choice = data;
	g_autoptr(GFile) chosen = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	CrimsonNetwork *network = find_choice_network(choice);
	if (!chosen || !network) {
		free_picture_choice(choice);
		return;
	}
	g_autoptr(GObject) avatar = g_weak_ref_get(&choice->avatar);
	if (avatar)
		gtk_widget_set_sensitive(GTK_WIDGET(avatar), FALSE);
	upload_avatar(network, choice->name, chosen, finish_sharing, choice);
}


/* The bouncer only reads these kinds of picture, while one kept here may be any picture the computer opens */
static void open_picture_dialog(GtkWidget *avatar, gboolean is_shared) {
	PictureChoice *choice = copy_choice(avatar);
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_CHOOSE_PICTURE_TITLE);
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_PICTURES);
	if (is_shared) {
		gtk_file_filter_add_mime_type(filter, "image/png");
		gtk_file_filter_add_mime_type(filter, "image/jpeg");
		gtk_file_filter_add_mime_type(filter, "image/webp");
		gtk_file_filter_add_mime_type(filter, "image/gif");
	} else {
		gtk_file_filter_add_mime_type(filter, "image/*");
	}
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, filter);
	gtk_file_dialog_open(dialog, GTK_WINDOW(choice->window->window), NULL,
		is_shared ? finish_choosing_shared : finish_choosing, choice);
}


static void remove_shared_picture(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	CrimsonNetwork *network = find_choice_network(template);
	if (network && network->client)
		send_irc_client_command(network->client, "METADATA", template->name, "SET", "avatar", NULL);
}


static void finish_asking(AdwAlertDialog *alert, const char *response, GtkWidget *avatar) {
	if (g_strcmp0(response, "confirm") != 0)
		return;
	PictureAction action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(alert), "picture-action"));
	if (action == PICTURE_CHOOSE_OWN || action == PICTURE_CHOOSE_SHARED) {
		open_picture_dialog(avatar, action == PICTURE_CHOOSE_SHARED);
	} else if (action == PICTURE_REMOVE_OWN) {
		PictureChoice *choice = copy_choice(avatar);
		take_picture(choice, NULL);
		free_picture_choice(choice);
	} else {
		remove_shared_picture(avatar);
	}
}


/* Every change asks first, saying whether only the user or everyone in the room will see it */
static void ask_about_picture(GtkWidget *avatar, PictureAction action) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	AdwDialog *alert = adw_alert_dialog_new(ACTIONS[action].title, ACTIONS[action].body);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "confirm", ACTIONS[action].answer,
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "confirm",
		ACTIONS[action].is_removal ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
	g_object_set_data(G_OBJECT(alert), "picture-action", GINT_TO_POINTER(action));
	g_signal_connect(alert, "response", G_CALLBACK(finish_asking), avatar);
	adw_dialog_present(alert, GTK_WIDGET(template->window->window));
}


static void change_picture(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	CrimsonNetwork *network = find_choice_network(template);
	if (network)
		ask_about_picture(avatar, is_picture_shared(network, template) ? PICTURE_CHOOSE_SHARED : PICTURE_CHOOSE_OWN);
}


static void press_picture_button(GtkWidget *avatar) {
	PictureChoice *template = g_object_get_data(G_OBJECT(avatar), "picture-choice");
	CrimsonNetwork *network = find_choice_network(template);
	if (network)
		ask_about_picture(avatar, decide_button_action(network, template));
}


/* The avatar inside an overlay for its corner signs, calling change with data when clicked */
static GtkWidget *wrap_picture(GtkWidget *avatar, GCallback change, gpointer data) {
	gtk_widget_set_cursor_from_name(avatar, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect_swapped(click, "released", change, data);
	gtk_widget_add_controller(avatar, GTK_EVENT_CONTROLLER(click));
	GtkWidget *overlay = gtk_overlay_new();
	gtk_widget_add_css_class(overlay, "picture-choice");
	gtk_overlay_set_child(GTK_OVERLAY(overlay), avatar);
	return overlay;
}


/* What the corner button of the user's own picture calls, both with the same data */
typedef struct {
	void (*change)(gpointer data);
	void (*remove)(gpointer data);
	gpointer data;
	gboolean is_removable;
} PictureChange;


static void press_change_button(PictureChange *change) {
	if (change->is_removable)
		change->remove(change->data);
	else
		change->change(change->data);
}


GtkWidget *offer_picture_change(GtkWidget *avatar, GCallback change, GCallback remove, gpointer data) {
	gtk_widget_set_tooltip_text(avatar, TEXT_CHANGE_PICTURE);
	GtkWidget *overlay = wrap_picture(avatar, change, data);
	PictureChange *calls = g_new0(PictureChange, 1);
	*calls = (PictureChange){(void (*)(gpointer))change, (void (*)(gpointer))remove, data, FALSE};
	g_object_set_data_full(G_OBJECT(avatar), "picture-change", calls, g_free);
	GtkWidget *button = gtk_button_new();
	gtk_widget_add_css_class(button, "picture-badge");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_set_halign(button, GTK_ALIGN_END);
	gtk_widget_set_valign(button, GTK_ALIGN_END);
	g_signal_connect_swapped(button, "clicked", G_CALLBACK(press_change_button), calls);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), button);
	g_object_set_data(G_OBJECT(avatar), "picture-button", button);
	show_picture_removable(avatar, FALSE);
	return overlay;
}


void show_picture_removable(GtkWidget *avatar, gboolean is_removable) {
	PictureChange *calls = g_object_get_data(G_OBJECT(avatar), "picture-change");
	GtkWidget *button = g_object_get_data(G_OBJECT(avatar), "picture-button");
	if (!calls || !button)
		return;
	calls->is_removable = is_removable;
	gtk_button_set_icon_name(GTK_BUTTON(button), is_removable ? "user-trash-symbolic" : "image-x-generic-symbolic");
	gtk_widget_set_tooltip_text(button, is_removable ? TEXT_REMOVE_YOUR_PICTURE : TEXT_CHANGE_PICTURE);
}


/* A sign in the lower left that tells, when clicked, that nobody else sees the picture */
static GtkWidget *create_local_sign(void) {
	GtkWidget *sign = gtk_menu_button_new();
	/* A label as the child keeps the arrow a menu button draws beside a text label */
	gtk_menu_button_set_child(GTK_MENU_BUTTON(sign), gtk_label_new("!"));
	gtk_widget_add_css_class(sign, "picture-local");
	gtk_widget_set_tooltip_text(sign, TEXT_PICTURE_ONLY_YOURS);
	gtk_widget_set_halign(sign, GTK_ALIGN_START);
	gtk_widget_set_valign(sign, GTK_ALIGN_END);
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), gtk_label_new(TEXT_PICTURE_ONLY_YOURS));
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(sign), popover);
	return sign;
}


GtkWidget *offer_picture_choice(GtkWidget *avatar, Window *window, CrimsonNetwork *network, PictureKind kind,
	const char *name, gboolean waits_for_save) {
	PictureChoice *choice = g_new0(PictureChoice, 1);
	*choice = (PictureChoice){window, g_strdup(network->key), g_strdup(name), kind, {{0}}, waits_for_save, FALSE, NULL};
	g_weak_ref_init(&choice->avatar, NULL);
	g_object_set_data_full(G_OBJECT(avatar), "picture-choice", choice, (GDestroyNotify)free_picture_choice);
	GtkWidget *overlay = wrap_picture(avatar, G_CALLBACK(change_picture), avatar);
	GtkWidget *button = gtk_button_new();
	gtk_widget_add_css_class(button, "picture-badge");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_set_halign(button, GTK_ALIGN_END);
	gtk_widget_set_valign(button, GTK_ALIGN_END);
	g_signal_connect_swapped(button, "clicked", G_CALLBACK(press_picture_button), avatar);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), button);
	g_object_set_data(G_OBJECT(avatar), "picture-button", button);
	GtkWidget *local = create_local_sign();
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), local);
	g_object_set_data(G_OBJECT(avatar), "picture-local", local);
	/* New pictures from the server and a change of operators change what the button does */
	g_signal_connect_object(network, "changed", G_CALLBACK(refresh_picture_choice), avatar, G_CONNECT_SWAPPED);
	CrimsonRoom *room = name ? find_room(network, name) : NULL;
	if (room)
		g_signal_connect_object(room, "changed", G_CALLBACK(refresh_picture_choice), avatar, G_CONNECT_SWAPPED);
	refresh_picture_choice(avatar);
	return overlay;
}
