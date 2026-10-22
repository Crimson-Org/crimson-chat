#include "../lang/en.h"
#include "common/startup.h"
#include "dialogs.h"
#include "images.h"
#include "profile.h"
#include "room-picker.h"
#include "settings.h"
#include "state.h"
#include "window.h"
#include "wizards.h"

#define NARROW_CONDITION "max-width: 640sp"
#define RAIL_AVATAR_SIZE 48
#define SIDEBAR_FRACTION 0.36
#define SIDEBAR_MINIMUM 340
#define SIDEBAR_MAXIMUM 440
#define BADGE_LIMIT 999
#define NETWORK_KEY "network"
#define BANNER_DELAY_MS 1500
#define APPLICATION_ICON "io.github.CrimsonOrg.CrimsonChat"

static Window *main_window;


Window *get_main_window(void) {
	return main_window;
}


void show_toast(Window *window, const char *text) {
	if (!text || !*text)
		return;
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_timeout(toast, 4);
	adw_toast_overlay_add_toast(window->toasts, toast);
}


static char *format_badge(guint count) {
	return count > BADGE_LIMIT ? g_strdup_printf("%u+", BADGE_LIMIT) : g_strdup_printf("%u", count);
}


void style_unread_badge(GtkWidget *badge, guint count, gboolean has_mention) {
	g_autofree char *text = format_badge(count);
	gtk_label_set_text(GTK_LABEL(badge), text);
	gtk_widget_set_visible(badge, count > 0);
	if (has_mention)
		gtk_widget_add_css_class(badge, "mention");
	else
		gtk_widget_remove_css_class(badge, "mention");
}


static void append_network_item(GMenu *menu, const char *label, const char *action, CrimsonNetwork *network) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(network->key));
	g_menu_append_item(menu, item);
}


GMenuModel *create_server_menu(Window *window, CrimsonNetwork *network) {
	GMenu *items = g_menu_new();
	/* Without the rail, a server's menu is the way to switch to its chats */
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	if (!window->has_rail && count > 1)
		append_network_item(items, TEXT_SHOW_CHATS, "win.choose-server", network);
	append_network_item(items, TEXT_ACCOUNT_MENU, "win.own-profile", network);
	append_network_item(items, TEXT_SERVER_MESSAGES, "win.server-messages", network);
	append_network_item(items, TEXT_SERVER_SETTINGS, "win.network-settings", network);
	append_network_item(items, TEXT_RECONNECT, "win.reconnect-server", network);
	append_network_item(items, TEXT_REMOVE_SERVER, "win.remove-network", network);
	return G_MENU_MODEL(items);
}


static void forget_rail_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking a server picks it and opens everything about it */
static void open_rail_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	CrimsonNetwork *network = g_object_get_data(G_OBJECT(row), NETWORK_KEY);
	choose_network(window, network);
	g_autoptr(GMenuModel) model = create_server_menu(window, network);
	GtkWidget *popover = gtk_popover_menu_new_from_model(model);
	gtk_widget_set_parent(popover, row);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_rail_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


static const char *describe_state(NetworkState state) {
	return state == NETWORK_CONNECTED ? TEXT_CONNECTED : state == NETWORK_CONNECTING ? TEXT_CONNECTING
		: TEXT_DISCONNECTED;
}


static void fill_rail_row(GtkWidget *row, CrimsonNetwork *network) {
	AdwAvatar *avatar = g_object_get_data(G_OBJECT(row), "avatar");
	GtkWidget *badge = g_object_get_data(G_OBJECT(row), "badge");
	adw_avatar_set_text(avatar, network->name);
	g_autofree char *icon = fill_image_size(network->icon_url, RAIL_AVATAR_SIZE * 2);
	show_avatar_image(avatar, icon);
	gboolean has_mention = FALSE;
	guint unread = count_network_unread(network, &has_mention);
	style_unread_badge(badge, unread, has_mention);
	gtk_widget_set_opacity(GTK_WIDGET(avatar), network->state == NETWORK_CONNECTED ? 1.0 : 0.45);
	g_autofree char *tooltip = g_strdup_printf("%s\n%s", network->name, describe_state(network->state));
	gtk_widget_set_tooltip_text(row, tooltip);
}


static void refresh_rail_row(CrimsonNetwork *network, gpointer row) {
	fill_rail_row(row, network);
}


static GtkWidget *create_rail_row(gpointer item, gpointer data) {
	CrimsonNetwork *network = item;
	GtkWidget *row = gtk_list_box_row_new();
	gtk_widget_add_css_class(row, "rail-row");
	GtkWidget *overlay = gtk_overlay_new();
	gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
	GtkWidget *avatar = adw_avatar_new(RAIL_AVATAR_SIZE, NULL, TRUE);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), avatar);
	GtkWidget *badge = gtk_label_new(NULL);
	gtk_widget_add_css_class(badge, "unread-badge");
	gtk_widget_add_css_class(badge, "rail-badge");
	gtk_widget_set_halign(badge, GTK_ALIGN_END);
	gtk_widget_set_valign(badge, GTK_ALIGN_END);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), overlay);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "badge", badge);
	g_object_set_data(G_OBJECT(row), NETWORK_KEY, network);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_rail_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	fill_rail_row(row, network);
	g_signal_connect_object(network, "changed", G_CALLBACK(refresh_rail_row), row, 0);
	/* The chat list header names the server and its state, so it follows the server even with the rail hidden */
	g_signal_connect_swapped(network, "changed", G_CALLBACK(refresh_window_later), data);
	return row;
}


static void refresh_rail(Window *window) {
	for (GtkWidget *row = gtk_widget_get_first_child(window->rail_list); row; row = gtk_widget_get_next_sibling(row)) {
		CrimsonNetwork *network = g_object_get_data(G_OBJECT(row), NETWORK_KEY);
		if (network)
			fill_rail_row(row, network);
	}
}


/* Before there is a server the window only welcomes, and with a bouncer it names it and offers a server */
static void refresh_welcome(Window *window) {
	Session *bouncer = find_bouncer(window->chat);
	AdwStatusPage *page = ADW_STATUS_PAGE(window->welcome);
	gtk_widget_set_visible(window->welcome_bouncer, !bouncer);
	if (!bouncer) {
		adw_status_page_set_title(page, TEXT_WELCOME_TITLE);
		adw_status_page_set_description(page, NULL);
		gtk_widget_remove_css_class(window->welcome_server, "suggested-action");
		return;
	}
	gtk_widget_add_css_class(window->welcome_server, "suggested-action");
	adw_status_page_set_title(page, describe_account_name(bouncer));
	NetworkState state = read_account_state(bouncer);
	const char *error = read_account_error(bouncer);
	/* A bouncer reached before only waits to reconnect, while one never reached says why */
	const char *shown = state == NETWORK_DISCONNECTED && bouncer->has_connected ? TEXT_RECONNECTING
		: describe_state(state);
	g_autofree char *description = state == NETWORK_DISCONNECTED && error && !bouncer->has_connected
		? g_strdup_printf("%s · %s · %s", TEXT_BOUNCER, TEXT_DISCONNECTED, error)
		: g_strdup_printf("%s · %s", TEXT_BOUNCER, shown);
	adw_status_page_set_description(page, description);
}


/* The connection behind the chosen server that is down: its bouncer when it goes through one, or itself */
static const char *find_lost_connection(Window *window, gboolean *was_connected) {
	CrimsonNetwork *network = window->network;
	if (!network)
		return NULL;
	Session *session = network->session;
	if (session->record->is_bouncer && read_account_state(session) != NETWORK_CONNECTED) {
		*was_connected = session->has_connected;
		return describe_account_name(session);
	}
	if (network->state != NETWORK_CONNECTED) {
		*was_connected = session->has_connected;
		return network->name;
	}
	return NULL;
}


static gboolean reveal_banner(gpointer data) {
	Window *window = data;
	window->banner_source = 0;
	adw_banner_set_revealed(window->banner, TRUE);
	return G_SOURCE_REMOVE;
}


/* A lost connection shows until it is back, and coming back says so once */
static void refresh_banner(Window *window) {
	gboolean was_connected = FALSE;
	const char *lost = window->chat->has_started ? find_lost_connection(window, &was_connected) : NULL;
	if (!lost) {
		g_clear_handle_id(&window->banner_source, g_source_remove);
		if (adw_banner_get_revealed(window->banner) && window->lost_name) {
			g_autofree char *text = g_strdup_printf(TEXT_RECONNECTED_TO, window->lost_name);
			show_toast(window, text);
		}
		adw_banner_set_revealed(window->banner, FALSE);
		g_clear_pointer(&window->lost_name, g_free);
		return;
	}
	g_autofree char *title = g_strdup_printf(was_connected ? TEXT_RECONNECTING_TO : TEXT_CONNECTING_TO_ELLIPSIS,
		lost);
	adw_banner_set_title(window->banner, title);
	g_free(window->lost_name);
	window->lost_name = was_connected ? g_strdup(lost) : NULL;
	if (!adw_banner_get_revealed(window->banner) && !window->banner_source)
		window->banner_source = g_timeout_add(BANNER_DELAY_MS, reveal_banner, window);
}


static void retry_now(AdwBanner *banner, Window *window) {
	Session *bouncer = find_bouncer(window->chat);
	if (bouncer && read_account_state(bouncer) != NETWORK_CONNECTED)
		connect_session(bouncer);
	else if (window->network)
		reconnect_network(window->network);
}


static gboolean is_listed_network(Window *window, CrimsonNetwork *network) {
	return network && g_list_store_find(window->chat->networks, network, NULL);
}


/* Shows the welcome or the chats, the rail only with several servers, and keeps a server chosen */
static void update_view(Window *window) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	gboolean has_chats = count > 0 || !window->chat->has_started;
	gtk_stack_set_visible_child_name(GTK_STACK(window->pages), has_chats ? "chats" : "welcome");
	refresh_welcome(window);
	gboolean has_rail = !window->is_narrow && count > 1;
	window->has_rail = has_rail;
	gtk_widget_set_visible(window->rail, has_rail);
	set_room_list_narrow(window->room_list, !has_rail);
	if (!is_listed_network(window, window->network)) {
		g_autoptr(CrimsonNetwork) first = count ? g_list_model_get_item(G_LIST_MODEL(window->chat->networks), 0) : NULL;
		choose_network(window, first);
	}
	refresh_room_list_header(window->room_list);
	refresh_banner(window);
}


static gboolean refresh_now(gpointer data) {
	Window *window = data;
	window->refresh_source = 0;
	refresh_rail(window);
	update_view(window);
	resort_room_list(window->room_list);
	refresh_conversation(window->conversation);
	return G_SOURCE_REMOVE;
}


void refresh_window_later(Window *window) {
	if (!window->refresh_source)
		window->refresh_source = g_idle_add(refresh_now, window);
}


static void select_rail_row(Window *window, CrimsonNetwork *network) {
	for (GtkWidget *row = gtk_widget_get_first_child(window->rail_list); row; row = gtk_widget_get_next_sibling(row)) {
		if (network && g_object_get_data(G_OBJECT(row), NETWORK_KEY) == network) {
			gtk_list_box_select_row(GTK_LIST_BOX(window->rail_list), GTK_LIST_BOX_ROW(row));
			return;
		}
	}
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->rail_list));
}


void choose_network(Window *window, CrimsonNetwork *network) {
	window->network = network;
	select_rail_row(window, network);
	show_room_list_network(window->room_list, network);
	save_window_choice(network ? network->key : NULL, NULL);
}


static void pick_rail_row(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	CrimsonNetwork *network = row ? g_object_get_data(G_OBJECT(row), NETWORK_KEY) : NULL;
	if (network && network != window->network)
		choose_network(window, network);
}


static void follow_networks(GListModel *networks, guint position, guint removed, guint added, Window *window) {
	update_view(window);
}


/* A bouncer's name and state show on the welcome, so each account change redraws it */
static void follow_accounts(GListModel *accounts, guint position, guint removed, guint added, Window *window) {
	for (guint index = position; index < position + added; index++) {
		g_autoptr(CrimsonAccount) account = g_list_model_get_item(accounts, index);
		g_signal_connect_swapped(account, "changed", G_CALLBACK(refresh_window_later), window);
	}
	refresh_window_later(window);
}


CrimsonRoom *get_open_room(Window *window) {
	return get_conversation_room(window->conversation);
}


gboolean is_room_in_view(Window *window, CrimsonRoom *room) {
	return room && get_open_room(window) == room && gtk_widget_get_visible(GTK_WIDGET(window->window))
		&& gtk_window_is_active(GTK_WINDOW(window->window))
		&& (!adw_navigation_split_view_get_collapsed(window->split)
		|| adw_navigation_split_view_get_show_content(window->split));
}


void open_room(Window *window, CrimsonRoom *room) {
	if (!room)
		return;
	if (room->network != window->network)
		choose_network(window, room->network);
	select_listed_room(window->room_list, room);
	show_conversation_room(window->conversation, room);
	adw_navigation_split_view_set_show_content(window->split, TRUE);
	save_window_choice(room->network->key, room);
	focus_composer(window->conversation);
}


/* The app's own items, with the bouncer under its name and its admin items for an admin */
static GMenuModel *create_main_menu(Window *window) {
	GMenu *menu = g_menu_new();
	Session *bouncer = find_bouncer(window->chat);
	if (bouncer) {
		GMenu *items = g_menu_new();
		if (bouncer->is_admin) {
			g_menu_append(items, TEXT_INVITE_PEOPLE, "win.invite-people");
			g_menu_append(items, TEXT_ADMIN_PAGE, "win.bouncer-admin");
		}
		g_menu_append(items, TEXT_SETTINGS, "win.bouncer-settings");
		g_menu_append(items, TEXT_DISCONNECT, "win.disconnect-bouncer");
		GMenu *section = g_menu_new();
		g_menu_append_submenu(section, describe_account_name(bouncer), G_MENU_MODEL(items));
		g_menu_append_section(menu, TEXT_BOUNCER, G_MENU_MODEL(section));
		g_object_unref(items);
		g_object_unref(section);
	}
	/* Each server opens its own menu, the same way the bouncer does */
	GMenu *servers = g_menu_new();
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		g_autoptr(GMenuModel) items = create_server_menu(window, network);
		g_menu_append_submenu(servers, network->name, items);
	}
	g_menu_append(servers, TEXT_ADD_SERVER, "app.add-server");
	g_menu_append_section(menu, TEXT_SERVERS, G_MENU_MODEL(servers));
	g_object_unref(servers);
	GMenu *adding = g_menu_new();
	if (!bouncer) {
		g_menu_append(adding, TEXT_CONNECT_BOUNCER, "app.add-bouncer");
		g_menu_append_section(menu, NULL, G_MENU_MODEL(adding));
	}
	g_object_unref(adding);
	GMenu *program = g_menu_new();
	g_menu_append(program, TEXT_SETTINGS, "app.settings");
	g_menu_append(program, TEXT_ABOUT, "app.about");
	g_menu_append(program, TEXT_QUIT, "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(program));
	g_object_unref(program);
	return G_MENU_MODEL(menu);
}


static void fill_main_menu(GtkMenuButton *button, gpointer data) {
	g_autoptr(GMenuModel) model = create_main_menu(data);
	gtk_menu_button_set_menu_model(button, model);
}


GtkWidget *create_main_menu_button(Window *window) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
	gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(button), fill_main_menu, window, NULL);
	gtk_widget_set_tooltip_text(button, TEXT_MENU);
	return button;
}


static GtkWidget *create_rail(Window *window) {
	GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(rail, "rail");
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *menu = create_main_menu_button(window);
	gtk_widget_add_css_class(menu, "flat");
	gtk_widget_add_css_class(menu, "circular");
	gtk_widget_add_css_class(menu, "rail-button");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), menu);
	gtk_widget_add_css_class(header, "top-bar");
	gtk_box_append(GTK_BOX(rail), header);
	window->rail_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->rail_list, "rail-list");
	gtk_list_box_bind_model(GTK_LIST_BOX(window->rail_list), G_LIST_MODEL(window->chat->networks), create_rail_row,
		window, NULL);
	g_signal_connect(window->rail_list, "row-selected", G_CALLBACK(pick_rail_row), window);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), window->rail_list);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(rail), scroller);
	return rail;
}


static GtkWidget *create_welcome_button(const char *label, const char *action) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	gtk_widget_add_css_class(button, "pill");
	return button;
}


static GtkWidget *create_welcome(Window *window) {
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_main_menu_button(window));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	window->welcome = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(window->welcome), APPLICATION_ICON);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
	window->welcome_bouncer = create_welcome_button(TEXT_CONNECT_BOUNCER, "app.add-bouncer");
	gtk_widget_add_css_class(window->welcome_bouncer, "suggested-action");
	window->welcome_server = create_welcome_button(TEXT_ADD_SERVER, "app.add-server");
	gtk_box_append(GTK_BOX(buttons), window->welcome_bouncer);
	gtk_box_append(GTK_BOX(buttons), window->welcome_server);
	adw_status_page_set_child(ADW_STATUS_PAGE(window->welcome), buttons);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->welcome);
	return toolbar;
}


static void follow_collapse(AdwNavigationSplitView *split, GParamSpec *spec, Window *window) {
	window->is_narrow = adw_navigation_split_view_get_collapsed(split);
	set_conversation_narrow(window->conversation, window->is_narrow);
	update_view(window);
}


void hide_window(Window *window) {
	if (window->tray && is_tray_shown(window->tray))
		gtk_widget_set_visible(GTK_WIDGET(window->window), FALSE);
	else
		gtk_window_minimize(GTK_WINDOW(window->window));
}


/* Closing keeps the chats connected in the tray, and only quitting really closes the window */
static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	read_window_geometry(gtk_window, &window->geometry);
	save_window_geometry_state(&window->geometry);
	gboolean has_tray = window->tray && is_tray_shown(window->tray);
	if (window->is_quitting || !get_settings()->closes_to_tray || !has_tray)
		return FALSE;
	gtk_widget_set_visible(GTK_WIDGET(gtk_window), FALSE);
	return TRUE;
}


static void join_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->network)
		show_room_picker(window, window->network);
	else
		show_toast(window, TEXT_ADD_SERVER_FIRST);
}


static void message_someone_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->network)
		show_message_dialog(window, window->network);
	else
		show_toast(window, TEXT_ADD_SERVER_FIRST);
}


static void bouncer_settings_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *bouncer = find_bouncer(((Window *)data)->chat);
	if (bouncer)
		show_bouncer_dialog(data, bouncer);
}


static void disconnect_bouncer_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *bouncer = find_bouncer(((Window *)data)->chat);
	if (bouncer)
		confirm_bouncer_disconnect(data, bouncer);
}


static void invite_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *bouncer = find_bouncer(((Window *)data)->chat);
	if (bouncer)
		show_invite_dialog(data, bouncer);
}


/* The bouncer's website signs admins in to manage invites */
static void bouncer_admin_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	Session *bouncer = find_bouncer(window->chat);
	if (!bouncer || !bouncer->web_port)
		return;
	g_autofree char *link = g_strdup_printf("https://%s:%s/", bouncer->record->host, bouncer->web_port);
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(link);
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(window->window), NULL, NULL, NULL);
}


static CrimsonNetwork *find_keyed_network(Window *window, GVariant *parameter) {
	const char *key = g_variant_get_string(parameter, NULL);
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_strcmp0(network->key, key) == 0)
			return network;
	}
	return NULL;
}


static void choose_server_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		choose_network(data, network);
}


static void own_profile_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		show_own_profile(data, network);
}


/* The server room holds what the network itself says, kept out of the chat list */
static void server_messages_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		open_room(data, network->server_room);
}


static void network_settings_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		show_network_settings(data, network);
}


static void reconnect_server_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		reconnect_network(network);
}


static void remove_network_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		confirm_network_removal(data, network);
}


static void leave_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonRoom *room = get_open_room(window);
	if (!room || room->kind == ROOM_SERVER)
		return;
	show_conversation_room(window->conversation, NULL);
	adw_navigation_split_view_set_show_content(window->split, FALSE);
	leave_room(room);
}


static void leave_chat_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *key = NULL, *name = NULL;
	g_variant_get(parameter, "(&s&s)", &key, &name);
	g_autoptr(GVariant) network_key = g_variant_ref_sink(g_variant_new_string(key));
	CrimsonNetwork *network = find_keyed_network(window, network_key);
	CrimsonRoom *room = network ? find_room(network, name) : NULL;
	if (!room)
		return;
	if (get_open_room(window) == room) {
		show_conversation_room(window->conversation, NULL);
		adw_navigation_split_view_set_show_content(window->split, FALSE);
	}
	leave_room(room);
}


static void search_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	adw_navigation_split_view_set_show_content(window->split, FALSE);
	focus_room_search(window->room_list);
}


static void step_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonRoom *room = step_listed_room(window->room_list, g_variant_get_int32(parameter));
	if (room)
		open_room(window, room);
}


static void hide_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	hide_window(data);
}


static void add_window_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"join-room", join_room_action, NULL, NULL, NULL, {0}},
		{"message-someone", message_someone_action, NULL, NULL, NULL, {0}},
		{"bouncer-settings", bouncer_settings_action, NULL, NULL, NULL, {0}},
		{"disconnect-bouncer", disconnect_bouncer_action, NULL, NULL, NULL, {0}},
		{"bouncer-admin", bouncer_admin_action, NULL, NULL, NULL, {0}},
		{"choose-server", choose_server_action, "s", NULL, NULL, {0}},
		{"own-profile", own_profile_action, "s", NULL, NULL, {0}},
		{"server-messages", server_messages_action, "s", NULL, NULL, {0}},
		{"network-settings", network_settings_action, "s", NULL, NULL, {0}},
		{"reconnect-server", reconnect_server_action, "s", NULL, NULL, {0}},
		{"remove-network", remove_network_action, "s", NULL, NULL, {0}},
		{"leave-room", leave_room_action, NULL, NULL, NULL, {0}},
		{"leave-chat", leave_chat_action, "(ss)", NULL, NULL, {0}},
		{"search", search_action, NULL, NULL, NULL, {0}},
		{"step-room", step_room_action, "i", NULL, NULL, {0}},
		{"hide", hide_action, NULL, NULL, NULL, {0}},
		{"invite-people", invite_action, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(window->window), ENTRIES, G_N_ELEMENTS(ENTRIES), window);
}


static GtkWidget *create_sidebar(Window *window) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	window->rail = create_rail(window);
	gtk_box_append(GTK_BOX(box), window->rail);
	GtkWidget *list = get_room_list_widget(window->room_list);
	gtk_widget_set_hexpand(list, TRUE);
	gtk_box_append(GTK_BOX(box), list);
	return box;
}


static void add_breakpoint(Window *window) {
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_CONDITION));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(window->split), "collapsed", &collapsed);
	adw_application_window_add_breakpoint(window->window, breakpoint);
}


void restore_window_choice(Window *window) {
	update_view(window);
	g_autofree char *network_key = NULL, *room_network = NULL, *room_name = NULL;
	load_window_choice(&network_key, &room_network, &room_name);
	CrimsonRoom *room = NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_strcmp0(network->key, network_key) == 0)
			choose_network(window, network);
		if (g_strcmp0(network->key, room_network) == 0 && room_name)
			room = find_room(network, room_name);
	}
	if (room)
		open_room(window, room);
}


Window *create_window(GtkApplication *application, Chat *chat) {
	Window *window = g_new0(Window, 1);
	main_window = window;
	window->chat = chat;
	window->window = ADW_APPLICATION_WINDOW(adw_application_window_new(application));
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	g_object_set_data(G_OBJECT(window->window), "window", window);
	load_window_geometry_state(&window->geometry);
	apply_window_geometry(GTK_WINDOW(window->window), &window->geometry);
	gtk_widget_set_size_request(GTK_WIDGET(window->window), 360, 480);
	window->banner = ADW_BANNER(adw_banner_new(""));
	adw_banner_set_button_label(window->banner, TEXT_RETRY);
	g_signal_connect(window->banner, "button-clicked", G_CALLBACK(retry_now), window);
	window->room_list = create_room_list(window);
	/* The banner sits in the chat list under its search field, next to the server it is about */
	GtkWidget *list = get_room_list_widget(window->room_list);
	gtk_box_insert_child_after(GTK_BOX(list), GTK_WIDGET(window->banner), gtk_widget_get_first_child(list));
	window->conversation = create_conversation(window);
	window->split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
	adw_navigation_split_view_set_sidebar_width_fraction(window->split, SIDEBAR_FRACTION);
	adw_navigation_split_view_set_min_sidebar_width(window->split, SIDEBAR_MINIMUM);
	adw_navigation_split_view_set_max_sidebar_width(window->split, SIDEBAR_MAXIMUM);
	AdwNavigationPage *sidebar = adw_navigation_page_new(create_sidebar(window), TEXT_CHATS);
	AdwNavigationPage *content = adw_navigation_page_new(get_conversation_widget(window->conversation),
		TEXT_CONVERSATION);
	adw_navigation_split_view_set_sidebar(window->split, sidebar);
	adw_navigation_split_view_set_content(window->split, content);
	g_signal_connect(window->split, "notify::collapsed", G_CALLBACK(follow_collapse), window);
	window->pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(window->pages), GTK_WIDGET(window->split), "chats");
	gtk_stack_add_named(GTK_STACK(window->pages), create_welcome(window), "welcome");
	g_signal_connect(chat->networks, "items-changed", G_CALLBACK(follow_networks), window);
	g_signal_connect(chat->accounts, "items-changed", G_CALLBACK(follow_accounts), window);
	window->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
	adw_toast_overlay_set_child(window->toasts, window->pages);
	adw_application_window_set_content(window->window, GTK_WIDGET(window->toasts));
	add_breakpoint(window);
	add_window_actions(window);
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	static const TrayEntry TRAY_ENTRIES[] = {
		{TEXT_SHOW_WINDOW, "app.show"},
		{NULL, NULL},
		{TEXT_QUIT_APPLICATION, "app.quit"},
	};
	TraySetup tray = {TRAY_ENTRIES, G_N_ELEMENTS(TRAY_ENTRIES), NULL, NULL};
	window->tray = create_tray(G_APPLICATION(application), GTK_WINDOW(window->window), &tray);
	update_view(window);
	note_first_frame(GTK_WIDGET(window->window));
	return window;
}
