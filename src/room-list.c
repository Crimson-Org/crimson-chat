#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "notifications.h"
#include "room-look.h"
#include "settings.h"
#include "window.h"

#define ROOM_AVATAR_SIZE 50
#define SEARCH_SLIDE_MS 260
#define CONNECTION_SPINNER_SIZE 24
#define ROOM_KEY "room"
#define HANDLER_KEY "changed-handler"
#define WATCHED_KEY "watched"
#define MINUTES_KEY "minutes"
#define ENABLES_KEY "enables"
/* The minutes a room's notifications change for unless the user picks another number, and the most they can */
#define OVERRIDE_MINUTES 60
#define OVERRIDE_MINUTES_MOST (7 * 24 * 60)

struct RoomList {
	Window *window;
	GtkWidget *box, *header, *search, *menu_button;
	/* The server's name until the search button slides the search field open in its place */
	GtkWidget *title_stack, *title, *search_button;
	/* Whether the pointer is over the chat list, which shows the + button */
	GtkEventControllerMotion *list_hover;
	GtkWidget *add_button, *add_revealer, *connection, *connection_button, *connection_icons, *connection_note,
		*connection_label, *connection_fix;
	/* The account offer above the chats, its words, its button and the button that closes it */
	GtkWidget *banner, *banner_label, *banner_button, *banner_close;
	/* The server the banner is about and what it offers, which its buttons act on when clicked */
	char *banner_network;
	AccountBanner banner_kind;
	/* Whether the press now going on started on the list's empty space, whether the list holds chats, and whether
	 * the corner shows a problem rather than a wait */
	gboolean is_space_pressed, has_chats, is_problem;
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
	if ((room->kind == ROOM_SERVER && !room->is_listed) || room->network != list->network)
		return FALSE;
	const char *search = gtk_editable_get_text(GTK_EDITABLE(list->search));
	if (!search || !*search)
		return TRUE;
	g_autofree char *name = g_utf8_casefold(room->name, -1);
	g_autofree char *wanted = g_utf8_casefold(search, -1);
	return strstr(name, wanted) != NULL;
}


/* When something last happened in a room: its latest message, or someone reacting to the user's own */
static gint64 read_last_activity(const CrimsonRoom *room) {
	return room->new_reaction ? MAX(room->last_time, room->new_reaction_time) : room->last_time;
}


/* Every chat, the server's messages included, follows its latest activity, newest first */
static int order_rooms(gconstpointer first, gconstpointer second, gpointer data) {
	const CrimsonRoom *one = first, *other = second;
	gint64 one_time = read_last_activity(one), other_time = read_last_activity(other);
	if (one_time != other_time)
		return one_time > other_time ? -1 : 1;
	return g_utf8_collate(one->name, other->name);
}


static void fill_row(GtkWidget *row, CrimsonRoom *room) {
	AdwAvatar *avatar = g_object_get_data(G_OBJECT(row), "avatar");
	GtkWidget *name = g_object_get_data(G_OBJECT(row), "name");
	GtkWidget *time = g_object_get_data(G_OBJECT(row), "time");
	GtkWidget *preview = g_object_get_data(G_OBJECT(row), "preview");
	GtkWidget *badge = g_object_get_data(G_OBJECT(row), "badge");
	GtkWidget *kind = g_object_get_data(G_OBJECT(row), "kind");
	GtkWidget *bottom = g_object_get_data(G_OBJECT(row), "bottom");
	fill_room_avatar(avatar, room, ROOM_AVATAR_SIZE);
	g_autofree char *title = describe_room_title(room);
	gtk_label_set_text(GTK_LABEL(name), title);
	gtk_image_set_from_icon_name(GTK_IMAGE(kind), "network-server-symbolic");
	gtk_widget_set_visible(kind, room->kind == ROOM_SERVER);
	g_autofree char *moment = format_list_time(room->last_time);
	gtk_label_set_text(GTK_LABEL(time), moment);
	g_autofree char *line = describe_room_preview(room);
	gboolean shows_typing = is_room_typing(room) && get_settings()->shows_typing;
	show_room_line(preview, line, shows_typing);
	gboolean shows_badge = shows_room_badge(room);
	style_unread_badge(badge, shows_badge ? room->unread_count : 0, room->has_mention || room->new_reaction);
	/* An unseen reaction to the user's message shows as itself where no unread lines are counted */
	if (shows_badge && room->new_reaction && !room->unread_count) {
		gtk_label_set_text(GTK_LABEL(badge), room->new_reaction);
		gtk_widget_set_visible(badge, TRUE);
	}
	/* A chat with nothing to show under its name keeps the name centred beside the avatar */
	gtk_widget_set_visible(bottom, (line && *line) || shows_typing || gtk_widget_get_visible(badge));
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


/* A menu item reading from the left like the app's other menus, with an arrow for one that opens a submenu */
static GtkWidget *create_menu_item(const char *text, const char *css_class, const char *icon_name,
	gboolean is_icon_first) {
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(content), label);
	if (icon_name) {
		GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
		if (is_icon_first)
			gtk_box_prepend(GTK_BOX(content), icon);
		else
			gtk_box_append(GTK_BOX(content), icon);
	}
	GtkWidget *item = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(item), content);
	gtk_widget_add_css_class(item, "flat");
	gtk_widget_add_css_class(item, css_class);
	return item;
}


static GtkWidget *add_menu_item(GtkWidget *items, GtkWidget *popover, const char *text, const char *css_class) {
	GtkWidget *item = create_menu_item(text, css_class, NULL, FALSE);
	g_signal_connect_swapped(item, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
	gtk_box_append(GTK_BOX(items), item);
	return item;
}


static void set_room_action(GtkWidget *item, const char *action, CrimsonRoom *room) {
	gtk_actionable_set_action_name(GTK_ACTIONABLE(item), action);
	gtk_actionable_set_action_target_value(GTK_ACTIONABLE(item), g_variant_new("(ss)", room->network->key,
		room->name));
}


static void override_notifications(GtkWidget *popover, gint64 until) {
	CrimsonRoom *room = g_object_get_data(G_OBJECT(popover), ROOM_KEY);
	gboolean enables = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popover), ENABLES_KEY));
	set_room_override(get_settings()->notification_overrides, room->network->key, room->name, enables, until);
	gtk_popover_popdown(GTK_POPOVER(popover));
}


static void override_notifications_forever(GtkButton *item, GtkWidget *popover) {
	override_notifications(popover, 0);
}


static void override_notifications_for_minutes(GtkWidget *source, GtkWidget *popover) {
	GtkSpinButton *minutes = g_object_get_data(G_OBJECT(popover), MINUTES_KEY);
	gint64 length = (gint64)gtk_spin_button_get_value_as_int(minutes) * 60 * G_USEC_PER_SEC;
	override_notifications(popover, g_get_real_time() + length);
}


static void use_default_notifications(GtkButton *item, GtkWidget *popover) {
	CrimsonRoom *room = g_object_get_data(G_OBJECT(popover), ROOM_KEY);
	clear_room_override(get_settings()->notification_overrides, room->network->key, room->name);
}


static void name_minutes_item(GtkSpinButton *minutes, GtkButton *item) {
	int count = gtk_spin_button_get_value_as_int(minutes);
	g_autofree char *text = count == 1 ? g_strdup(TEXT_FOR_ONE_MINUTE) : g_strdup_printf(TEXT_FOR_MINUTES, count);
	GtkWidget *content = gtk_button_get_child(item);
	gtk_label_set_text(GTK_LABEL(gtk_widget_get_first_child(content)), text);
}


static void show_menu_page(GtkWidget *item, const char *page) {
	GtkWidget *stack = gtk_widget_get_ancestor(item, GTK_TYPE_STACK);
	gtk_stack_set_visible_child_name(GTK_STACK(stack), page);
}


static void open_notifications_page(GtkButton *item, gpointer data) {
	show_menu_page(GTK_WIDGET(item), "notifications");
}


static void close_notifications_page(GtkButton *item, gpointer data) {
	show_menu_page(GTK_WIDGET(item), "main");
}


/* The submenu switches the room's notifications the other way for ever or for a number of minutes */
static GtkWidget *create_notifications_page(GtkWidget *popover, const char *title) {
	GtkWidget *items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *back = create_menu_item(title, "plain-item", "go-previous-symbolic", TRUE);
	g_signal_connect(back, "clicked", G_CALLBACK(close_notifications_page), NULL);
	gtk_box_append(GTK_BOX(items), back);
	gtk_box_append(GTK_BOX(items), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	GtkWidget *forever = create_menu_item(TEXT_FOREVER, "plain-item", NULL, FALSE);
	g_signal_connect(forever, "clicked", G_CALLBACK(override_notifications_forever), popover);
	gtk_box_append(GTK_BOX(items), forever);
	GtkWidget *timed = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *minutes = gtk_spin_button_new_with_range(1, OVERRIDE_MINUTES_MOST, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(minutes), OVERRIDE_MINUTES);
	gtk_widget_add_css_class(minutes, "override-minutes");
	g_object_set_data(G_OBJECT(popover), MINUTES_KEY, minutes);
	GtkWidget *for_minutes = create_menu_item(NULL, "plain-item", NULL, FALSE);
	gtk_widget_set_hexpand(for_minutes, TRUE);
	name_minutes_item(GTK_SPIN_BUTTON(minutes), GTK_BUTTON(for_minutes));
	g_signal_connect(minutes, "value-changed", G_CALLBACK(name_minutes_item), for_minutes);
	g_signal_connect(minutes, "activate", G_CALLBACK(override_notifications_for_minutes), popover);
	g_signal_connect(for_minutes, "clicked", G_CALLBACK(override_notifications_for_minutes), popover);
	gtk_box_append(GTK_BOX(timed), for_minutes);
	gtk_box_append(GTK_BOX(timed), minutes);
	gtk_box_append(GTK_BOX(items), timed);
	return items;
}


/* A room set apart from the defaults only offers to restore them, while any other offers the submenu that switches
 * its notifications the other way */
static void add_notifications_item(GtkWidget *items, GtkWidget *pages, GtkWidget *popover, CrimsonRoom *room) {
	if (find_room_override(get_settings()->notification_overrides, room->network->key, room->name)) {
		GtkWidget *restore = add_menu_item(items, popover, TEXT_RESTORE_NOTIFICATIONS, "plain-item");
		g_signal_connect(restore, "clicked", G_CALLBACK(use_default_notifications), popover);
		return;
	}
	gboolean enables = !is_room_notified(room);
	g_object_set_data(G_OBJECT(popover), ENABLES_KEY, GINT_TO_POINTER(enables));
	const char *title = enables ? TEXT_ENABLE_NOTIFICATIONS : TEXT_DISABLE_NOTIFICATIONS;
	GtkWidget *notifications = create_menu_item(title, "plain-item", "go-next-symbolic", FALSE);
	g_signal_connect(notifications, "clicked", G_CALLBACK(open_notifications_page), NULL);
	gtk_box_append(GTK_BOX(items), notifications);
	gtk_stack_add_named(GTK_STACK(pages), create_notifications_page(popover, title), "notifications");
}


static void switch_badges(GtkButton *item, CrimsonRoom *room) {
	set_room_override(get_settings()->badge_overrides, room->network->key, room->name, !is_room_badged(room), 0);
	announce_room_change(room);
	refresh_window_later(get_main_window());
}


static void use_default_badges(GtkButton *item, CrimsonRoom *room) {
	clear_room_override(get_settings()->badge_overrides, room->network->key, room->name);
	announce_room_change(room);
	refresh_window_later(get_main_window());
}


/* Badges switch the other way at once, or back to the defaults for a room already set apart */
static void add_badges_item(GtkWidget *items, GtkWidget *popover, CrimsonRoom *room) {
	gboolean is_overridden = find_room_override(get_settings()->badge_overrides, room->network->key, room->name)
		!= NULL;
	const char *title = is_overridden ? TEXT_RESTORE_BADGES : is_room_badged(room) ? TEXT_DISABLE_BADGES
		: TEXT_ENABLE_BADGES;
	GtkWidget *badges = add_menu_item(items, popover, title, "plain-item");
	g_object_set_data_full(G_OBJECT(badges), ROOM_KEY, g_object_ref(room), g_object_unref);
	g_signal_connect(badges, "clicked", G_CALLBACK(is_overridden ? use_default_badges : switch_badges), room);
}


/* Right-clicking a room or private chat offers its card or the person's profile first, then its notifications,
 * then leaving it */
static void open_row_menu(GtkGestureClick *gesture, int presses, double x, double y, gpointer data) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	CrimsonRoom *room = g_object_get_data(G_OBJECT(row), ROOM_KEY);
	if (!room)
		return;
	GtkWidget *popover = gtk_popover_new();
	gtk_widget_add_css_class(popover, "menu");
	g_object_set_data_full(G_OBJECT(popover), ROOM_KEY, g_object_ref(room), g_object_unref);
	GtkWidget *items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(pages), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_set_hhomogeneous(GTK_STACK(pages), FALSE);
	gtk_stack_set_vhomogeneous(GTK_STACK(pages), FALSE);
	gtk_stack_set_interpolate_size(GTK_STACK(pages), TRUE);
	gtk_stack_add_named(GTK_STACK(pages), items, "main");
	/* The server's messages never notify and only leave the list */
	gboolean is_server = room->kind == ROOM_SERVER;
	if (!is_server) {
		GtkWidget *details = add_menu_item(items, popover, room->kind == ROOM_CHANNEL ? TEXT_ROOM_DETAILS
			: TEXT_VIEW_PROFILE, "plain-item");
		set_room_action(details, "win.chat-details", room);
		add_notifications_item(items, pages, popover, room);
		add_badges_item(items, popover, room);
	}
	set_room_action(add_menu_item(items, popover, TEXT_MARK_READ, "plain-item"), "win.mark-read", room);
	/* Leaving cannot be undone from here, so the item is red */
	GtkWidget *leave = add_menu_item(items, popover, is_server ? TEXT_HIDE : room->kind == ROOM_CHANNEL
		? TEXT_LEAVE_ROOM : TEXT_DELETE_CHAT, is_server ? "plain-item" : "destructive-item");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(leave), is_server ? "win.hide-server-messages" : "win.leave-chat");
	gtk_actionable_set_action_target_value(GTK_ACTIONABLE(leave), is_server ? g_variant_new_string(room->network->key)
		: g_variant_new("(ss)", room->network->key, room->name));
	gtk_popover_set_child(GTK_POPOVER(popover), pages);
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
	g_object_set_data(G_OBJECT(row), "bottom", bottom);
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


/* A chat's time turns from the clock time into a weekday once its day is over */
static void refill_rows_for_new_day(gpointer data) {
	RoomList *list = data;
	for (GtkWidget *item = gtk_widget_get_first_child(list->list_view); item;
		item = gtk_widget_get_next_sibling(item)) {
		GtkWidget *row = gtk_widget_get_first_child(item);
		CrimsonRoom *room = row ? g_object_get_data(G_OBJECT(row), ROOM_KEY) : NULL;
		if (room)
			fill_row(row, room);
	}
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


static void show_connection_note(GtkEventControllerMotion *motion, double x, double y, RoomList *list) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->connection_note), TRUE);
}


static void hide_connection_note(GtkEventControllerMotion *motion, RoomList *list) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->connection_note), list->is_problem);
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


static void forget_space_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking the list's empty space offers what the + button offers */
static void open_space_menu(GtkGestureClick *gesture, int presses, double x, double y, RoomList *list) {
	GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (is_on_room(view, x, y))
		return;
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_JOIN_ROOM, "win.join-room");
	g_menu_append(menu, TEXT_MESSAGE_SOMEONE, "win.message-someone");
	GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
	gtk_widget_set_parent(popover, view);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_space_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


/* The + button rises while the window has focus and the pointer is over the chat list, and stays while its menu
 * is open */
static void show_add_button(RoomList *list) {
	gboolean is_active = gtk_window_is_active(GTK_WINDOW(list->window->window));
	gboolean is_pointed_at = gtk_event_controller_motion_contains_pointer(list->list_hover);
	gboolean is_open = gtk_menu_button_get_active(GTK_MENU_BUTTON(list->add_button));
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->add_revealer), is_open || (is_active && is_pointed_at));
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
	/* The search field fills the header, while the title keeps its own width so the header can centre it */
	gtk_widget_set_hexpand(list->title_stack, is_open);
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
	gtk_widget_add_css_class(list->header, "title-bar");
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
	gtk_widget_set_hexpand(list->title_stack, FALSE);
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
	gtk_widget_add_css_class(list->add_button, "add-chat");
	gtk_widget_set_tooltip_text(list->add_button, TEXT_ADD);

	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_JOIN_ROOM, "win.join-room");
	g_menu_append(menu, TEXT_MESSAGE_SOMEONE, "win.message-someone");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(list->add_button), G_MENU_MODEL(menu));
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(list->add_button), GTK_ARROW_UP);
	list->add_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(list->add_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_child(GTK_REVEALER(list->add_revealer), list->add_button);
	gtk_widget_set_halign(list->add_revealer, GTK_ALIGN_END);
	gtk_widget_set_valign(list->add_revealer, GTK_ALIGN_END);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), list->add_revealer);
	list->list_hover = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());
	g_signal_connect_swapped(list->list_hover, "notify::contains-pointer", G_CALLBACK(show_add_button), list);
	gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(list->list_hover));
	g_signal_connect_swapped(list->add_button, "notify::active", G_CALLBACK(show_add_button), list);
	g_signal_connect_swapped(list->window->window, "notify::is-active", G_CALLBACK(show_add_button), list);
	/* A spinner in the other corner while a connection is down, or a warning when it needs the user, with what it
	 * is about sliding out beside it */
	list->connection_button = gtk_button_new();
	list->connection_icons = gtk_stack_new();
	GtkWidget *spinner = create_spinner();
	gtk_widget_set_size_request(spinner, CONNECTION_SPINNER_SIZE, CONNECTION_SPINNER_SIZE);
	gtk_stack_add_named(GTK_STACK(list->connection_icons), spinner, "waiting");
	gtk_stack_add_named(GTK_STACK(list->connection_icons), gtk_image_new_from_icon_name("dialog-warning-symbolic"),
		"problem");
	gtk_widget_set_halign(list->connection_icons, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(list->connection_icons, GTK_ALIGN_CENTER);
	gtk_button_set_child(GTK_BUTTON(list->connection_button), list->connection_icons);
	gtk_widget_add_css_class(list->connection_button, "circular");
	gtk_widget_add_css_class(list->connection_button, "flat");
	gtk_widget_add_css_class(list->connection_button, "connection-spinner");
	gtk_widget_set_cursor_from_name(list->connection_button, "pointer");
	list->connection_label = gtk_label_new(NULL);
	/* The notes are a few words, kept on one line, since a wrapping label beside a button measures too narrow */
	gtk_label_set_ellipsize(GTK_LABEL(list->connection_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(list->connection_label), 0);
	gtk_widget_add_css_class(list->connection_label, "connection-note");
	list->connection_note = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(list->connection_note), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
	list->connection_fix = gtk_button_new();
	gtk_widget_add_css_class(list->connection_fix, "suggested-action");
	gtk_widget_add_css_class(list->connection_fix, "connection-fix");
	gtk_widget_set_valign(list->connection_fix, GTK_ALIGN_CENTER);
	/* The words and the fix share one row, like a toast, so the pill stays a single line */
	GtkWidget *note_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(note_box, "connection-note-box");
	gtk_widget_set_valign(list->connection_label, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(note_box), list->connection_label);
	gtk_box_append(GTK_BOX(note_box), list->connection_fix);
	gtk_revealer_set_child(GTK_REVEALER(list->connection_note), note_box);
	gtk_widget_set_valign(list->connection_note, GTK_ALIGN_CENTER);
	/* One rounded shape that is a circle around the spinner and grows into a pill as the note slides out */
	list->connection = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(list->connection, "connection-pill");
	gtk_box_append(GTK_BOX(list->connection), list->connection_button);
	gtk_box_append(GTK_BOX(list->connection), list->connection_note);
	GtkEventController *hover = gtk_event_controller_motion_new();
	g_signal_connect(hover, "enter", G_CALLBACK(show_connection_note), list);
	g_signal_connect(hover, "leave", G_CALLBACK(hide_connection_note), list);
	gtk_widget_add_controller(list->connection, hover);
	gtk_widget_set_halign(list->connection, GTK_ALIGN_START);
	gtk_widget_set_valign(list->connection, GTK_ALIGN_END);
	gtk_widget_set_visible(list->connection, FALSE);
	GtkGesture *space = gtk_gesture_click_new();
	g_signal_connect(space, "pressed", G_CALLBACK(press_list_space), list);
	g_signal_connect(space, "released", G_CALLBACK(click_list_space), list);
	gtk_widget_add_controller(list->list_view, GTK_EVENT_CONTROLLER(space));
	GtkGesture *menu_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu_click, "pressed", G_CALLBACK(open_space_menu), list);
	gtk_widget_add_controller(list->list_view, GTK_EVENT_CONTROLLER(menu_click));
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


static void run_banner_action(RoomList *list, const char *action) {
	if (list->banner_network)
		g_action_group_activate_action(G_ACTION_GROUP(list->window->window), action,
			g_variant_new_string(list->banner_network));
}


static void press_banner_button(GtkButton *button, RoomList *list) {
	run_banner_action(list, list->banner_kind == ACCOUNT_BANNER_LOG_IN ? "own-profile" : "register-account");
}


static void close_banner(GtkButton *button, RoomList *list) {
	run_banner_action(list, list->banner_kind == ACCOUNT_BANNER_LOG_IN ? "dismiss-login" : "dismiss-register");
}


static GtkWidget *create_banner(RoomList *list) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(box, "account-banner");
	list->banner_label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(list->banner_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(list->banner_label), 0);
	gtk_widget_set_hexpand(list->banner_label, TRUE);
	gtk_box_append(GTK_BOX(box), list->banner_label);
	list->banner_button = gtk_button_new();
	gtk_widget_add_css_class(list->banner_button, "suggested-action");
	gtk_widget_add_css_class(list->banner_button, "connection-fix");
	gtk_widget_set_valign(list->banner_button, GTK_ALIGN_CENTER);
	g_signal_connect(list->banner_button, "clicked", G_CALLBACK(press_banner_button), list);
	gtk_box_append(GTK_BOX(box), list->banner_button);
	list->banner_close = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_add_css_class(list->banner_close, "flat");
	gtk_widget_add_css_class(list->banner_close, "circular");
	gtk_widget_set_valign(list->banner_close, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(list->banner_close, TEXT_CLOSE);
	g_signal_connect(list->banner_close, "clicked", G_CALLBACK(close_banner), list);
	gtk_box_append(GTK_BOX(box), list->banner_close);
	list->banner = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(list->banner), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(list->banner), box);
	return list->banner;
}


void show_room_list_banner(RoomList *list, AccountBanner banner, const char *network_key) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->banner), banner != ACCOUNT_BANNER_NONE);
	if (banner == ACCOUNT_BANNER_NONE)
		return;
	gboolean is_login = banner == ACCOUNT_BANNER_LOG_IN;
	gtk_label_set_text(GTK_LABEL(list->banner_label), is_login ? TEXT_LOGIN_BANNER : TEXT_REGISTER_BANNER);
	gtk_button_set_label(GTK_BUTTON(list->banner_button), is_login ? TEXT_LOG_IN : TEXT_REGISTER);
	g_free(list->banner_network);
	list->banner_network = g_strdup(network_key);
	list->banner_kind = banner;
}


RoomList *create_room_list(Window *window) {
	RoomList *list = g_new0(RoomList, 1);
	list->window = window;
	build_models(list);
	list->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(list->box, "room-column");
	create_header(list);
	gtk_box_append(GTK_BOX(list->box), create_banner(list));
	list->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(list->stack), create_list_view(list), "list");
	watch_day_change(refill_rows_for_new_day, list);
	gtk_stack_add_named(GTK_STACK(list->stack), create_no_rooms_page(), "no-rooms");
	/* The connection note sits over every page of the list, including the one for a server without chats */
	GtkWidget *pages = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(pages), list->stack);
	gtk_overlay_add_overlay(GTK_OVERLAY(pages), list->connection);
	gtk_widget_set_vexpand(pages, TRUE);
	gtk_box_append(GTK_BOX(list->box), pages);
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
	list->has_chats = has_rooms || is_searching;
	gtk_widget_set_visible(list->add_revealer, list->has_chats && !gtk_widget_get_visible(list->connection));
	/* The header names the app, the search field the server, and a spinner in the corner says when it is down */
	gtk_label_set_text(GTK_LABEL(list->title), TEXT_APPLICATION_NAME);
	g_autofree char *placeholder = list->network ? g_strdup_printf(TEXT_SEARCH_IN, list->network->name)
		: g_strdup(TEXT_SEARCH);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(list->search), placeholder);
}


/* Switching servers changes the list at once, without an animation */
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


GtkWidget *get_room_list_header(RoomList *list) {
	return list->header;
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


void show_room_list_connection(RoomList *list, const ConnectionNote *note) {
	gtk_widget_set_visible(list->connection, note != NULL);
	list->is_problem = note && note->is_problem;
	if (note) {
		gtk_label_set_text(GTK_LABEL(list->connection_label), note->message);
		gtk_widget_set_visible(list->connection_fix, note->fix_label != NULL);
		/* With a fix the pill ends in the button, as far from the edge as from the top and bottom */
		GtkWidget *note_box = gtk_widget_get_parent(list->connection_fix);
		if (note->fix_label)
			gtk_widget_add_css_class(note_box, "has-fix");
		else
			gtk_widget_remove_css_class(note_box, "has-fix");
		if (note->fix_label) {
			gtk_button_set_label(GTK_BUTTON(list->connection_fix), note->fix_label);
			gtk_actionable_set_action_name(GTK_ACTIONABLE(list->connection_fix), note->fix_action);
			gtk_actionable_set_action_target_value(GTK_ACTIONABLE(list->connection_fix), note->fix_target);
		}
		gtk_stack_set_visible_child_name(GTK_STACK(list->connection_icons), note->is_problem ? "problem" : "waiting");
		gtk_actionable_set_action_name(GTK_ACTIONABLE(list->connection_button), note->action);
		gtk_actionable_set_action_target_value(GTK_ACTIONABLE(list->connection_button), note->target);
	}
	/* A problem keeps its explanation out, while waiting only shows it when pointed at */
	gtk_revealer_set_reveal_child(GTK_REVEALER(list->connection_note), list->is_problem);
	/* While waiting there is nowhere to join or message yet, so the + button steps aside */
	gtk_widget_set_visible(list->add_revealer, !note && list->has_chats);
	if (note)
		gtk_widget_add_css_class(list->list_view, "is-offline");
	else
		gtk_widget_remove_css_class(list->list_view, "is-offline");
}
