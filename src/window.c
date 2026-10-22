#include "../lang/en.h"
#include "common/startup.h"
#include "dialogs.h"
#include "images.h"
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
	const char *state = network->state == NETWORK_CONNECTED ? TEXT_CONNECTED : network->state == NETWORK_CONNECTING
		? TEXT_CONNECTING : TEXT_DISCONNECTED;
	g_autofree char *tooltip = g_strdup_printf("%s\n%s", network->name, state);
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
	GtkWidget *avatar = adw_avatar_new(RAIL_AVATAR_SIZE, network->name, TRUE);
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
	fill_rail_row(row, network);
	g_signal_connect_object(network, "changed", G_CALLBACK(refresh_rail_row), row, 0);
	return row;
}


static void refresh_rail(Window *window) {
	for (GtkWidget *row = gtk_widget_get_first_child(window->rail_list); row; row = gtk_widget_get_next_sibling(row)) {
		CrimsonNetwork *network = g_object_get_data(G_OBJECT(row), NETWORK_KEY);
		if (network)
			fill_rail_row(row, network);
	}
	gboolean is_all = window->network == NULL;
	if (is_all)
		gtk_widget_add_css_class(window->all_button, "chosen");
	else
		gtk_widget_remove_css_class(window->all_button, "chosen");
}


static gboolean refresh_now(gpointer data) {
	Window *window = data;
	window->refresh_source = 0;
	refresh_rail(window);
	resort_room_list(window->room_list);
	refresh_room_list_header(window->room_list);
	refresh_conversation(window->conversation);
	return G_SOURCE_REMOVE;
}


void refresh_window_later(Window *window) {
	if (!window->refresh_source)
		window->refresh_source = g_idle_add(refresh_now, window);
}


void choose_network(Window *window, CrimsonNetwork *network) {
	if (window->is_narrow)
		network = NULL;
	window->network = network;
	show_room_list_network(window->room_list, network);
	if (!network)
		gtk_list_box_unselect_all(GTK_LIST_BOX(window->rail_list));
	refresh_rail(window);
	save_window_choice(network ? network->key : NULL, NULL);
}


static void pick_rail_row(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	if (!row)
		return;
	choose_network(window, g_object_get_data(G_OBJECT(row), NETWORK_KEY));
}


static void pick_all_chats(GtkButton *button, Window *window) {
	choose_network(window, NULL);
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
	if (window->network && room->network != window->network)
		choose_network(window, room->network);
	select_listed_room(window->room_list, room);
	show_conversation_room(window->conversation, room);
	adw_navigation_split_view_set_show_content(window->split, TRUE);
	save_window_choice(window->network ? window->network->key : NULL, room);
	focus_composer(window->conversation);
}


static GMenuModel *create_add_menu(void) {
	GMenu *menu = g_menu_new();
	g_menu_append(menu, TEXT_JOIN_ROOM, "win.join-room");
	g_menu_append(menu, TEXT_MESSAGE_SOMEONE, "win.message-someone");
	GMenu *accounts = g_menu_new();
	g_menu_append(accounts, TEXT_ADD_SERVER, "app.add-server");
	g_menu_append(accounts, TEXT_ADD_BOUNCER, "app.add-bouncer");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(accounts));
	g_object_unref(accounts);
	return G_MENU_MODEL(menu);
}


GMenuModel *create_main_menu(void) {
	GMenu *menu = g_menu_new();
	GMenu *chats = g_menu_new();
	g_menu_append(chats, TEXT_JOIN_ROOM, "win.join-room");
	g_menu_append(chats, TEXT_MESSAGE_SOMEONE, "win.message-someone");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(chats));
	GMenu *accounts = g_menu_new();
	g_menu_append(accounts, TEXT_ADD_SERVER, "app.add-server");
	g_menu_append(accounts, TEXT_ADD_BOUNCER, "app.add-bouncer");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(accounts));
	GMenu *program = g_menu_new();
	g_menu_append(program, TEXT_SETTINGS, "app.settings");
	g_menu_append(program, TEXT_ABOUT, "app.about");
	g_menu_append(program, TEXT_QUIT, "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(program));
	g_object_unref(chats);
	g_object_unref(accounts);
	g_object_unref(program);
	return G_MENU_MODEL(menu);
}


static GtkWidget *create_rail_menu_button(const char *icon, const char *tooltip, GMenuModel *model) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), icon);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), model);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_add_css_class(button, "rail-button");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	g_object_unref(model);
	return button;
}


static GtkWidget *create_rail(Window *window) {
	GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(rail, "rail");
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), create_rail_menu_button("open-menu-symbolic",
		TEXT_MENU, create_main_menu()));
	gtk_widget_add_css_class(header, "top-bar");
	gtk_box_append(GTK_BOX(rail), header);
	window->all_button = gtk_button_new_from_icon_name("crimson-chats-symbolic");
	gtk_widget_set_tooltip_text(window->all_button, TEXT_ALL_CHATS);
	gtk_widget_add_css_class(window->all_button, "rail-all");
	gtk_widget_set_halign(window->all_button, GTK_ALIGN_CENTER);
	g_signal_connect(window->all_button, "clicked", G_CALLBACK(pick_all_chats), window);
	gtk_box_append(GTK_BOX(rail), window->all_button);
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
	GtkWidget *add = create_rail_menu_button("list-add-symbolic", TEXT_ADD, create_add_menu());
	gtk_widget_set_margin_bottom(add, 12);
	gtk_box_append(GTK_BOX(rail), add);
	return rail;
}


static void follow_collapse(AdwNavigationSplitView *split, GParamSpec *spec, Window *window) {
	window->is_narrow = adw_navigation_split_view_get_collapsed(split);
	gtk_widget_set_visible(window->rail, !window->is_narrow);
	set_room_list_narrow(window->room_list, window->is_narrow);
	set_conversation_narrow(window->conversation, window->is_narrow);
	if (window->is_narrow)
		choose_network(window, NULL);
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


static CrimsonNetwork *get_action_network(Window *window) {
	CrimsonRoom *room = get_open_room(window);
	if (room)
		return room->network;
	if (window->network)
		return window->network;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	CrimsonNetwork *first = count ? g_list_model_get_item(G_LIST_MODEL(window->chat->networks), 0) : NULL;
	if (first)
		g_object_unref(first);
	return first;
}


static void join_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = get_action_network(window);
	if (network)
		show_join_dialog(window, network);
	else
		show_toast(window, TEXT_ADD_SERVER_FIRST);
}


static void message_someone_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = get_action_network(window);
	if (network)
		show_message_dialog(window, network);
	else
		show_toast(window, TEXT_ADD_SERVER_FIRST);
}


static void edit_network_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->network)
		show_network_settings(window, window->network);
}


static void reconnect_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->network)
		reconnect_network(window->network);
}


static void remove_network_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->network)
		confirm_network_removal(window, window->network);
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


static void search_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	choose_network(window, NULL);
	adw_navigation_split_view_set_show_content(window->split, FALSE);
	focus_room_search(window->room_list);
}


static void step_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonRoom *room = step_listed_room(window->room_list, g_variant_get_int32(parameter));
	if (room)
		open_room(window, room);
}


static void invite_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = get_action_network(window);
	if (network)
		show_invite_dialog(window, network);
}


static void hide_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	hide_window(data);
}


static void add_window_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"join-room", join_room_action, NULL, NULL, NULL, {0}},
		{"message-someone", message_someone_action, NULL, NULL, NULL, {0}},
		{"edit-network", edit_network_action, NULL, NULL, NULL, {0}},
		{"reconnect", reconnect_action, NULL, NULL, NULL, {0}},
		{"remove-network", remove_network_action, NULL, NULL, NULL, {0}},
		{"leave-room", leave_room_action, NULL, NULL, NULL, {0}},
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
	g_autofree char *network_key = NULL, *room_network = NULL, *room_name = NULL;
	load_window_choice(&network_key, &room_network, &room_name);
	CrimsonRoom *room = NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_strcmp0(network->key, network_key) == 0 && !window->is_narrow)
			gtk_list_box_select_row(GTK_LIST_BOX(window->rail_list),
				gtk_list_box_get_row_at_index(GTK_LIST_BOX(window->rail_list), (int)index));
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
	window->room_list = create_room_list(window);
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
	window->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
	adw_toast_overlay_set_child(window->toasts, GTK_WIDGET(window->split));
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
	choose_network(window, NULL);
	note_first_frame(GTK_WIDGET(window->window));
	return window;
}
