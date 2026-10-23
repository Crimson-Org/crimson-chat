#include <adwaita.h>

#include "../lang/en.h"
#include "application.h"
#include "dialogs.h"
#include "formatting.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"
#include "wizards.h"

#define ROOM_SEPARATOR "\n"

static Chat *chat;


static char *describe_room_key(CrimsonRoom *room) {
	return g_strconcat(room->network->key, ROOM_SEPARATOR, room->name, NULL);
}


static CrimsonRoom *find_room_by_key(const char *key) {
	g_auto(GStrv) parts = g_strsplit(key, ROOM_SEPARATOR, 2);
	if (g_strv_length(parts) != 2)
		return NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(chat->networks), index);
		if (g_str_equal(network->key, parts[0]))
			return find_room(network, parts[1]);
	}
	return NULL;
}


static void withdraw_notification(CrimsonRoom *room, gpointer data) {
	g_autofree char *key = describe_room_key(room);
	g_application_withdraw_notification(g_application_get_default(), key);
}


/* A desktop notification for a mention or a private message, unless the room is already on screen */
static void notify_message(CrimsonRoom *room, CrimsonMessage *message, gpointer data) {
	Window *window = get_main_window();
	if (window && is_room_in_view(window, room))
		return;
	g_autofree char *title = room->kind == ROOM_PRIVATE ? g_strdup(message->sender)
		: g_strdup_printf(TEXT_NOTIFICATION_TITLE, message->sender, room->name);
	g_autofree char *body = strip_irc_formatting(message->text);
	g_autoptr(GNotification) notification = g_notification_new(title);
	g_notification_set_body(notification, body);
	g_notification_set_category(notification, "im.received");
	g_autofree char *key = describe_room_key(room);
	g_notification_set_default_action_and_target(notification, "app.open-room", "s", key);
	g_application_send_notification(g_application_get_default(), key, notification);
}


static void refuse_certificate(Session *session, const char *hash, gpointer data) {
	Window *window = get_main_window();
	if (window)
		ask_certificate_trust(window, session, hash);
}


static void show_problem(const char *text, gpointer data) {
	Window *window = get_main_window();
	if (window)
		show_toast(window, text);
}


static void open_requested_room(CrimsonRoom *room, gpointer data) {
	Window *window = get_main_window();
	if (window)
		open_room(window, room);
}


static void forget_network(CrimsonNetwork *network, gpointer data) {
	Window *window = get_main_window();
	if (window)
		forget_network_in_window(window, network);
}


static void offer_login(CrimsonNetwork *network, gpointer data) {
	Window *window = get_main_window();
	if (!window)
		return;
	AdwToast *toast = adw_toast_new(TEXT_LOGIN_WANTED);
	adw_toast_set_timeout(toast, 0);
	adw_toast_set_button_label(toast, TEXT_LOG_IN);
	adw_toast_set_action_name(toast, "win.own-profile");
	adw_toast_set_action_target_value(toast, g_variant_new_string(network->key));
	adw_toast_overlay_add_toast(window->toasts, toast);
}


static void share_invite(Session *session, const char *token, const char *uses, const char *port, gpointer data) {
	Window *window = get_main_window();
	if (window)
		show_invite_link(window, session, token, uses, port);
}


static gboolean start_chat_later(gpointer data) {
	start_chat(chat);
	refresh_window_later(data);
	restore_window_choice(data);
	return G_SOURCE_REMOVE;
}


/* The window shows first and the accounts connect after its first frame */
static void activate(GApplication *application) {
	Window *window = get_main_window();
	if (window) {
		gtk_window_present(GTK_WINDOW(window->window));
		return;
	}
	window = create_window(GTK_APPLICATION(application), chat);
	gtk_window_present(GTK_WINDOW(window->window));
	run_after_frame(GTK_WIDGET(window->window), start_chat_later, window);
}


/* crimson-chat:// links from a bouncer's welcome page open the guide with the bouncer filled in */
static void open_links(GApplication *application, GFile **files, int count, const char *hint) {
	activate(application);
	for (int index = 0; index < count; index++) {
		g_autofree char *link = g_file_get_uri(files[index]);
		open_application_link(get_main_window(), link);
	}
}


static void show_about(GSimpleAction *action, GVariant *parameter, gpointer application) {
	AdwDialog *dialog = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), get_application_identity()->name);
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), get_application_identity()->id);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), get_application_identity()->version);
	adw_dialog_present(dialog, GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void open_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	Window *window = get_main_window();
	if (window)
		show_settings(GTK_WIDGET(window->window));
}


static void add_server(GSimpleAction *action, GVariant *parameter, gpointer application) {
	Window *window = get_main_window();
	if (window)
		show_server_guide(window);
}


static void add_bouncer(GSimpleAction *action, GVariant *parameter, gpointer application) {
	Window *window = get_main_window();
	if (window)
		show_bouncer_guide(window, NULL);
}


static void show_window(GSimpleAction *action, GVariant *parameter, gpointer application) {
	activate(application);
}


static void open_room_action(GSimpleAction *action, GVariant *parameter, gpointer application) {
	activate(application);
	CrimsonRoom *room = find_room_by_key(g_variant_get_string(parameter, NULL));
	if (room)
		open_room(get_main_window(), room);
}


static void quit(GSimpleAction *action, GVariant *parameter, gpointer application) {
	Window *window = get_main_window();
	stop_chat(chat);
	if (!window) {
		g_application_quit(application);
		return;
	}
	window->is_quitting = TRUE;
	gtk_window_close(GTK_WINDOW(window->window));
}


static void start_up(GApplication *application) {
	load_settings();
	apply_style();
	static const ChatEvents EVENTS = {notify_message, refuse_certificate, show_problem, open_requested_room,
		share_invite, forget_network, offer_login, withdraw_notification};
	chat = create_chat(&EVENTS, NULL);
	static const GActionEntry ENTRIES[] = {
		{"about", show_about, NULL, NULL, NULL, {0}},
		{"settings", open_settings, NULL, NULL, NULL, {0}},
		{"add-server", add_server, NULL, NULL, NULL, {0}},
		{"add-bouncer", add_bouncer, NULL, NULL, NULL, {0}},
		{"show", show_window, NULL, NULL, NULL, {0}},
		{"open-room", open_room_action, "s", NULL, NULL, {0}},
		{"quit", quit, NULL, NULL, NULL, {0}},
	};
	static const struct {
		const char *action, *accelerators[3];
	} SHORTCUTS[] = {
		{"app.settings", {"<Control>comma", "<Control><Shift>s", NULL}},
		{"app.quit", {"<Control>q", NULL}},
		{"win.hide", {"<Control>w", NULL}},
		{"win.search", {"<Control>f", "<Control>k", NULL}},
		{"win.join-room", {"<Control>j", NULL}},
		{"win.message-someone", {"<Control>n", NULL}},
		{"win.step-room(-1)", {"<Alt>Up", "<Control>Page_Up", NULL}},
		{"win.step-room(1)", {"<Alt>Down", "<Control>Page_Down", NULL}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	for (guint index = 0; index < G_N_ELEMENTS(SHORTCUTS); index++)
		gtk_application_set_accels_for_action(GTK_APPLICATION(application), SHORTCUTS[index].action,
			SHORTCUTS[index].accelerators);
}


const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonChat", TEXT_APPLICATION_NAME, "crimson-chat", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_links,
	.parameter_summary = TEXT_LINKS_OPTION,
};
