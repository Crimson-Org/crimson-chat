#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "room-look.h"
#include "settings.h"
#include "window.h"

#define ROOM_AVATAR_SIZE 50
#define ROOM_KEY "room"
#define HANDLER_KEY "changed-handler"
#define WATCHED_KEY "watched"

struct RoomList {
	Window *window;
	GtkWidget *box, *header, *title_stack, *search, *title, *subtitle, *menu_button, *account_button, *status;
	GtkWidget *stack, *list_view;
	GtkCustomFilter *filter;
	GtkCustomSorter *sorter;
	GtkSingleSelection *selection;
	GtkFlattenListModel *rooms;
	Session *account;
	gboolean is_choosing;
};


static gboolean keep_room(gpointer item, gpointer data) {
	CrimsonRoom *room = item;
	RoomList *list = data;
	/* Server rooms stay out of the list, and open from the account menu */
	if (room->kind == ROOM_SERVER || (list->account && room->network->session != list->account))
		return FALSE;
	const char *search = gtk_editable_get_text(GTK_EDITABLE(list->search));
	if (!search || !*search)
		return TRUE;
	g_autofree char *name = g_utf8_casefold(room->name, -1);
	g_autofree char *wanted = g_utf8_casefold(search, -1);
	return strstr(name, wanted) != NULL;
}


/* The server room stays on top of a network's rooms, and the rest follow their latest message */
static int order_rooms(gconstpointer first, gconstpointer second, gpointer data) {
	const CrimsonRoom *one = first, *other = second;
	if ((one->kind == ROOM_SERVER) != (other->kind == ROOM_SERVER))
		return one->kind == ROOM_SERVER ? -1 : 1;
	if (one->last_time != other->last_time)
		return one->last_time > other->last_time ? -1 : 1;
	return g_utf8_collate(one->name, other->name);
}


/* Rooms from several networks side by side carry their network's name, since a channel name alone is ambiguous */
static gboolean mixes_networks(RoomList *list) {
	if (list->account)
		return list->account->networks->len > 1;
	return g_list_model_get_n_items(G_LIST_MODEL(list->window->chat->networks)) > 1;
}


static void fill_row(GtkWidget *row, CrimsonRoom *room) {
	AdwAvatar *avatar = g_object_get_data(G_OBJECT(row), "avatar");
	GtkWidget *name = g_object_get_data(G_OBJECT(row), "name");
	GtkWidget *time = g_object_get_data(G_OBJECT(row), "time");
	GtkWidget *preview = g_object_get_data(G_OBJECT(row), "preview");
	GtkWidget *badge = g_object_get_data(G_OBJECT(row), "badge");
	GtkWidget *kind = g_object_get_data(G_OBJECT(row), "kind");
	fill_room_avatar(avatar, room, ROOM_AVATAR_SIZE);
	g_autofree char *title = describe_room_title(room);
	gtk_label_set_text(GTK_LABEL(name), title);
	gtk_image_set_from_icon_name(GTK_IMAGE(kind), "network-server-symbolic");
	gtk_widget_set_visible(kind, room->kind == ROOM_SERVER);
	GtkWidget *network = g_object_get_data(G_OBJECT(row), "network");
	RoomList *list = g_object_get_data(G_OBJECT(row), "list");
	gtk_label_set_text(GTK_LABEL(network), room->network->name);
	gtk_widget_set_visible(network, room->kind != ROOM_SERVER && mixes_networks(list));
	g_autofree char *moment = format_list_time(room->last_time);
	gtk_label_set_text(GTK_LABEL(time), moment);
	g_autofree char *line = describe_room_preview(room);
	gtk_label_set_text(GTK_LABEL(preview), line);
	if (is_room_typing(room) && get_settings()->shows_typing)
		gtk_widget_add_css_class(preview, "typing");
	else
		gtk_widget_remove_css_class(preview, "typing");
	style_unread_badge(badge, room->unread_count, room->has_mention);
	gtk_widget_set_opacity(row, room->kind == ROOM_CHANNEL && !room->is_joined ? 0.6 : 1.0);
}


static void refresh_row(CrimsonRoom *room, gpointer row) {
	fill_row(row, room);
}


static GtkWidget *create_label(const char *css_class, gboolean expands) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
	gtk_widget_set_hexpand(label, expands);
	if (css_class)
		gtk_widget_add_css_class(label, css_class);
	return label;
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(row, "room-row");
	GtkWidget *avatar = adw_avatar_new(ROOM_AVATAR_SIZE, NULL, TRUE);
	gtk_box_append(GTK_BOX(row), avatar);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(text, TRUE);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *kind = gtk_image_new();
	gtk_widget_add_css_class(kind, "room-kind");
	GtkWidget *name = create_label("room-name", FALSE);
	GtkWidget *network = create_label("room-network", TRUE);
	GtkWidget *time = create_label("room-time", FALSE);
	gtk_box_append(GTK_BOX(top), kind);
	gtk_box_append(GTK_BOX(top), name);
	gtk_box_append(GTK_BOX(top), network);
	gtk_box_append(GTK_BOX(top), time);
	GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *preview = create_label("room-preview", TRUE);
	GtkWidget *badge = gtk_label_new(NULL);
	gtk_widget_add_css_class(badge, "unread-badge");
	gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(bottom), preview);
	gtk_box_append(GTK_BOX(bottom), badge);
	gtk_box_append(GTK_BOX(text), top);
	gtk_box_append(GTK_BOX(text), bottom);
	gtk_box_append(GTK_BOX(row), text);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "name", name);
	g_object_set_data(G_OBJECT(row), "time", time);
	g_object_set_data(G_OBJECT(row), "preview", preview);
	g_object_set_data(G_OBJECT(row), "badge", badge);
	g_object_set_data(G_OBJECT(row), "kind", kind);
	g_object_set_data(G_OBJECT(row), "network", network);
	g_object_set_data(G_OBJECT(row), "list", data);
	gtk_list_item_set_child(item, row);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	CrimsonRoom *room = gtk_list_item_get_item(item);
	GtkWidget *row = gtk_list_item_get_child(item);
	fill_row(row, room);
	gulong handler = g_signal_connect(room, "changed", G_CALLBACK(refresh_row), row);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, GSIZE_TO_POINTER(handler));
	g_object_set_data(G_OBJECT(row), ROOM_KEY, room);
}


static void unbind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *row = gtk_list_item_get_child(item);
	CrimsonRoom *room = g_object_get_data(G_OBJECT(row), ROOM_KEY);
	gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), HANDLER_KEY));
	if (room && handler)
		g_signal_handler_disconnect(room, handler);
	g_object_set_data(G_OBJECT(row), ROOM_KEY, NULL);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, NULL);
}


static void note_room_change(CrimsonRoom *room, gpointer data) {
	RoomList *list = data;
	refresh_window_later(list->window);
}


/* Every room, wherever it is listed, tells the window when its badge or order may have changed */
static void watch_new_rooms(GListModel *rooms, guint position, guint removed, guint added, gpointer data) {
	RoomList *list = data;
	for (guint index = position; index < position + added; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(rooms, index);
		if (g_object_get_data(G_OBJECT(room), WATCHED_KEY))
			continue;
		g_object_set_data(G_OBJECT(room), WATCHED_KEY, GINT_TO_POINTER(TRUE));
		g_signal_connect(room, "changed", G_CALLBACK(note_room_change), list);
	}
	refresh_window_later(list->window);
}


static gpointer list_network_rooms(gpointer item, gpointer data) {
	CrimsonNetwork *network = item;
	GListStore *rooms = g_object_ref(network->rooms);
	g_object_unref(network);
	return rooms;
}


static void pick_room(GtkSingleSelection *selection, GParamSpec *spec, RoomList *list) {
	CrimsonRoom *room = gtk_single_selection_get_selected_item(selection);
	if (room && !list->is_choosing)
		open_room(list->window, room);
}


/* Clicking the room that is already chosen still opens it, which a narrow window needs after going back */
static void activate_room(GtkListView *view, guint position, RoomList *list) {
	g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(list->selection), position);
	if (room)
		open_room(list->window, room);
}


static void change_search(GtkSearchEntry *entry, RoomList *list) {
	gtk_filter_changed(GTK_FILTER(list->filter), GTK_FILTER_CHANGE_DIFFERENT);
	refresh_room_list_header(list);
}


/* The account menu is built as it opens, so it lists the networks the account has right now */
static void create_account_popover(GtkMenuButton *button, gpointer data) {
	RoomList *list = data;
	Session *account = get_menu_account(list->window);
	if (!account)
		return;
	g_autoptr(GMenuModel) model = create_account_menu(account);
	gtk_menu_button_set_menu_model(button, model);
}


static GtkWidget *create_header(RoomList *list) {
	list->header = adw_header_bar_new();
	gtk_widget_add_css_class(list->header, "top-bar");
	list->menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(list->menu_button), "open-menu-symbolic");
	g_autoptr(GMenuModel) main_menu = create_main_menu();
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(list->menu_button), main_menu);
	gtk_widget_set_tooltip_text(list->menu_button, TEXT_MENU);
	adw_header_bar_pack_start(ADW_HEADER_BAR(list->header), list->menu_button);
	list->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(list->search), TEXT_SEARCH);
	gtk_widget_set_hexpand(list->search, TRUE);
	gtk_widget_add_css_class(list->search, "room-search");
	g_signal_connect(list->search, "search-changed", G_CALLBACK(change_search), list);
	GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(titles, GTK_ALIGN_CENTER);
	list->title = create_label("network-title", FALSE);
	list->subtitle = create_label("network-subtitle", FALSE);
	gtk_box_append(GTK_BOX(titles), list->title);
	gtk_box_append(GTK_BOX(titles), list->subtitle);
	list->title_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(list->title_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(list->title_stack), list->search, "search");
	gtk_stack_add_named(GTK_STACK(list->title_stack), titles, "network");
	gtk_widget_set_hexpand(list->title_stack, TRUE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(list->header), list->title_stack);
	list->account_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(list->account_button), "view-more-symbolic");
	gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(list->account_button),
		create_account_popover, list, NULL);
	gtk_widget_set_tooltip_text(list->account_button, TEXT_NETWORK_MENU);
	adw_header_bar_pack_end(ADW_HEADER_BAR(list->header), list->account_button);
	return list->header;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "crimson-chats-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(page), TEXT_NO_CHATS_TITLE);
	adw_status_page_set_description(ADW_STATUS_PAGE(page), NULL);
	gtk_widget_add_css_class(page, "compact");
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
	GtkWidget *server = gtk_button_new_with_label(TEXT_ADD_SERVER);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(server), "app.add-server");
	gtk_widget_add_css_class(server, "pill");
	gtk_widget_add_css_class(server, "suggested-action");
	GtkWidget *bouncer = gtk_button_new_with_label(TEXT_ADD_BOUNCER);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(bouncer), "app.add-bouncer");
	gtk_widget_add_css_class(bouncer, "pill");
	gtk_box_append(GTK_BOX(buttons), server);
	gtk_box_append(GTK_BOX(buttons), bouncer);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), buttons);
	return page;
}


/* A bouncer without networks yet offers to add one, instead of an empty list that looks like nothing happened */
static GtkWidget *create_no_networks_page(void) {
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "network-server-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(page), TEXT_NO_NETWORKS_TITLE);
	gtk_widget_add_css_class(page, "compact");
	GtkWidget *button = gtk_button_new_with_label(TEXT_ADD_NETWORK);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.add-network");
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
	return page;
}


static GtkWidget *create_no_rooms_page(void) {
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "crimson-chats-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(page), TEXT_NO_CHATS_TITLE);
	gtk_widget_add_css_class(page, "compact");
	GtkWidget *button = gtk_button_new_with_label(TEXT_JOIN_ROOM);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.join-room");
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
	return page;
}


static void follow_listed_rooms(GListModel *model, guint position, guint removed, guint added, RoomList *list) {
	refresh_room_list_header(list);
}


static GtkWidget *create_list_view(RoomList *list) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), list);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), list);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), list);
	list->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(list->selection)), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list->list_view), TRUE);
	gtk_widget_add_css_class(list->list_view, "room-list");
	g_signal_connect(list->list_view, "activate", G_CALLBACK(activate_room), list);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list->list_view);
	gtk_widget_set_vexpand(scroller, TRUE);
	return scroller;
}


static void build_models(RoomList *list) {
	GListModel *networks = G_LIST_MODEL(g_object_ref(list->window->chat->networks));
	GtkMapListModel *mapped = gtk_map_list_model_new(networks, list_network_rooms, NULL, NULL);
	list->rooms = gtk_flatten_list_model_new(G_LIST_MODEL(mapped));
	g_signal_connect(list->rooms, "items-changed", G_CALLBACK(watch_new_rooms), list);
	list->filter = gtk_custom_filter_new(keep_room, list, NULL);
	GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(list->rooms)),
		GTK_FILTER(g_object_ref(list->filter)));
	list->sorter = gtk_custom_sorter_new(order_rooms, NULL, NULL);
	GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), GTK_SORTER(g_object_ref(list->sorter)));
	list->selection = gtk_single_selection_new(G_LIST_MODEL(sorted));
	gtk_single_selection_set_autoselect(list->selection, FALSE);
	gtk_single_selection_set_can_unselect(list->selection, TRUE);
	gtk_single_selection_set_selected(list->selection, GTK_INVALID_LIST_POSITION);
	g_signal_connect(list->selection, "notify::selected-item", G_CALLBACK(pick_room), list);
	g_signal_connect(list->selection, "items-changed", G_CALLBACK(follow_listed_rooms), list);
}


RoomList *create_room_list(Window *window) {
	RoomList *list = g_new0(RoomList, 1);
	list->window = window;
	build_models(list);
	list->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(list->box, "room-column");
	gtk_box_append(GTK_BOX(list->box), create_header(list));
	/* Without a rail the header has no room for the state, so a line says when the account is not connected */
	list->status = create_label("room-list-status", FALSE);
	gtk_label_set_xalign(GTK_LABEL(list->status), 0.5f);
	gtk_box_append(GTK_BOX(list->box), list->status);
	list->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(list->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(list->stack), create_list_view(list), "list");
	gtk_stack_add_named(GTK_STACK(list->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(list->stack), create_no_networks_page(), "no-networks");
	gtk_stack_add_named(GTK_STACK(list->stack), create_no_rooms_page(), "no-rooms");
	gtk_box_append(GTK_BOX(list->box), list->stack);
	set_room_list_narrow(list, FALSE);
	return list;
}


GtkWidget *get_room_list_widget(RoomList *list) {
	return list->box;
}


static const char *describe_state(NetworkState state) {
	switch (state) {
	case NETWORK_CONNECTED: return TEXT_CONNECTED;
	case NETWORK_CONNECTING: return TEXT_CONNECTING;
	default: return TEXT_DISCONNECTED;
	}
}


/* A chosen account names its state, and the reason when it could not connect */
static char *describe_account(Session *account) {
	NetworkState state = read_account_state(account);
	const char *error = read_account_error(account);
	if (state == NETWORK_DISCONNECTED && error)
		return g_strdup_printf("%s · %s", TEXT_DISCONNECTED, error);
	return g_strdup(describe_state(state));
}


static const char *choose_page(RoomList *list, Session *account) {
	Chat *chat = list->window->chat;
	if (!chat->has_started)
		return "list";
	if (!chat->sessions->len)
		return "empty";
	if (account && account->record->is_bouncer && !account->networks->len)
		return "no-networks";
	const char *search = gtk_editable_get_text(GTK_EDITABLE(list->search));
	gboolean is_searching = search && *search;
	if (!is_searching && !g_list_model_get_n_items(G_LIST_MODEL(list->selection)))
		return "no-rooms";
	return "list";
}


void refresh_room_list_header(RoomList *list) {
	Session *account = get_menu_account(list->window);
	gtk_stack_set_visible_child_name(GTK_STACK(list->stack), choose_page(list, account));
	gtk_stack_set_visible_child_name(GTK_STACK(list->title_stack), list->account ? "network" : "search");
	gtk_widget_set_visible(list->account_button, account != NULL);
	gboolean is_connected = !account || read_account_state(account) == NETWORK_CONNECTED;
	gtk_widget_set_visible(list->status, !list->account && !is_connected);
	if (!account)
		return;
	g_autofree char *state = describe_account(account);
	gtk_label_set_text(GTK_LABEL(list->status), state);
	gtk_label_set_text(GTK_LABEL(list->title), describe_account_name(account));
	gtk_label_set_text(GTK_LABEL(list->subtitle), state);
}


void show_room_list_account(RoomList *list, Session *account) {
	list->account = account;
	gtk_filter_changed(GTK_FILTER(list->filter), GTK_FILTER_CHANGE_DIFFERENT);
	select_listed_room(list, get_open_room(list->window));
	refresh_room_list_header(list);
}


void select_listed_room(RoomList *list, CrimsonRoom *room) {
	GListModel *model = G_LIST_MODEL(list->selection);
	guint count = g_list_model_get_n_items(model);
	guint found = GTK_INVALID_LIST_POSITION;
	for (guint index = 0; room && index < count; index++) {
		g_autoptr(CrimsonRoom) item = g_list_model_get_item(model, index);
		if (item == room)
			found = index;
	}
	list->is_choosing = TRUE;
	gtk_single_selection_set_selected(list->selection, found);
	list->is_choosing = FALSE;
}


void resort_room_list(RoomList *list) {
	list->is_choosing = TRUE;
	gtk_sorter_changed(GTK_SORTER(list->sorter), GTK_SORTER_CHANGE_DIFFERENT);
	gtk_filter_changed(GTK_FILTER(list->filter), GTK_FILTER_CHANGE_DIFFERENT);
	list->is_choosing = FALSE;
}


void set_room_list_narrow(RoomList *list, gboolean is_narrow) {
	gtk_widget_set_visible(list->menu_button, is_narrow);
}


void focus_room_search(RoomList *list) {
	gtk_widget_grab_focus(list->search);
}


CrimsonRoom *step_listed_room(RoomList *list, int step) {
	GListModel *model = G_LIST_MODEL(list->selection);
	guint count = g_list_model_get_n_items(model);
	if (!count)
		return NULL;
	guint selected = gtk_single_selection_get_selected(list->selection);
	int position = selected == GTK_INVALID_LIST_POSITION ? 0 : (int)selected + step;
	position = CLAMP(position, 0, (int)count - 1);
	CrimsonRoom *room = g_list_model_get_item(model, (guint)position);
	g_object_unref(room);
	return room;
}
