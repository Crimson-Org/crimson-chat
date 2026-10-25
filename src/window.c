#include <string.h>

#include "../lang/en.h"
#include "common/sidebar-width.h"
#include "common/startup.h"
#include "dialogs.h"
#include "images.h"
#include "notifications.h"
#include "profile.h"
#include "register.h"
#include "room-picker.h"
#include "settings.h"
#include "state.h"
#include "storage.h"
#include "window.h"
#include "wizards.h"
#include "pictures.h"

#define NARROW_CONDITION "max-width: 640sp"
#define RAIL_AVATAR_SIZE 48
#define SIDEBAR_WIDTH 420
#define SIDEBAR_MINIMUM 410
#define SIDEBAR_MAXIMUM 640
#define BADGE_LIMIT 99
#define NETWORK_KEY "network"
#define CONNECTION_DELAY_MS 1500
#define RAIL_DIMMED 0.45
/* How long after connecting the offer to register waits, so a login prompt from NickServ can come first */
#define REGISTER_OFFER_DELAY_US (5 * G_USEC_PER_SEC)
#define MOUSE_BACK_BUTTON 8
/* How many earlier chats going back can return through */
#define ROOM_HISTORY_LIMIT 50
#define MINIMUM_WIDTH 452
#define RAIL_SPINNER_SIZE 22
#define MINIMUM_HEIGHT 668
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
	/* Starting something new on the server comes first, above everything about the server itself */
	g_autoptr(GMenu) starts = g_menu_new();
	append_network_item(starts, TEXT_JOIN_ROOM, "win.join-room-on", network);
	append_network_item(starts, TEXT_MESSAGE_SOMEONE, "win.message-on", network);
	g_menu_append_section(items, NULL, G_MENU_MODEL(starts));
	g_autoptr(GMenu) about = g_menu_new();
	/* Without the rail, a server's menu is the way to switch to its chats */
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	if (!window->has_rail && count > 1)
		append_network_item(about, TEXT_SHOW_CHATS, "win.choose-server", network);
	gboolean has_mention = FALSE;
	if (count_network_badges(network, &has_mention))
		append_network_item(about, TEXT_MARK_READ, "win.mark-server-read", network);
	append_network_item(about, TEXT_ACCOUNT_MENU, "win.own-profile", network);
	append_network_item(about, TEXT_SERVER_MESSAGES, "win.server-messages", network);
	append_network_item(about, TEXT_SERVER_SETTINGS, "win.network-settings", network);
	append_network_item(about, TEXT_RECONNECT, "win.reconnect-server", network);
	append_network_item(about, TEXT_REMOVE_SERVER, "win.remove-network", network);
	g_menu_append_section(items, NULL, G_MENU_MODEL(about));
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


/* Whether the link behind a server is down with nothing trying it again, which only the user can change */
static gboolean is_stuck(CrimsonNetwork *network) {
	Session *session = network->session;
	if (session->record->is_bouncer && read_account_state(session) != NETWORK_CONNECTED)
		return read_account_state(session) == NETWORK_DISCONNECTED && session->control
			&& !will_irc_client_retry(session->control);
	return network->state == NETWORK_DISCONNECTED && !is_network_retrying(network);
}


/* What stopped a connection, in kind: a refused login, an untrusted certificate, or anything else */
typedef enum {
	STOP_OTHER,
	STOP_LOGIN,
	STOP_CERTIFICATE,
} StopKind;


static StopKind read_stop_kind(const char *error) {
	g_autofree char *reason = error ? g_ascii_strdown(error, -1) : NULL;
	if (reason && strstr(reason, "certificate"))
		return STOP_CERTIFICATE;
	if (reason && (strstr(reason, "sasl") || strstr(reason, "authentication") || strstr(reason, "password")
		|| strstr(reason, "login")))
		return STOP_LOGIN;
	return STOP_OTHER;
}


/* A stopped connection is described in a few plain words, while the server's messages keep the technical reason */
static char *describe_stop(const char *error) {
	switch (read_stop_kind(error)) {
	case STOP_LOGIN: return g_strdup(TEXT_LOGIN_REFUSED);
	case STOP_CERTIFICATE: return g_strdup(TEXT_CERTIFICATE_REFUSED);
	default: return g_strdup(TEXT_CANNOT_CONNECT);
	}
}


/* Only says reconnecting when something really is trying again, and otherwise says plainly why it stopped */
char *describe_connection_wait(CrimsonNetwork *network) {
	Session *session = network->session;
	gboolean is_stopped = is_stuck(network);
	if (session->record->is_bouncer && read_account_state(session) != NETWORK_CONNECTED) {
		if (is_stopped)
			return describe_stop(read_account_error(session));
		return g_strdup(session->has_connected ? TEXT_RECONNECTING : TEXT_CONNECTING);
	}
	if (network->state == NETWORK_CONNECTED)
		return NULL;
	if (is_stopped)
		return describe_stop(read_network_error(network));
	return g_strdup(network->state == NETWORK_CONNECTING && !session->has_connected ? TEXT_CONNECTING
		: TEXT_RECONNECTING);
}


static void fill_rail_row(GtkWidget *row, CrimsonNetwork *network) {
	AdwAvatar *avatar = g_object_get_data(G_OBJECT(row), "avatar");
	GtkWidget *badge = g_object_get_data(G_OBJECT(row), "badge");
	adw_avatar_set_text(avatar, network->name);
	g_autofree char *icon = fill_image_size(pick_network_picture(network), RAIL_AVATAR_SIZE * 2);
	show_avatar_image(avatar, icon);
	gboolean has_mention = FALSE;
	guint unread = count_network_badges(network, &has_mention);
	style_unread_badge(badge, unread, has_mention);
	/* The picture dims along with the spinner, once the wait has lasted, and a warning shows at once */
	GtkWidget *state_shown = g_object_get_data(G_OBJECT(row), "state");
	gboolean is_dimmed = network->state != NETWORK_CONNECTED
		&& (is_stuck(network) || gtk_widget_get_opacity(state_shown) > 0);
	gtk_widget_set_opacity(GTK_WIDGET(avatar), is_dimmed ? RAIL_DIMMED : 1.0);
	/* A server that is not connected shows a spinner over its picture while it connects, or a warning when it
	 * stopped with a reason and waits for the user */
	gboolean is_stopped = is_stuck(network);
	GtkWidget *state = g_object_get_data(G_OBJECT(row), "state");
	gtk_widget_set_visible(state, network->state != NETWORK_CONNECTED);
	gtk_stack_set_visible_child_name(GTK_STACK(state), is_stopped ? "problem" : "waiting");
	if (is_stopped)
		gtk_widget_set_opacity(state, 1);
	/* A connected server only names itself, and one that is not says what the corner of the list says about it */
	g_autofree char *wait = describe_connection_wait(network);
	g_autofree char *tooltip = wait ? g_strdup_printf("%s\n%s", network->name, wait) : g_strdup(network->name);
	gtk_widget_set_tooltip_text(row, tooltip);
}


static void dim_rail_avatar(gpointer avatar) {
	gtk_widget_set_opacity(avatar, RAIL_DIMMED);
}


static void refresh_rail_row(CrimsonNetwork *network, gpointer row) {
	fill_rail_row(row, network);
}


static void select_rail_row(Window *window, CrimsonNetwork *network);
static CrimsonNetwork *find_keyed_network(Window *window, GVariant *parameter);


/* A server dragged in the rail carries its key, and shows as a picture of its row under the pointer */
static GdkContentProvider *start_rail_drag(GtkDragSource *source, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	CrimsonNetwork *network = g_object_get_data(G_OBJECT(row), NETWORK_KEY);
	window->dragged_network = network;
	g_autoptr(GdkPaintable) picture = gtk_widget_paintable_new(row);
	gtk_drag_source_set_icon(source, picture, (int)x, (int)y);
	return gdk_content_provider_new_typed(G_TYPE_STRING, network->key);
}


/* Whether a drop over this point of the row lands before it rather than after */
static gboolean is_drop_before(GtkWidget *row, double y) {
	return y < gtk_widget_get_height(row) / 2.0;
}


/* Only one line shows at a time, so the previous one goes before another is drawn */
static void clear_drop_line(Window *window) {
	if (!window->drop_line_row)
		return;
	gtk_widget_remove_css_class(window->drop_line_row, "drop-before");
	gtk_widget_remove_css_class(window->drop_line_row, "drop-after");
	g_clear_object(&window->drop_line_row);
}


static gboolean is_dragged_row(Window *window, GtkWidget *row) {
	return row && g_object_get_data(G_OBJECT(row), NETWORK_KEY) == window->dragged_network;
}


/* One line marks the gap the dragged server would land in, drawn on the server below the gap, or below the last
 * server for the end of the rail; the gaps on either side of the dragged server would change nothing, so they
 * show no line */
static GdkDragAction follow_rail_drag(GtkDropTarget *target, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	GtkWidget *below = is_drop_before(row, y) ? row : gtk_widget_get_next_sibling(row);
	GtkWidget *above = below ? gtk_widget_get_prev_sibling(below) : row;
	clear_drop_line(window);
	if (is_dragged_row(window, below) || is_dragged_row(window, above))
		return GDK_ACTION_MOVE;
	window->drop_line_row = g_object_ref(below ? below : row);
	gtk_widget_add_css_class(window->drop_line_row, below ? "drop-before" : "drop-after");
	return GDK_ACTION_MOVE;
}


static void leave_rail_row(GtkDropTarget *target, Window *window) {
	clear_drop_line(window);
}


static void end_rail_drag(GtkDragSource *source, GdkDrag *drag, gboolean is_deleting, Window *window) {
	clear_drop_line(window);
	window->dragged_network = NULL;
}


/* Dropping a server lands it before or after the one under the pointer, and the new order is kept */
static gboolean drop_on_rail_row(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	clear_drop_line(window);
	CrimsonNetwork *destination = g_object_get_data(G_OBJECT(row), NETWORK_KEY);
	g_autoptr(GVariant) key = g_variant_ref_sink(g_variant_new_string(g_value_get_string(value)));
	CrimsonNetwork *moved = find_keyed_network(window, key);
	GListStore *networks = window->chat->networks;
	guint from = 0, to = 0;
	if (!moved || moved == destination || !g_list_store_find(networks, moved, &from)
		|| !g_list_store_find(networks, destination, &to))
		return FALSE;
	/* Taking the server out first moves every later position up by one */
	guint position = to + (is_drop_before(row, y) ? 0 : 1);
	if (from < position)
		position--;
	g_object_ref(moved);
	g_list_store_remove(networks, from);
	g_list_store_insert(networks, position, moved);
	g_object_unref(moved);
	save_network_order(G_LIST_MODEL(networks));
	select_rail_row(window, window->network);
	return TRUE;
}


static GtkWidget *create_rail_row(gpointer item, gpointer data) {
	CrimsonNetwork *network = item;
	GtkWidget *row = gtk_list_box_row_new();
	gtk_widget_add_css_class(row, "rail-row");
	GtkWidget *overlay = gtk_overlay_new();
	gtk_widget_add_css_class(overlay, "rail-picture");
	gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
	GtkWidget *avatar = adw_avatar_new(RAIL_AVATAR_SIZE, NULL, TRUE);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), avatar);
	/* The badge sits on a slightly larger circle in the rail's colour, which parts it from the picture */
	GtkWidget *badge = gtk_label_new(NULL);
	gtk_widget_add_css_class(badge, "unread-badge");
	GtkWidget *ring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(ring, "rail-badge");
	gtk_widget_set_halign(ring, GTK_ALIGN_END);
	gtk_widget_set_valign(ring, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(ring), badge);
	g_object_bind_property(badge, "visible", ring, "visible", G_BINDING_SYNC_CREATE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ring);
	GtkWidget *state = gtk_stack_new();
	show_after_wait(state, G_CALLBACK(dim_rail_avatar), avatar);
	GtkWidget *spinner = adw_spinner_new();
	gtk_widget_set_size_request(spinner, RAIL_SPINNER_SIZE, RAIL_SPINNER_SIZE);
	gtk_stack_add_named(GTK_STACK(state), spinner, "waiting");
	gtk_stack_add_named(GTK_STACK(state), gtk_image_new_from_icon_name("dialog-warning-symbolic"), "problem");
	gtk_widget_set_halign(state, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(state, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(state, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), overlay);
	g_object_set_data(G_OBJECT(row), "state", state);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "badge", badge);
	g_object_set_data(G_OBJECT(row), NETWORK_KEY, network);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_rail_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	GtkDragSource *drag = gtk_drag_source_new();
	gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
	g_signal_connect(drag, "prepare", G_CALLBACK(start_rail_drag), data);
	g_signal_connect(drag, "drag-end", G_CALLBACK(end_rail_drag), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drag));
	GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect(drop, "drop", G_CALLBACK(drop_on_rail_row), data);
	g_signal_connect(drop, "enter", G_CALLBACK(follow_rail_drag), data);
	g_signal_connect(drop, "motion", G_CALLBACK(follow_rail_drag), data);
	g_signal_connect(drop, "leave", G_CALLBACK(leave_rail_row), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
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
	*was_connected = session->has_connected;
	if (session->record->is_bouncer && read_account_state(session) != NETWORK_CONNECTED)
		return describe_account_name(session);
	if (network->state == NETWORK_CONNECTED)
		return NULL;
	/* Behind a bouncer the link to it can be up long before this server is, which is no reconnection */
	*was_connected = network->has_connected;
	return network->name;
}


/* A link nothing is retrying shows as a problem, where one that retries by itself only shows the spinner */
static void read_connection_problem(Window *window) {
	window->is_problem = window->network && is_stuck(window->network);
}


/* Clicking the corner opens the server's messages, where what happened is written, and a stopped connection
 * offers the fix: the account for a refused login, the bouncer's settings for its own, or another try */
static void show_connection(Window *window) {
	CrimsonNetwork *network = window->network;
	g_autoptr(GVariant) target = network ? g_variant_ref_sink(g_variant_new_string(network->key)) : NULL;
	ConnectionNote note = {window->lost_message, window->is_problem, target ? "win.server-messages" : NULL, target,
		NULL, NULL, NULL};
	if (window->is_problem && network) {
		Session *session = network->session;
		gboolean is_bouncer_down = session->record->is_bouncer && read_account_state(session) != NETWORK_CONNECTED;
		StopKind kind = read_stop_kind(is_bouncer_down ? read_account_error(session) : read_network_error(network));
		if (kind == STOP_LOGIN && is_bouncer_down) {
			note.fix_label = TEXT_LOG_IN;
			note.fix_action = "win.bouncer-settings";
		} else if (kind == STOP_LOGIN) {
			note.fix_label = TEXT_LOG_IN;
			note.fix_action = "win.own-profile";
			note.fix_target = target;
		} else if (kind == STOP_OTHER) {
			note.fix_label = TEXT_TRY_AGAIN;
			note.fix_action = "win.reconnect-server";
			note.fix_target = target;
		}
	}
	show_room_list_connection(window->room_list, &note);
}


static gboolean reveal_connection(gpointer data) {
	Window *window = data;
	window->connection_source = 0;
	window->is_connection_shown = TRUE;
	show_connection(window);
	return G_SOURCE_REMOVE;
}


/* A lost connection shows as a spinner until it is back, and coming back says so once */
static void refresh_connection(Window *window) {
	gboolean was_connected = FALSE;
	const char *lost = window->chat->has_started ? find_lost_connection(window, &was_connected) : NULL;
	gboolean is_same_server = window->lost_network == window->network;
	window->lost_network = lost ? window->network : NULL;
	if (!lost) {
		g_clear_handle_id(&window->connection_source, g_source_remove);
		if (window->is_connection_shown && window->lost_name && is_same_server) {
			g_autofree char *text = g_strdup_printf(TEXT_RECONNECTED_TO, window->lost_name);
			show_toast(window, text);
		}
		window->is_connection_shown = FALSE;
		show_room_list_connection(window->room_list, NULL);
		g_clear_pointer(&window->lost_name, g_free);
		g_clear_pointer(&window->lost_message, g_free);
		return;
	}
	read_connection_problem(window);
	g_free(window->lost_message);
	window->lost_message = describe_connection_wait(window->network);
	g_free(window->lost_name);
	window->lost_name = was_connected ? g_strdup(lost) : NULL;
	/* A problem shows at once, since nothing is coming to fix it, a server that was already down when chosen shows
	 * once its wait has lasted, and a fresh drop waits a little longer in case it comes straight back */
	if (window->is_connection_shown || window->is_problem) {
		g_clear_handle_id(&window->connection_source, g_source_remove);
		window->is_connection_shown = TRUE;
		show_connection(window);
	} else if (!window->connection_source)
		window->connection_source = g_timeout_add(is_same_server ? CONNECTION_DELAY_MS : WAIT_SHOWN_MS,
			reveal_connection, window);
}


static gboolean is_listed_network(Window *window, CrimsonNetwork *network) {
	return network && g_list_store_find(window->chat->networks, network, NULL);
}


/* Shows the welcome or the chats, the rail only with several servers, and keeps a server chosen */
static void show_network(Window *window, CrimsonNetwork *network);


static CrimsonNetwork *find_wanted_network(Window *window) {
	if (!window->wanted_network)
		return NULL;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_str_equal(network->key, window->wanted_network) && is_listed_network(window, network))
			return g_steal_pointer(&network);
	}
	return NULL;
}


static void update_view(Window *window) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	gboolean has_chats = count > 0 || !window->chat->has_started;
	gtk_stack_set_visible_child_name(GTK_STACK(window->pages), has_chats ? "chats" : "welcome");
	refresh_welcome(window);
	/* The rail shows whenever there are several servers, in a narrow window as well */
	gboolean has_rail = count > 1;
	window->has_rail = has_rail;
	gtk_widget_set_visible(window->rail, has_rail);
	g_autoptr(CrimsonNetwork) wanted = find_wanted_network(window);
	if (wanted)
		choose_network(window, wanted);
	else if (!is_listed_network(window, window->network)) {
		g_autoptr(CrimsonNetwork) first = count ? g_list_model_get_item(G_LIST_MODEL(window->chat->networks), 0) : NULL;
		show_network(window, first);
	}
	refresh_room_list_header(window->room_list);
	refresh_connection(window);
}


/* The tray icon carries a red dot while a badged room holds what the Badges settings ask the tray to show */
static void refresh_tray_attention(Window *window) {
	if (!window->tray)
		return;
	TrayBadge tray_badge = get_settings()->tray_badge;
	gboolean wants_attention = FALSE;
	guint count = tray_badge == TRAY_BADGE_NOTHING ? 0 : g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint index = 0; index < count && !wants_attention; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		gboolean has_mention = FALSE;
		guint unread = count_network_badges(network, &has_mention);
		wants_attention = has_mention || (tray_badge == TRAY_BADGE_ANY && unread);
	}
	set_tray_attention(window->tray, wants_attention);
}


/* A login is saved for the server, so not being logged in is a failed login rather than a missing account */
static gboolean has_saved_login(CrimsonNetwork *network) {
	const char *login = network->bouncer_id ? g_hash_table_lookup(network->attributes, "sasl-username")
		: network->session->record->sasl_username;
	return login && *login;
}


static gboolean show_banner_later(gpointer data) {
	Window *window = data;
	window->banner_source = 0;
	refresh_window_later(window);
	return G_SOURCE_REMOVE;
}


/* NickServ saying the nickname is registered offers to log in at once, while a server that would register the
 * nickname offers it a moment after connecting, once nothing asked for a login */
static AccountBanner pick_account_banner(Window *window) {
	CrimsonNetwork *network = window->network;
	if (!network || network->state != NETWORK_CONNECTED || is_logged_in(network))
		return ACCOUNT_BANNER_NONE;
	if (network->wants_login && !g_hash_table_contains(window->dismissed_logins, network->key))
		return ACCOUNT_BANNER_LOG_IN;
	if (network->wants_login || has_saved_login(network) || !can_register_account(network)
		|| is_registration_dismissed(network->key))
		return ACCOUNT_BANNER_NONE;
	gint64 waited = g_get_monotonic_time() - network->connected_since;
	if (waited < REGISTER_OFFER_DELAY_US) {
		if (!window->banner_source)
			window->banner_source = g_timeout_add((guint)((REGISTER_OFFER_DELAY_US - waited) / 1000) + 1,
				show_banner_later, window);
		return ACCOUNT_BANNER_NONE;
	}
	return ACCOUNT_BANNER_REGISTER;
}


static gboolean refresh_now(gpointer data) {
	Window *window = data;
	window->refresh_source = 0;
	refresh_tray_attention(window);
	refresh_rail(window);
	update_view(window);
	resort_room_list(window->room_list);
	refresh_conversation(window->conversation);
	if (window->network)
		build_rooms_ahead(window->conversation, window->network);
	show_room_list_banner(window->room_list, pick_account_banner(window), window->network ? window->network->key
		: NULL);
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


/* Shows a server without taking it as the user's choice, while the one from last time is still awaited */
static void show_network(Window *window, CrimsonNetwork *network) {
	window->network = network;
	select_rail_row(window, network);
	show_room_list_network(window->room_list, network);
	/* The corner's spinner and the dimming belong to the chosen server's own connection */
	refresh_window_later(window);
}


void choose_network(Window *window, CrimsonNetwork *network) {
	g_clear_pointer(&window->wanted_network, g_free);
	show_network(window, network);
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
		&& (!crimson_panes_get_collapsed(window->panes)
		|| crimson_panes_get_show_content(window->panes));
}


static void show_room(Window *window, CrimsonRoom *room, gboolean is_going_back) {
	if (room->network != window->network)
		choose_network(window, room->network);
	select_listed_room(window->room_list, room);
	show_conversation_room(window->conversation, room, is_going_back);
	crimson_panes_set_show_content(window->panes, TRUE);
	save_window_choice(room->network->key, room);
	focus_composer(window->conversation);
}


/* The chat being left is kept so going back returns to it, such as after a command opened the server's messages */
void open_room(Window *window, CrimsonRoom *room) {
	if (!room)
		return;
	CrimsonRoom *open = get_open_room(window);
	if (open && open != room) {
		if (window->room_history->len == ROOM_HISTORY_LIMIT)
			g_ptr_array_remove_index(window->room_history, 0);
		g_ptr_array_add(window->room_history, g_object_ref(open));
	}
	show_room(window, room, FALSE);
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


/* Right-clicking the rail beside its servers offers to add another */
static void open_rail_space_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	GtkWidget *list = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y))
		return;
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_ADD_SERVER, "app.add-server");
	GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
	gtk_widget_set_parent(popover, list);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_rail_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


static GtkWidget *create_rail(Window *window) {
	GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(rail, "rail");
	window->rail_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->rail_list, "rail-list");
	gtk_list_box_bind_model(GTK_LIST_BOX(window->rail_list), G_LIST_MODEL(window->chat->networks), create_rail_row,
		window, NULL);
	g_signal_connect(window->rail_list, "row-selected", G_CALLBACK(pick_rail_row), window);
	/* The list fills the rail, so its empty space below the servers takes a right click too */
	gtk_widget_set_vexpand(window->rail_list, TRUE);
	GtkGesture *space_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(space_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(space_click, "pressed", G_CALLBACK(open_rail_space_menu), window);
	gtk_widget_add_controller(window->rail_list, GTK_EVENT_CONTROLLER(space_click));
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


/* Side by side, the list's header leaves the window buttons to the chat's header at the window's right edge */
static void follow_collapse(CrimsonPanes *panes, GParamSpec *spec, Window *window) {
	window->is_narrow = crimson_panes_get_collapsed(panes);
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(get_room_list_header(window->room_list)),
		window->is_narrow);
	set_conversation_narrow(window->conversation, window->is_narrow);
	update_view(window);
}


void hide_window(Window *window) {
	if (window->tray && is_tray_shown(window->tray))
		gtk_widget_set_visible(GTK_WIDGET(window->window), FALSE);
	else
		gtk_window_minimize(GTK_WINDOW(window->window));
}


/* Quitting takes the window and tray icon away at once, while the connections close behind them */
void withdraw_window(Window *window) {
	window->is_quitting = TRUE;
	read_window_geometry(GTK_WINDOW(window->window), &window->geometry);
	save_window_geometry_state(&window->geometry);
	hide_closing_window(GTK_WINDOW(window->window));
	if (window->tray)
		remove_tray(window->tray);
}


/* Closing keeps the chats connected in the tray, and otherwise quits, which never destroys the window */
static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	if (window->is_quitting)
		return TRUE;
	gboolean has_tray = window->tray && is_tray_shown(window->tray);
	if (!get_settings()->closes_to_tray || !has_tray) {
		g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", NULL);
		return TRUE;
	}
	read_window_geometry(gtk_window, &window->geometry);
	save_window_geometry_state(&window->geometry);
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


/* The same, for a server named by its key, which becomes the chosen server first */
static void join_room_on_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network) {
		choose_network(data, network);
		show_room_picker(data, network);
	}
}


static void message_on_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network) {
		choose_network(data, network);
		show_message_dialog(data, network);
	}
}


static void choose_server_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		choose_network(data, network);
}


static void register_account_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		show_registration(data, network);
}


static void dismiss_register_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	save_registration_dismissed(g_variant_get_string(parameter, NULL));
	refresh_window_later(data);
}


static void dismiss_login_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_hash_table_add(window->dismissed_logins, g_strdup(g_variant_get_string(parameter, NULL)));
	refresh_window_later(window);
}


static void own_profile_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CrimsonNetwork *network = find_keyed_network(data, parameter);
	if (network)
		show_own_profile(data, network);
}


/* The server room holds what the network itself says, kept out of the chat list */
static void server_messages_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = find_keyed_network(window, parameter);
	if (!network)
		return;
	network->server_room->is_listed = TRUE;
	save_chat_cache_later(window->chat);
	resort_room_list(window->room_list);
	open_room(window, network->server_room);
}


/* The server's messages are only taken off the list, to come back the next time they are opened */
static void hide_server_messages_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = find_keyed_network(window, parameter);
	if (!network)
		return;
	if (get_open_room(window) == network->server_room)
		close_open_room(window);
	network->server_room->is_listed = FALSE;
	save_chat_cache_later(window->chat);
	resort_room_list(window->room_list);
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


static void leave_room_now(Window *window, CrimsonRoom *room) {
	if (get_open_room(window) == room) {
		show_conversation_room(window->conversation, NULL, FALSE);
		crimson_panes_set_show_content(window->panes, FALSE);
	}
	leave_room(room);
}


/* The room is found again by its server and name once the user answers, since it may have gone meanwhile */
static void finish_leave_question(AdwAlertDialog *alert, const char *response, Window *window) {
	if (g_strcmp0(response, "leave") != 0)
		return;
	GVariant *network_key = g_object_get_data(G_OBJECT(alert), "network-key");
	CrimsonNetwork *network = find_keyed_network(window, network_key);
	CrimsonRoom *room = network ? find_room(network, g_object_get_data(G_OBJECT(alert), "room-name")) : NULL;
	if (room)
		leave_room_now(window, room);
}


/* Leaving a room or deleting a chat cannot be undone, so it is asked first */
static void ask_to_leave_room(Window *window, CrimsonRoom *room) {
	gboolean is_channel = room->kind == ROOM_CHANNEL;
	AdwDialog *alert = adw_alert_dialog_new(is_channel ? TEXT_LEAVE_ROOM_TITLE : TEXT_DELETE_CHAT_TITLE, NULL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "leave",
		is_channel ? TEXT_LEAVE : TEXT_DELETE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "leave", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
	g_object_set_data_full(G_OBJECT(alert), "network-key",
		g_variant_ref_sink(g_variant_new_string(room->network->key)), (GDestroyNotify)g_variant_unref);
	g_object_set_data_full(G_OBJECT(alert), "room-name", g_strdup(room->name), g_free);
	g_signal_connect(alert, "response", G_CALLBACK(finish_leave_question), window);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void leave_room_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonRoom *room = get_open_room(window);
	if (room && room->kind != ROOM_SERVER)
		ask_to_leave_room(window, room);
}


/* A private chat's person shows over the list as it is, while a room opens first, so its card can show the
 * members beside it */
static void chat_details_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *key = NULL, *name = NULL;
	g_variant_get(parameter, "(&s&s)", &key, &name);
	g_autoptr(GVariant) network_key = g_variant_ref_sink(g_variant_new_string(key));
	CrimsonNetwork *network = find_keyed_network(window, network_key);
	CrimsonRoom *room = network ? find_room(network, name) : NULL;
	if (!room)
		return;
	if (room->kind == ROOM_PRIVATE) {
		show_profile(window, room, room->name);
		return;
	}
	if (get_open_room(window) != room)
		open_room(window, room);
	show_conversation_details(window->conversation);
}


static void leave_chat_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *key = NULL, *name = NULL;
	g_variant_get(parameter, "(&s&s)", &key, &name);
	g_autoptr(GVariant) network_key = g_variant_ref_sink(g_variant_new_string(key));
	CrimsonNetwork *network = find_keyed_network(window, network_key);
	CrimsonRoom *room = network ? find_room(network, name) : NULL;
	if (room && room->kind != ROOM_SERVER)
		ask_to_leave_room(window, room);
}


static void mark_read_in_window(Window *window, CrimsonRoom *room) {
	mark_room_read(room);
	clear_read_line(window->conversation, room);
}


static void mark_read_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	const char *key = NULL, *name = NULL;
	g_variant_get(parameter, "(&s&s)", &key, &name);
	g_autoptr(GVariant) network_key = g_variant_ref_sink(g_variant_new_string(key));
	CrimsonNetwork *network = find_keyed_network(window, network_key);
	CrimsonRoom *room = network ? find_room(network, name) : NULL;
	if (room)
		mark_read_in_window(window, room);
}


static void mark_server_read_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	CrimsonNetwork *network = find_keyed_network(window, parameter);
	guint count = network ? g_list_model_get_n_items(G_LIST_MODEL(network->rooms)) : 0;
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		mark_read_in_window(window, room);
	}
}


static void search_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	crimson_panes_set_show_content(window->panes, FALSE);
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


/* A kept chat may have been left or its server removed since, so it is looked for among the servers that remain
 * before its own server is trusted */
static gboolean is_room_kept(Window *window, CrimsonRoom *room) {
	GListModel *networks = G_LIST_MODEL(window->chat->networks);
	for (guint index = 0; index < g_list_model_get_n_items(networks); index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(networks, index);
		if (g_list_store_find(network->rooms, room, NULL))
			return TRUE;
	}
	return FALSE;
}


/* Going back returns to the chat open before this one, and closes the chat once there is none */
static void go_back(Window *window) {
	while (window->room_history->len > 0) {
		g_autoptr(CrimsonRoom) room = g_ptr_array_steal_index(window->room_history, window->room_history->len - 1);
		if (room != get_open_room(window) && is_room_kept(window, room)) {
			show_room(window, room, TRUE);
			return;
		}
	}
	close_open_room(window);
}


static void press_back_button(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	go_back(window);
}


static void go_back_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	go_back(data);
}


/* Back to the list starts over, so a chat opened from there goes back to the list again */
static void follow_shown_content(CrimsonPanes *panes, GParamSpec *spec, Window *window) {
	if (!crimson_panes_get_show_content(panes))
		g_ptr_array_set_size(window->room_history, 0);
}


static void add_window_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"join-room", join_room_action, NULL, NULL, NULL, {0}},
		{"message-someone", message_someone_action, NULL, NULL, NULL, {0}},
		{"join-room-on", join_room_on_action, "s", NULL, NULL, {0}},
		{"message-on", message_on_action, "s", NULL, NULL, {0}},
		{"bouncer-settings", bouncer_settings_action, NULL, NULL, NULL, {0}},
		{"disconnect-bouncer", disconnect_bouncer_action, NULL, NULL, NULL, {0}},
		{"bouncer-admin", bouncer_admin_action, NULL, NULL, NULL, {0}},
		{"choose-server", choose_server_action, "s", NULL, NULL, {0}},
		{"own-profile", own_profile_action, "s", NULL, NULL, {0}},
		{"register-account", register_account_action, "s", NULL, NULL, {0}},
		{"dismiss-register", dismiss_register_action, "s", NULL, NULL, {0}},
		{"dismiss-login", dismiss_login_action, "s", NULL, NULL, {0}},
		{"server-messages", server_messages_action, "s", NULL, NULL, {0}},
		{"hide-server-messages", hide_server_messages_action, "s", NULL, NULL, {0}},
		{"network-settings", network_settings_action, "s", NULL, NULL, {0}},
		{"reconnect-server", reconnect_server_action, "s", NULL, NULL, {0}},
		{"remove-network", remove_network_action, "s", NULL, NULL, {0}},
		{"leave-room", leave_room_action, NULL, NULL, NULL, {0}},
		{"leave-chat", leave_chat_action, "(ss)", NULL, NULL, {0}},
		{"chat-details", chat_details_action, "(ss)", NULL, NULL, {0}},
		{"mark-read", mark_read_action, "(ss)", NULL, NULL, {0}},
		{"mark-server-read", mark_server_read_action, "s", NULL, NULL, {0}},
		{"search", search_action, NULL, NULL, NULL, {0}},
		{"step-room", step_room_action, "i", NULL, NULL, {0}},
		{"hide", hide_action, NULL, NULL, NULL, {0}},
		{"invite-people", invite_action, NULL, NULL, NULL, {0}},
		{"go-back", go_back_action, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(window->window), ENTRIES, G_N_ELEMENTS(ENTRIES), window);
}


/* One header spans the rail and the list, so its title sits in the middle of the whole side panel */
static GtkWidget *create_sidebar(Window *window) {
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(panel), get_room_list_header(window->room_list));
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(box, TRUE);
	window->rail = create_rail(window);
	gtk_box_append(GTK_BOX(box), window->rail);
	GtkWidget *list = get_room_list_widget(window->room_list);
	gtk_widget_set_hexpand(list, TRUE);
	gtk_box_append(GTK_BOX(box), list);
	gtk_box_append(GTK_BOX(panel), box);
	return panel;
}


void close_open_room(Window *window) {
	show_conversation_room(window->conversation, NULL, FALSE);
	select_listed_room(window->room_list, NULL);
	crimson_panes_set_show_content(window->panes, FALSE);
	save_window_choice(window->network ? window->network->key : NULL, NULL);
}


static void add_breakpoint(Window *window) {
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_CONDITION));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(window->panes), "collapsed", &collapsed);
	adw_application_window_add_breakpoint(window->window, breakpoint);
}


void restore_window_choice(Window *window) {
	update_view(window);
	/* The app starts on the server from last time, with no chat open */
	g_autofree char *network_key = NULL, *room_network = NULL, *room_name = NULL;
	load_window_choice(&network_key, &room_network, &room_name);
	window->wanted_network = network_key && *network_key ? g_steal_pointer(&network_key) : NULL;
	update_view(window);
}


typedef struct {
	GtkWidget *widget;
	guint source;
	GCallback shown;
	gpointer data;
} WaitShown;


static gboolean reveal_wait(gpointer data) {
	WaitShown *wait = data;
	wait->source = 0;
	gtk_widget_set_opacity(wait->widget, 1);
	if (wait->shown)
		((void (*)(gpointer))wait->shown)(wait->data);
	return G_SOURCE_REMOVE;
}


static void start_wait(GtkWidget *widget, WaitShown *wait) {
	gtk_widget_set_opacity(widget, 0);
	g_clear_handle_id(&wait->source, g_source_remove);
	wait->source = g_timeout_add(WAIT_SHOWN_MS, reveal_wait, wait);
}


static void stop_wait(GtkWidget *widget, WaitShown *wait) {
	g_clear_handle_id(&wait->source, g_source_remove);
	gtk_widget_set_opacity(widget, 0);
}


static void free_wait(gpointer data) {
	WaitShown *wait = data;
	g_clear_handle_id(&wait->source, g_source_remove);
	g_free(wait);
}


void show_after_wait(GtkWidget *widget, GCallback shown, gpointer data) {
	WaitShown *wait = g_new0(WaitShown, 1);
	*wait = (WaitShown){widget, 0, shown, data};
	gtk_widget_set_opacity(widget, 0);
	g_signal_connect(widget, "map", G_CALLBACK(start_wait), wait);
	g_signal_connect(widget, "unmap", G_CALLBACK(stop_wait), wait);
	g_object_set_data_full(G_OBJECT(widget), "wait-shown", wait, free_wait);
}


GtkWidget *create_spinner(void) {
	GtkWidget *spinner = adw_spinner_new();
	show_after_wait(spinner, NULL, NULL);
	return spinner;
}


static void remember_sidebar_width(int width, gpointer data) {
	save_sidebar_width(width);
}


Window *create_window(GtkApplication *application, Chat *chat) {
	Window *window = g_new0(Window, 1);
	main_window = window;
	window->dismissed_logins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	window->room_history = g_ptr_array_new_with_free_func(g_object_unref);
	window->chat = chat;
	window->window = ADW_APPLICATION_WINDOW(adw_application_window_new(application));
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	g_object_set_data(G_OBJECT(window->window), "window", window);
	load_window_geometry_state(&window->geometry);
	apply_window_geometry(GTK_WINDOW(window->window), &window->geometry);
	gtk_widget_set_size_request(GTK_WIDGET(window->window), MINIMUM_WIDTH, MINIMUM_HEIGHT);
	window->room_list = create_room_list(window);
	window->conversation = create_conversation(window);
	window->panes = CRIMSON_PANES(crimson_panes_new(create_sidebar(window),
		get_conversation_widget(window->conversation)));
	g_signal_connect(window->panes, "notify::collapsed", G_CALLBACK(follow_collapse), window);
	window->pages = gtk_stack_new();
	/* The side panel's edge drags it wider or narrower, and the width is kept for the next launch */
	GtkWidget *chats = add_panes_sidebar_handle(window->panes, load_sidebar_width(SIDEBAR_WIDTH),
		SIDEBAR_MINIMUM, SIDEBAR_MAXIMUM, remember_sidebar_width, NULL);
	gtk_stack_add_named(GTK_STACK(window->pages), chats, "chats");
	gtk_stack_add_named(GTK_STACK(window->pages), create_welcome(window), "welcome");
	g_signal_connect(chat->networks, "items-changed", G_CALLBACK(follow_networks), window);
	g_signal_connect(chat->accounts, "items-changed", G_CALLBACK(follow_accounts), window);
	window->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
	adw_toast_overlay_set_child(window->toasts, window->pages);
	adw_application_window_set_content(window->window, GTK_WIDGET(window->toasts));
	add_breakpoint(window);
	GtkGesture *back = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(back), MOUSE_BACK_BUTTON);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(back), GTK_PHASE_CAPTURE);
	g_signal_connect(back, "pressed", G_CALLBACK(press_back_button), window);
	gtk_widget_add_controller(GTK_WIDGET(window->window), GTK_EVENT_CONTROLLER(back));
	add_window_actions(window);
	g_signal_connect(window->panes, "notify::show-content", G_CALLBACK(follow_shown_content), window);
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	static const TrayEntry TRAY_ENTRIES[] = {
		{TEXT_SHOW_WINDOW, "app.show"},
		{NULL, NULL},
		{TEXT_QUIT_APPLICATION, "app.quit"},
	};
	TraySetup tray = {TRAY_ENTRIES, G_N_ELEMENTS(TRAY_ENTRIES), NULL, NULL};
	window->tray = create_tray(G_APPLICATION(application), GTK_WINDOW(window->window), &tray);
	follow_collapse(window->panes, NULL, window);
	note_first_frame(GTK_WIDGET(window->window));
	return window;
}
