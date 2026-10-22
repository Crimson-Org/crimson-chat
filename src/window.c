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
#define ACCOUNT_KEY "account"

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


/* Only what belongs to the account: chats and the app itself live in the main menu */
GMenuModel *create_account_menu(Session *account) {
	gboolean is_bouncer = account->record->is_bouncer;
	GMenu *menu = g_menu_new();
	if (is_bouncer) {
		GMenu *adding = g_menu_new();
		g_menu_append(adding, TEXT_ADD_NETWORK, "win.add-network");
		g_menu_append(adding, TEXT_INVITE_PEOPLE, "win.invite-people");
		g_menu_append_section(menu, NULL, G_MENU_MODEL(adding));
		g_object_unref(adding);
		/* Each network the bouncer keeps opens its own small menu */
		GMenu *networks = g_menu_new();
		for (guint index = 0; index < account->networks->len; index++) {
			CrimsonNetwork *network = g_ptr_array_index(account->networks, index);
			GMenu *items = g_menu_new();
			append_network_item(items, TEXT_SERVER_MESSAGES, "win.server-messages", network);
			append_network_item(items, TEXT_SETTINGS, "win.network-settings", network);
			append_network_item(items, TEXT_REMOVE, "win.remove-network", network);
			g_menu_append_submenu(networks, network->name, G_MENU_MODEL(items));
			g_object_unref(items);
		}
		g_menu_append_section(menu, NULL, G_MENU_MODEL(networks));
		g_object_unref(networks);
	}
	GMenu *own = g_menu_new();
	if (!is_bouncer && account->networks->len)
		append_network_item(own, TEXT_SERVER_MESSAGES, "win.server-messages",
			g_ptr_array_index(account->networks, 0));
	g_menu_append(own, is_bouncer ? TEXT_BOUNCER_SETTINGS : TEXT_SERVER_SETTINGS, "win.account-settings");
	g_menu_append(own, TEXT_RECONNECT, "win.reconnect");
	g_menu_append(own, is_bouncer ? TEXT_REMOVE_BOUNCER : TEXT_REMOVE_SERVER, "win.remove-account");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(own));
	g_object_unref(own);
	return G_MENU_MODEL(menu);
}


static void forget_rail_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking a server or bouncer picks it and opens everything about it */
static void open_rail_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	Session *account = g_object_get_data(G_OBJECT(row), ACCOUNT_KEY);
	choose_account(window, account);
	g_autoptr(GMenuModel) model = create_account_menu(account);
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


/* A direct account shows its network's icon when the server has one */
static const char *find_account_icon(Session *session) {
	if (session->record->is_bouncer || !session->networks->len)
		return NULL;
	return ((CrimsonNetwork *)g_ptr_array_index(session->networks, 0))->icon_url;
}


static void fill_rail_row(GtkWidget *row, Session *session) {
	AdwAvatar *avatar = g_object_get_data(G_OBJECT(row), "avatar");
	GtkWidget *badge = g_object_get_data(G_OBJECT(row), "badge");
	GtkWidget *kind = g_object_get_data(G_OBJECT(row), "kind");
	const char *name = describe_account_name(session);
	adw_avatar_set_text(avatar, name);
	g_autofree char *icon = fill_image_size(find_account_icon(session), RAIL_AVATAR_SIZE * 2);
	show_avatar_image(avatar, icon);
	gtk_widget_set_visible(kind, session->record->is_bouncer);
	gboolean has_mention = FALSE;
	guint unread = count_account_unread(session, &has_mention);
	style_unread_badge(badge, unread, has_mention);
	NetworkState state = read_account_state(session);
	gtk_widget_set_opacity(GTK_WIDGET(avatar), state == NETWORK_CONNECTED ? 1.0 : 0.45);
	g_autofree char *tooltip = g_strdup_printf("%s\n%s", name, describe_state(state));
	gtk_widget_set_tooltip_text(row, tooltip);
}


static void refresh_rail_row(CrimsonAccount *account, gpointer row) {
	fill_rail_row(row, account->session);
}


static GtkWidget *create_rail_row(gpointer item, gpointer data) {
	CrimsonAccount *account = item;
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
	/* A bouncer carries a small server mark, so it reads differently from a single server */
	GtkWidget *kind = gtk_image_new_from_icon_name("network-server-symbolic");
	gtk_widget_add_css_class(kind, "rail-kind");
	gtk_widget_set_halign(kind, GTK_ALIGN_START);
	gtk_widget_set_valign(kind, GTK_ALIGN_END);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), kind);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), overlay);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "badge", badge);
	g_object_set_data(G_OBJECT(row), "kind", kind);
	g_object_set_data(G_OBJECT(row), ACCOUNT_KEY, account->session);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_rail_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	fill_rail_row(row, account->session);
	g_signal_connect_object(account, "changed", G_CALLBACK(refresh_rail_row), row, 0);
	/* The chat list header and its connection line follow the account too, even with the rail hidden */
	g_signal_connect_swapped(account, "changed", G_CALLBACK(refresh_window_later), data);
	return row;
}


static void refresh_rail(Window *window) {
	for (GtkWidget *row = gtk_widget_get_first_child(window->rail_list); row; row = gtk_widget_get_next_sibling(row)) {
		Session *session = g_object_get_data(G_OBJECT(row), ACCOUNT_KEY);
		if (session)
			fill_rail_row(row, session);
	}
	if (!window->account)
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


static void select_rail_row(Window *window, Session *account) {
	for (GtkWidget *row = gtk_widget_get_first_child(window->rail_list); row; row = gtk_widget_get_next_sibling(row)) {
		if (account && g_object_get_data(G_OBJECT(row), ACCOUNT_KEY) == account) {
			gtk_list_box_select_row(GTK_LIST_BOX(window->rail_list), GTK_LIST_BOX_ROW(row));
			return;
		}
	}
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->rail_list));
}


void choose_account(Window *window, Session *account) {
	if (!window->has_rail)
		account = NULL;
	window->account = account;
	select_rail_row(window, account);
	show_room_list_account(window->room_list, account);
	refresh_rail(window);
	save_window_choice(account ? account->record->id : NULL, NULL);
}


static void pick_rail_row(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	Session *account = row ? g_object_get_data(G_OBJECT(row), ACCOUNT_KEY) : NULL;
	if (account && account != window->account)
		choose_account(window, account);
}


Session *get_menu_account(Window *window) {
	if (window->account)
		return window->account;
	return window->chat->sessions->len == 1 ? g_ptr_array_index(window->chat->sessions, 0) : NULL;
}


/* With one server or bouncer there is nothing to choose between, so the chat list stands alone */
static void update_rail(Window *window) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->accounts));
	gboolean has_rail = !window->is_narrow && count > 1;
	if (has_rail == window->has_rail && gtk_widget_get_visible(window->rail) == has_rail) {
		refresh_room_list_header(window->room_list);
		return;
	}
	window->has_rail = has_rail;
	gtk_widget_set_visible(window->rail, has_rail);
	set_room_list_narrow(window->room_list, !has_rail);
	if (!has_rail)
		choose_account(window, NULL);
	refresh_room_list_header(window->room_list);
}


static void follow_accounts(GListModel *accounts, guint position, guint removed, guint added, Window *window) {
	update_rail(window);
}


static void pick_all_chats(GtkButton *button, Window *window) {
	choose_account(window, NULL);
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
	if (window->account && room->network->session != window->account)
		choose_account(window, room->network->session);
	select_listed_room(window->room_list, room);
	show_conversation_room(window->conversation, room);
	adw_navigation_split_view_set_show_content(window->split, TRUE);
	save_window_choice(window->account ? window->account->record->id : NULL, room);
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
	gtk_list_box_bind_model(GTK_LIST_BOX(window->rail_list), G_LIST_MODEL(window->chat->accounts), create_rail_row,
		window, NULL);
	g_signal_connect(window->rail_list, "row-selected", G_CALLBACK(pick_rail_row), window);
	g_signal_connect(window->chat->accounts, "items-changed", G_CALLBACK(follow_accounts), window);
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
	set_conversation_narrow(window->conversation, window->is_narrow);
	update_rail(window);
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
	Session *account = get_menu_account(window);
	if (account && account->networks->len)
		return g_ptr_array_index(account->networks, 0);
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


static void account_settings_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	Session *account = get_menu_account(window);
	if (account && account->record->is_bouncer)
		show_bouncer_dialog(window, account);
	else if (account && account->networks->len)
		show_network_settings(window, g_ptr_array_index(account->networks, 0));
}


static void reconnect_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *account = get_menu_account(data);
	if (account)
		connect_session(account);
}


static void remove_account_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *account = get_menu_account(data);
	if (account)
		confirm_account_removal(data, account);
}


static void add_network_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Session *account = get_menu_account(data);
	if (account && account->record->is_bouncer)
		show_server_guide(data, account);
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


static void search_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	choose_account(window, NULL);
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
	Session *account = get_menu_account(window);
	if (account)
		show_invite_dialog(window, account);
}


static void hide_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	hide_window(data);
}


static void add_window_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"join-room", join_room_action, NULL, NULL, NULL, {0}},
		{"message-someone", message_someone_action, NULL, NULL, NULL, {0}},
		{"account-settings", account_settings_action, NULL, NULL, NULL, {0}},
		{"reconnect", reconnect_action, NULL, NULL, NULL, {0}},
		{"remove-account", remove_account_action, NULL, NULL, NULL, {0}},
		{"add-network", add_network_action, NULL, NULL, NULL, {0}},
		{"server-messages", server_messages_action, "s", NULL, NULL, {0}},
		{"network-settings", network_settings_action, "s", NULL, NULL, {0}},
		{"remove-network", remove_network_action, "s", NULL, NULL, {0}},
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
	g_autofree char *account_id = NULL, *room_network = NULL, *room_name = NULL;
	load_window_choice(&account_id, &room_network, &room_name);
	for (guint index = 0; index < window->chat->sessions->len; index++) {
		Session *session = g_ptr_array_index(window->chat->sessions, index);
		if (g_strcmp0(session->record->id, account_id) == 0)
			choose_account(window, session);
	}
	CrimsonRoom *room = NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count && room_name; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_strcmp0(network->key, room_network) == 0)
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
	choose_account(window, NULL);
	note_first_frame(GTK_WIDGET(window->window));
	return window;
}
