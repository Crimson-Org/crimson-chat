#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "room-look.h"
#include "settings.h"
#include "window.h"

#define ROOM_AVATAR_SIZE 50
#define SEARCH_SLIDE_MS 200
#define CONNECTION_SPINNER_SIZE 24
#define ROOM_KEY "room"
#define HANDLER_KEY "changed-handler"
#define WATCHED_KEY "watched"

struct RoomList {
	Window *window;
	GtkWidget *box, *header, *search, *menu_button;
	/* The server's name until the search button slides the search field open in its place */
	GtkWidget *title_stack, *title, *search_button;
	GtkWidget *add_button, *add_revealer, *connection;
	/* Whether the press now going on started on the list's empty space */
	gboolean is_space_pressed;
	GtkWidget *stack, *list_view;
	GtkCustomFilter *filter;
	GtkCustomSorter *sorter;
	GtkSingleSelection *selection;
	GtkFlattenListModel *rooms;
	CrimsonNetwork *network;
};


static gboolean keep_room(gpointer item, gpointer data) {
	CrimsonRoom *room = item;
	RoomList *list = data;
	/* Server rooms stay out of the list, and open from the server menu */
	if (room->kind == ROOM_SERVER || room->network != list->network)
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


/* A click opens a room, even the one already chosen, which a narrow window needs after going back; pointing at
 * rooms never switches */
static void click_room(GtkGestureClick *gesture, int presses, double x, double y, gpointer data) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	CrimsonRoom *room = g_object_get_data(G_OBJECT(row), ROOM_KEY);
	RoomList *list = data;
	/* A press dragged off the room before letting go opens nothing */
	if (room && gtk_widget_contains(row, x, y))
		open_room(list->window, room);
}


static void forget_row_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking a room or private chat offers to leave it, without opening it */
static void open_row_menu(GtkGestureClick *gesture, int presses, double x, double y, gpointer data) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	CrimsonRoom *room = g_object_get_data(G_OBJECT(row), ROOM_KEY);
	if (!room || room->kind == ROOM_SERVER)
		return;
	/* Leaving cannot be undone from here, so the item is red */
	GtkWidget *leave = gtk_button_new_with_label(room->kind == ROOM_CHANNEL ? TEXT_LEAVE_ROOM : TEXT_DELETE_CHAT);
	gtk_widget_add_css_class(leave, "flat");
	gtk_widget_add_css_class(leave, "destructive-item");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(leave), "win.leave-chat");
	gtk_actionable_set_action_target_value(GTK_ACTIONABLE(leave), g_variant_new("(ss)", room->network->key,
		room->name));
	GtkWidget *popover = gtk_popover_new();
	gtk_widget_add_css_class(popover, "menu");
	gtk_popover_set_child(GTK_POPOVER(popover), leave);
	g_signal_connect_swapped(leave, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
	gtk_widget_set_parent(popover, row);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_row_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
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
	GtkWidget *name = create_label("room-name", TRUE);
	GtkWidget *time = create_label("room-time", FALSE);
	gtk_box_append(GTK_BOX(top), kind);
	gtk_box_append(GTK_BOX(top), name);
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
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_row_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	GtkGesture *open = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(open), GDK_BUTTON_PRIMARY);
	g_signal_connect(open, "released", G_CALLBACK(click_room), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(open));
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


static gboolean is_on_room(GtkWidget *view, double x, double y) {
	for (GtkWidget *picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT); picked && picked != view;
		picked = gtk_widget_get_parent(picked)) {
		if (g_object_get_data(G_OBJECT(picked), ROOM_KEY))
			return TRUE;
	}
	return FALSE;
}


static void press_list_space(GtkGestureClick *gesture, int presses, double x, double y, RoomList *list) {
	list->is_space_pressed = !is_on_room(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
}


/* A click that starts and ends on the list's empty space closes the open chat, the same as going back, and
 * clears the keyboard focus so no room keeps its outline */
static void click_list_space(GtkGestureClick *gesture, int presses, double x, double y, RoomList *list) {
	GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (!list->is_space_pressed || is_on_room(view, x, y))
		return;
	list->is_space_pressed = FALSE;
	close_open_room(list->window);
	gtk_root_set_focus(gtk_widget_get_root(view), NULL);
}


/* The + button rises while the window has focus and sinks out of sight when another window takes it */
static void follow_window_focus(GtkWindow *window, GParamSpec *spec, RoomList *list) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->add_revealer), gtk_window_is_active(window));
}


void watch_room_list_focus(RoomList *list, GtkWidget *window) {
	g_signal_connect(window, "notify::is-active", G_CALLBACK(follow_window_focus), list);
	follow_window_focus(GTK_WINDOW(window), NULL, list);
}


/* Enter on a room opens it from the keyboard */
static void activate_room(GtkListView *view, guint position, RoomList *list) {
	g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(list->selection), position);
	if (room)
		open_room(list->window, room);
}


static void change_search(GtkSearchEntry *entry, RoomList *list) {
	gtk_filter_changed(GTK_FILTER(list->filter), GTK_FILTER_CHANGE_DIFFERENT);
	refresh_room_list_header(list);
}


/* The search field slides in from the button's side, and slides back out once it is left empty */
static void toggle_search(GtkToggleButton *button, RoomList *list) {
	gboolean is_open = gtk_toggle_button_get_active(button);
	gtk_stack_set_transition_type(GTK_STACK(list->title_stack), is_open ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
		: GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
	gtk_stack_set_visible_child_name(GTK_STACK(list->title_stack), is_open ? "search" : "title");
	if (is_open)
		gtk_widget_grab_focus(list->search);
	else
		gtk_editable_set_text(GTK_EDITABLE(list->search), "");
}


static void stop_search(GtkSearchEntry *entry, RoomList *list) {
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list->search_button), FALSE);
}


static void leave_search(GtkEventControllerFocus *focus, RoomList *list) {
	if (!*gtk_editable_get_text(GTK_EDITABLE(list->search)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list->search_button), FALSE);
}


static GtkWidget *create_header(RoomList *list) {
	list->header = adw_header_bar_new();
	gtk_widget_add_css_class(list->header, "top-bar");
	list->menu_button = create_main_menu_button(list->window);
	adw_header_bar_pack_start(ADW_HEADER_BAR(list->header), list->menu_button);
	list->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(list->search), TEXT_SEARCH);
	gtk_widget_set_hexpand(list->search, TRUE);
	gtk_widget_add_css_class(list->search, "room-search");
	g_signal_connect(list->search, "search-changed", G_CALLBACK(change_search), list);
	g_signal_connect(list->search, "stop-search", G_CALLBACK(stop_search), list);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(leave_search), list);
	gtk_widget_add_controller(list->search, focus);
	list->title = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(list->title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(list->title, "list-title");
	list->title_stack = gtk_stack_new();
	gtk_stack_set_transition_duration(GTK_STACK(list->title_stack), SEARCH_SLIDE_MS);
	gtk_stack_set_hhomogeneous(GTK_STACK(list->title_stack), FALSE);
	gtk_stack_add_named(GTK_STACK(list->title_stack), list->title, "title");
	gtk_stack_add_named(GTK_STACK(list->title_stack), list->search, "search");
	gtk_widget_set_hexpand(list->title_stack, TRUE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(list->header), list->title_stack);
	list->search_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(list->search_button), "system-search-symbolic");
	gtk_widget_set_tooltip_text(list->search_button, TEXT_SEARCH);
	/* Clicking the button must not first pull focus from the field, which would close it only for the click to
	 * open it again */
	gtk_widget_set_focus_on_click(list->search_button, FALSE);
	g_signal_connect(list->search_button, "toggled", G_CALLBACK(toggle_search), list);
	adw_header_bar_pack_end(ADW_HEADER_BAR(list->header), list->search_button);
	return list->header;
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
	gtk_widget_add_css_class(list->list_view, "room-list");
	g_signal_connect(list->list_view, "activate", G_CALLBACK(activate_room), list);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list->list_view);
	gtk_widget_set_vexpand(scroller, TRUE);
	/* Once there is a chat, joining another or starting one moves to a round button in the corner */
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), scroller);
	list->add_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(list->add_button), "list-add-symbolic");
	gtk_widget_add_css_class(list->add_button, "circular");
	gtk_widget_add_css_class(list->add_button, "suggested-action");
	gtk_widget_add_css_class(list->add_button, "add-chat");
	gtk_widget_set_tooltip_text(list->add_button, TEXT_ADD);

	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_JOIN_ROOM, "win.join-room");
	g_menu_append(menu, TEXT_START_PRIVATE_CHAT, "win.message-someone");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(list->add_button), G_MENU_MODEL(menu));
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(list->add_button), GTK_ARROW_UP);
	list->add_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(list->add_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_child(GTK_REVEALER(list->add_revealer), list->add_button);
	gtk_widget_set_halign(list->add_revealer, GTK_ALIGN_END);
	gtk_widget_set_valign(list->add_revealer, GTK_ALIGN_END);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), list->add_revealer);
	/* A spinner in the other corner while a connection is down, saying which when pointed at */
	list->connection = gtk_button_new();
	GtkWidget *spinner = adw_spinner_new();
	gtk_widget_set_size_request(spinner, CONNECTION_SPINNER_SIZE, CONNECTION_SPINNER_SIZE);
	gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
	gtk_button_set_child(GTK_BUTTON(list->connection), spinner);
	gtk_widget_add_css_class(list->connection, "circular");
	gtk_widget_add_css_class(list->connection, "connection-spinner");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(list->connection), "win.retry-connection");
	gtk_widget_set_halign(list->connection, GTK_ALIGN_START);
	gtk_widget_set_valign(list->connection, GTK_ALIGN_END);
	gtk_widget_set_visible(list->connection, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), list->connection);
	GtkGesture *space = gtk_gesture_click_new();
	g_signal_connect(space, "pressed", G_CALLBACK(press_list_space), list);
	g_signal_connect(space, "released", G_CALLBACK(click_list_space), list);
	gtk_widget_add_controller(list->list_view, GTK_EVENT_CONTROLLER(space));
	return overlay;
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
	g_signal_connect(list->selection, "items-changed", G_CALLBACK(follow_listed_rooms), list);
}


RoomList *create_room_list(Window *window) {
	RoomList *list = g_new0(RoomList, 1);
	list->window = window;
	build_models(list);
	list->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(list->box, "room-column");
	gtk_box_append(GTK_BOX(list->box), create_header(list));
	list->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(list->stack), create_list_view(list), "list");
	gtk_stack_add_named(GTK_STACK(list->stack), create_no_rooms_page(), "no-rooms");
	gtk_box_append(GTK_BOX(list->box), list->stack);
	set_room_list_narrow(list, FALSE);
	return list;
}


GtkWidget *get_room_list_widget(RoomList *list) {
	return list->box;
}


void refresh_room_list_header(RoomList *list) {
	const char *search = gtk_editable_get_text(GTK_EDITABLE(list->search));
	gboolean is_searching = search && *search;
	gboolean has_rooms = g_list_model_get_n_items(G_LIST_MODEL(list->selection)) > 0;
	gtk_stack_set_visible_child_name(GTK_STACK(list->stack), has_rooms || is_searching
		|| !list->window->chat->has_started ? "list" : "no-rooms");
	gtk_widget_set_visible(list->add_revealer, has_rooms || is_searching);
	/* The header names the app, the search field the server, and a spinner in the corner says when it is down */
	gtk_label_set_text(GTK_LABEL(list->title), TEXT_APPLICATION_NAME);
	g_autofree char *placeholder = list->network ? g_strdup_printf(TEXT_SEARCH_IN, list->network->name)
		: g_strdup(TEXT_SEARCH);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(list->search), placeholder);
}


void show_room_list_network(RoomList *list, CrimsonNetwork *network) {
	list->network = network;
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
	gtk_single_selection_set_selected(list->selection, found);
}


void resort_room_list(RoomList *list) {
	gtk_sorter_changed(GTK_SORTER(list->sorter), GTK_SORTER_CHANGE_DIFFERENT);
	gtk_filter_changed(GTK_FILTER(list->filter), GTK_FILTER_CHANGE_DIFFERENT);
}


void set_room_list_narrow(RoomList *list, gboolean is_narrow) {
	gtk_widget_set_visible(list->menu_button, is_narrow);
}


void focus_room_search(RoomList *list) {
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list->search_button), TRUE);
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


void show_room_list_connection(RoomList *list, const char *message) {
	gtk_widget_set_visible(list->connection, message != NULL);
	gtk_widget_set_tooltip_text(list->connection, message);
	if (message)
		gtk_widget_add_css_class(list->list_view, "is-offline");
	else
		gtk_widget_remove_css_class(list->list_view, "is-offline");
}
