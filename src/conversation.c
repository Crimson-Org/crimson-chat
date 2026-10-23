#include "../lang/en.h"
#include "settings.h"
#include "common/startup.h"
#include "composer.h"
#include "profile.h"
#include "formatting.h"
#include "images.h"
#include "message-row.h"
#include "notifications.h"
#include "room-look.h"
#include "sounds.h"
#include "window.h"
#include "pictures.h"
#include "shown-messages.h"

#define FADE_DURATION_MS 160
#define PAGE_SLIDE_MS 260
#define ACTIVITY_SECONDS 120
#define SECONDS_PER_MINUTE 60
#define HEADER_AVATAR_SIZE 38
#define MEMBER_AVATAR_SIZE 32
#define BOTTOM_SLACK 48.0
/* The list builds a row for each message it holds, so a room shows enough of its latest messages to fill the chat,
 * counting on rows at least this tall */
#define SHORTEST_ROW_HEIGHT 32
#define FILL_SPARE_ROWS 4
/* Each row added above the others makes the list go over all of them again, so older messages come in a screenful
 * at a time once scrolling has paused this long within this many screens of the top */
#define REVEAL_PAUSE_MS 150
#define REVEAL_MARGIN_PAGES 2
/* The server is asked for older messages once fewer than this wait hidden above */
#define PREFETCH_HIDDEN 40
/* How many rooms keep their built chat, the ones opened last and those built ahead of time */
#define KEPT_VIEWS 16
#define CONTEXT_BEFORE_TARGET 10
#define LOADING_SPINNER_SIZE 32
#define MEMBERS_WIDTH 260
/* The member list lies over the chat instead of beside it when the chat would keep less than this, and the chat
 * column itself never asks for more than the smallest room the window's own layout leaves it */
#define DOCKED_MEMBERS_CONDITION "max-width: 620px"
#define CHAT_MINIMUM_WIDTH 180
#define CHAT_MINIMUM_HEIGHT 300
#define MESSAGE_KEY "message"
#define HANDLER_KEY "changed-handler"
#define VIEW_KEY "room-view"
/* How far below the top of the chat the avatar of the person being read stays */
#define STICKY_AVATAR_MARGIN 8
/* How far in from the side the avatars of who types in a busy room sit, how many show and how far each lies over
 * the one before, how much of them rises into view and how fast they rise */
#define TYPING_FACES_MARGIN 16
#define TYPING_FACES_COUNT 3
#define TYPING_FACES_OVERLAP 12
#define TYPING_FACES_SHOWN 0.5
/* Room above the avatars for their hop and its shadow, since the overlay clips them to their own box */
#define TYPING_HOP_ROOM 16
#define NICK_KEY "nick"
#define TYPING_RISE_MS 340
/* Rooms with more people than the user and one other show typing with avatars rather than only the header */
#define TYPING_FACES_MEMBERS 3

/* One room's messages as the chat shows them, kept built while the room was opened lately or built ahead of time,
 * so showing the room again only switches to it */
typedef struct {
	Conversation *conversation;
	CrimsonRoom *room;
	GtkWidget *scroller, *list_view;
	/* The room's latest messages, and those without the joins and leaves the setting hides, which are kept */
	CrimsonShownMessages *recent_messages;
	GtkFilterListModel *shown_messages;
	GtkNoSelection *messages;
	/* The view follows the newest message until the user scrolls away from it */
	gboolean is_pinned, has_user_scrolled;
	gulong layout_handler;
	guint reveal_source;
	/* The last moment read when new messages started coming in unseen, which a line in the chat marks until the
	 * user leaves the room */
	gint64 read_line;
	gboolean has_read_line, was_unread;
	/* When the room was last shown, so the view shown longest ago is let go first */
	gint64 shown_time;
} RoomView;

struct Conversation {
	Window *window;
	GtkWidget *stack, *split, *avatar, *title, *subtitle, *members_button;
	/* A still image of the chat a narrow window is leaving, which slides away as the next chat slides in */
	GtkWidget *still_page;
	/* The button down to the latest message, which shows an arrow or how many new messages wait below */
	GtkWidget *jump_button, *jump_icons, *jump_count;
	/* The kept rooms' chats, of which the open room's shows, and the rooms waiting to be built ahead of time */
	GtkWidget *views_stack;
	GHashTable *views;
	RoomView *view;
	GQueue waiting_builds;
	guint build_source, read_source;
	/* A narrow window slides whole pages itself, so the chat view does not fade on top of that */
	gboolean is_narrow;
	GtkWidget *member_view, *empty_note, *loading_spinner, *members_title;
	GtkStringList *members;
	gboolean shows_events;
	Composer *composer;
	CrimsonRoom *room;
	gulong room_handler;
	/* Whether the user opened the member list, which the split view would otherwise change when it resizes */
	gboolean wants_members;
	guint member_source;
	/* Asks the server every couple of minutes when the person in a private chat was last active */
	guint activity_source;
	/* The avatar of the person whose messages are read at the top, kept in view while the first row of their group
	 * has scrolled away, with that row's own avatar hidden so the two never show at once */
	GtkWidget *message_overlay, *sticky_avatar, *hidden_avatar;
	char *sticky_nick, *wanted_nick;
	guint sticky_source;
	/* The avatars of who types in a busy room, partly out of sight at the bottom and rising while they type */
	GtkWidget *typing_faces;
	AdwAnimation *typing_animation;
	double typing_rise;
	gboolean shows_typing_faces;
	MessageActions actions;
};


static gboolean is_shown_view(RoomView *view) {
	return view && view == view->conversation->view;
}


static GtkAdjustment *get_scroll(RoomView *view) {
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->scroller));
}


/* A pinned list is moved to its bottom right after each layout pass and before it is drawn, which anchors it to its
 * last row so it stays there as rows measure themselves. The list sets its scroll range inside its own layout pass,
 * where scrolling it would add rows that pass never measures and that then draw on top of each other */
static void keep_at_bottom(GdkFrameClock *clock, RoomView *view) {
	if (!view->is_pinned || !gtk_widget_get_mapped(view->list_view))
		return;
	GtkAdjustment *scroll = get_scroll(view);
	double bottom = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
	if (gtk_adjustment_get_value(scroll) < bottom)
		gtk_adjustment_set_value(scroll, bottom);
}


static void follow_frames(GtkWidget *list_view, RoomView *view) {
	view->layout_handler = g_signal_connect(gtk_widget_get_frame_clock(list_view), "layout",
		G_CALLBACK(keep_at_bottom), view);
}


static void forget_frames(GtkWidget *list_view, RoomView *view) {
	g_clear_signal_handler(&view->layout_handler, gtk_widget_get_frame_clock(list_view));
}


static gboolean mark_read_now(gpointer data) {
	Conversation *conversation = data;
	conversation->read_source = 0;
	if (conversation->view && conversation->view->is_pinned
		&& is_room_in_view(conversation->window, conversation->room))
		mark_room_read(conversation->room);
	return G_SOURCE_REMOVE;
}


/* Marking read tells the server and redraws the room's badge, so it waits until the frame is drawn */
static void mark_read_when_seen(Conversation *conversation) {
	if (!conversation->read_source)
		conversation->read_source = g_idle_add(mark_read_now, conversation);
}


static void pin_to_bottom(RoomView *view) {
	view->is_pinned = TRUE;
	view->has_user_scrolled = FALSE;
	if (is_shown_view(view))
		gtk_widget_set_visible(view->conversation->jump_button, FALSE);
	gtk_widget_queue_allocate(view->list_view);
}


/* More of the room may still come while it waits for its server, while its history has not been asked for or is
 * on the way, while messages wait above the shown ones, and while older pages may hold more than hidden joins */
static gboolean is_room_loading(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	if (!room || room->kind == ROOM_SERVER || !conversation->view)
		return FALSE;
	if (room->network->state != NETWORK_CONNECTED)
		return TRUE;
	if (!can_network(room->network, "draft/chathistory"))
		return FALSE;
	CrimsonShownMessages *shown = conversation->view->recent_messages;
	gboolean has_held = g_list_model_get_n_items(G_LIST_MODEL(shown)) > 0;
	return room->is_loading_history || !room->has_history || count_hidden_messages(shown)
		|| (has_held && !room->has_all_history);
}


/* A chat with nothing to show says it is empty only once it knows, and shows a spinner while it may still load */
static void show_empty_note(Conversation *conversation) {
	RoomView *view = conversation->view;
	gboolean is_empty = view && g_list_model_get_n_items(G_LIST_MODEL(view->messages)) == 0;
	gboolean is_loading = is_room_loading(conversation);
	gtk_widget_set_visible(conversation->empty_note, is_empty && !is_loading);
	gtk_widget_set_visible(conversation->loading_spinner, is_empty && is_loading);
}


/* The earliest shown row may lose its day line or start of a group to the messages coming in above it */
static void reveal_before_shown(RoomView *view, guint position) {
	g_autoptr(CrimsonMessage) first = g_list_model_get_item(G_LIST_MODEL(view->messages), 0);
	reveal_messages_from(view->recent_messages, position);
	if (first)
		announce_message_change(first);
	if (is_shown_view(view))
		show_empty_note(view->conversation);
}


static gboolean is_near_top(RoomView *view) {
	GtkAdjustment *scroll = get_scroll(view);
	return gtk_adjustment_get_value(scroll) < gtk_adjustment_get_page_size(scroll) * REVEAL_MARGIN_PAGES;
}


/* Enough of the latest messages to fill the chat */
static guint count_filling_messages(Conversation *conversation) {
	int height = gtk_widget_get_height(conversation->views_stack);
	if (height <= 0)
		height = gtk_widget_get_height(GTK_WIDGET(conversation->window->window));
	return (guint)MAX(height, 0) / SHORTEST_ROW_HEIGHT + FILL_SPARE_ROWS;
}


static void reveal_when_near_top(RoomView *view);


/* The server is asked for more before the messages in memory run out, and the next screenful follows another pause
 * while the top is still near, since hidden joins and leaves may have filled this one */
static gboolean reveal_after_pause(gpointer data) {
	RoomView *view = data;
	view->reveal_source = 0;
	if (!is_shown_view(view) || !is_near_top(view))
		return G_SOURCE_REMOVE;
	guint hidden = count_hidden_messages(view->recent_messages);
	if (hidden < PREFETCH_HIDDEN)
		load_older_history(view->room);
	guint page = count_filling_messages(view->conversation);
	if (hidden)
		reveal_before_shown(view, hidden > page ? hidden - page : 0);
	if (count_hidden_messages(view->recent_messages))
		reveal_when_near_top(view);
	return G_SOURCE_REMOVE;
}


/* Each movement starts the pause again, and the list growing above starts it for the next screenful */
static void reveal_when_near_top(RoomView *view) {
	if (!view)
		return;
	g_clear_handle_id(&view->reveal_source, g_source_remove);
	if (is_shown_view(view) && is_near_top(view))
		view->reveal_source = g_timeout_add(REVEAL_PAUSE_MS, reveal_after_pause, view);
}


static void replace_sticky_avatar(Conversation *conversation);


/* Rows measure themselves as they appear and move the view, so only the user's own scrolling unpins it */
static void follow_scroll(GtkAdjustment *scroll, RoomView *view) {
	if (!is_shown_view(view))
		return;
	if (view->has_user_scrolled) {
		double bottom = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
		view->is_pinned = gtk_adjustment_get_value(scroll) >= bottom - BOTTOM_SLACK;
		gtk_widget_set_visible(view->conversation->jump_button, !view->is_pinned);
	}
	mark_read_when_seen(view->conversation);
	reveal_when_near_top(view);
	replace_sticky_avatar(view->conversation);
}


static gboolean note_user_scroll(GtkEventControllerScroll *controller, double x, double y, gpointer data) {
	RoomView *view = data;
	view->has_user_scrolled = TRUE;
	return FALSE;
}


static void reach_edge(GtkScrolledWindow *scroller, GtkPositionType position, RoomView *view) {
	if (position == GTK_POS_TOP)
		reveal_when_near_top(view);
	else if (position == GTK_POS_BOTTOM && view->has_user_scrolled)
		pin_to_bottom(view);
}


static void jump_down(GtkButton *button, Conversation *conversation) {
	if (conversation->view)
		pin_to_bottom(conversation->view);
}


/* The model keeps its messages, so the borrowed one stays valid while the list holds it */
static CrimsonMessage *get_message_at(RoomView *view, guint position) {
	CrimsonMessage *message = g_list_model_get_item(G_LIST_MODEL(view->messages), position);
	if (message)
		g_object_unref(message);
	return message;
}


/* The line needs something read above it, so a chat with no earlier activity shows none */
static gboolean starts_unread(RoomView *view, CrimsonMessage *message, CrimsonMessage *previous) {
	gint64 line = view->read_line;
	return view->has_read_line && previous && message->time > line && previous->time <= line;
}


static gboolean ends_group_at(RoomView *view, guint position, CrimsonMessage *message) {
	CrimsonMessage *next = get_message_at(view, position + 1);
	return !next || starts_message_group(next, message, starts_unread(view, next, message));
}


/* The first event of a run gathers the events below it, one per line */
static char *gather_notes(RoomView *view, guint position, CrimsonMessage *message) {
	GString *notes = g_string_new(NULL);
	CrimsonMessage *current = message;
	for (guint index = position;; index++) {
		g_autofree char *plain = strip_irc_formatting(current->text);
		g_string_append_printf(notes, "%s%s", notes->len ? "\n" : "", plain);
		CrimsonMessage *next = get_message_at(view, index + 1);
		if (!next || !continues_note_run(next, current, starts_unread(view, next, current)))
			break;
		current = next;
	}
	return g_string_free(notes, FALSE);
}


static void fill_row(RoomView *view, GtkListItem *item) {
	CrimsonMessage *message = gtk_list_item_get_item(item);
	guint position = gtk_list_item_get_position(item);
	CrimsonMessage *previous = position ? get_message_at(view, position - 1) : NULL;
	RowPlace place = {previous, starts_unread(view, message, previous), ends_group_at(view, position, message),
		NULL};
	g_autofree char *notes = NULL;
	if (message->kind == MESSAGE_EVENT && !continues_note_run(message, previous, place.starts_unread))
		notes = gather_notes(view, position, message);
	place.notes = notes;
	fill_message_row(gtk_list_item_get_child(item), message, &place, view->room, &view->conversation->actions);
}


static void refill_row(CrimsonMessage *message, gpointer item) {
	GtkWidget *row = gtk_list_item_get_child(item);
	fill_row(g_object_get_data(G_OBJECT(row), VIEW_KEY), item);
}


static void set_up_message(GtkSignalListItemFactory *factory, GtkListItem *item, RoomView *view) {
	GtkWidget *row = create_message_row();
	g_object_set_data(G_OBJECT(row), VIEW_KEY, view);
	gtk_list_item_set_child(item, row);
	gtk_list_item_set_activatable(item, FALSE);
	gtk_list_item_set_focusable(item, FALSE);
}


static void bind_message(GtkSignalListItemFactory *factory, GtkListItem *item, RoomView *view) {
	CrimsonMessage *message = gtk_list_item_get_item(item);
	GtkWidget *row = gtk_list_item_get_child(item);
	fill_row(view, item);
	gulong handler = g_signal_connect(message, "changed", G_CALLBACK(refill_row), item);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, GSIZE_TO_POINTER(handler));
	g_object_set_data(G_OBJECT(row), MESSAGE_KEY, message);
}


static void unbind_message(GtkSignalListItemFactory *factory, GtkListItem *item, RoomView *view) {
	GtkWidget *row = gtk_list_item_get_child(item);
	CrimsonMessage *message = g_object_get_data(G_OBJECT(row), MESSAGE_KEY);
	gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), HANDLER_KEY));
	if (message && handler)
		g_signal_handler_disconnect(message, handler);
	g_object_set_data(G_OBJECT(row), MESSAGE_KEY, NULL);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, NULL);
}


static void reply_to(CrimsonMessage *message, gpointer data) {
	Conversation *conversation = data;
	reply_in_composer(conversation->composer, message);
}


static void react_to(CrimsonMessage *message, const char *reaction, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		send_reaction(conversation->room, message, reaction);
}


static void remove_message(CrimsonMessage *message, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		redact_message(conversation->room, message);
}


static gboolean find_shown_position(RoomView *view, CrimsonMessage *target, guint *position) {
	GListModel *shown = G_LIST_MODEL(view->messages);
	for (guint index = g_list_model_get_n_items(shown); index > 0; index--) {
		g_autoptr(CrimsonMessage) message = g_list_model_get_item(shown, index - 1);
		if (message == target) {
			*position = index - 1;
			return TRUE;
		}
	}
	return FALSE;
}


static void show_message(const char *message_id, gpointer data) {
	Conversation *conversation = data;
	RoomView *view = conversation->view;
	CrimsonMessage *target = view ? find_room_message(view->room, message_id) : NULL;
	guint position = 0;
	/* A message older than the ones held comes into the list first, with a few before it for context */
	guint index = 0;
	if (target && g_list_store_find(view->room->messages, target, &index))
		reveal_before_shown(view, index > CONTEXT_BEFORE_TARGET ? index - CONTEXT_BEFORE_TARGET : 0);
	if (target && find_shown_position(view, target, &position)) {
		view->is_pinned = FALSE;
		view->has_user_scrolled = TRUE;
		gtk_widget_set_visible(conversation->jump_button, TRUE);
		gtk_list_view_scroll_to(GTK_LIST_VIEW(view->list_view), position, GTK_LIST_SCROLL_NONE, NULL);
	}
}


static void open_person(const char *nick, gpointer data) {
	Conversation *conversation = data;
	if (!conversation->room || !nick)
		return;
	CrimsonRoom *room = open_private_room(conversation->room->network, nick);
	open_room(conversation->window, room);
}


/* The rows on screen from top to bottom with their places in the chat area */
typedef struct {
	GtkWidget *row;
	graphene_rect_t bounds;
} PlacedRow;


static int compare_placed_rows(gconstpointer first, gconstpointer second) {
	float difference = ((const PlacedRow *)first)->bounds.origin.y - ((const PlacedRow *)second)->bounds.origin.y;
	return (difference > 0) - (difference < 0);
}


static GArray *list_placed_rows(Conversation *conversation) {
	GArray *rows = g_array_new(FALSE, FALSE, sizeof(PlacedRow));
	GtkWidget *list_view = conversation->view ? conversation->view->list_view : NULL;
	for (GtkWidget *item = list_view ? gtk_widget_get_first_child(list_view) : NULL; item;
		item = gtk_widget_get_next_sibling(item)) {
		GtkWidget *row = gtk_widget_get_first_child(item);
		PlacedRow placed = {row, {{0, 0}, {0, 0}}};
		if (!row || !gtk_widget_get_child_visible(item) || !g_object_get_data(G_OBJECT(row), MESSAGE_KEY)
			|| !gtk_widget_compute_bounds(row, conversation->message_overlay, &placed.bounds))
			continue;
		g_array_append_val(rows, placed);
	}
	g_array_sort(rows, compare_placed_rows);
	return rows;
}


static void show_hidden_avatar(Conversation *conversation, GtkWidget *avatar) {
	if (conversation->hidden_avatar == avatar)
		return;
	if (conversation->hidden_avatar)
		gtk_widget_set_opacity(conversation->hidden_avatar, 1);
	g_set_weak_pointer(&conversation->hidden_avatar, avatar);
	if (avatar)
		gtk_widget_set_opacity(avatar, 0);
}


/* Changing the picture inside the layout pass is not allowed, so it changes right after and the layout runs again */
static gboolean change_sticky_person(gpointer data) {
	Conversation *conversation = data;
	conversation->sticky_source = 0;
	g_free(conversation->sticky_nick);
	conversation->sticky_nick = g_strdup(conversation->wanted_nick);
	AdwAvatar *avatar = ADW_AVATAR(conversation->sticky_avatar);
	adw_avatar_set_text(avatar, conversation->sticky_nick);
	gtk_widget_set_tooltip_text(conversation->sticky_avatar, conversation->sticky_nick);
	const char *url = conversation->room && conversation->sticky_nick
		? pick_person_picture(conversation->room, conversation->sticky_nick) : NULL;
	g_autofree char *sized = fill_image_size(url, MESSAGE_AVATAR_SIZE * 2);
	show_avatar_image(avatar, sized);
	gtk_widget_queue_allocate(conversation->message_overlay);
	return G_SOURCE_REMOVE;
}


static void hide_sticky_avatar(Conversation *conversation) {
	gtk_widget_set_opacity(conversation->sticky_avatar, 0);
	gtk_widget_set_can_target(conversation->sticky_avatar, FALSE);
	show_hidden_avatar(conversation, NULL);
}


/* The row crossing the top margin belongs to a group, and its avatar follows the top down to the group's last row */
static void place_sticky_avatar(Conversation *conversation, GArray *rows, GdkRectangle *allocation) {
	guint top = 0;
	while (top < rows->len) {
		graphene_rect_t *bounds = &g_array_index(rows, PlacedRow, top).bounds;
		if (bounds->origin.y + bounds->size.height > STICKY_AVATAR_MARGIN)
			break;
		top++;
	}
	GtkWidget *row = top < rows->len ? g_array_index(rows, PlacedRow, top).row : NULL;
	GtkWidget *slot = row ? find_avatar_slot(row) : NULL;
	graphene_rect_t slot_bounds;
	if (!slot || !gtk_widget_compute_bounds(slot, conversation->message_overlay, &slot_bounds)) {
		hide_sticky_avatar(conversation);
		return;
	}
	/* A group's first row still showing its avatar in full needs no stand in */
	gboolean is_start = is_group_start(row);
	if (is_start && slot_bounds.origin.y >= STICKY_AVATAR_MARGIN) {
		hide_sticky_avatar(conversation);
		return;
	}
	guint last = top;
	while (last + 1 < rows->len && find_avatar_slot(g_array_index(rows, PlacedRow, last + 1).row)
		&& !is_group_start(g_array_index(rows, PlacedRow, last + 1).row))
		last++;
	graphene_rect_t *end = &g_array_index(rows, PlacedRow, last).bounds;
	float bottom = end->origin.y + end->size.height;
	allocation->x = (int)slot_bounds.origin.x;
	allocation->y = (int)MIN(STICKY_AVATAR_MARGIN, bottom - MESSAGE_AVATAR_SIZE);
	CrimsonMessage *message = g_object_get_data(G_OBJECT(row), MESSAGE_KEY);
	if (g_strcmp0(message->sender, conversation->sticky_nick) != 0) {
		g_free(conversation->wanted_nick);
		conversation->wanted_nick = g_strdup(message->sender);
		if (!conversation->sticky_source)
			conversation->sticky_source = g_idle_add(change_sticky_person, conversation);
		hide_sticky_avatar(conversation);
		return;
	}
	gtk_widget_set_opacity(conversation->sticky_avatar, 1);
	gtk_widget_set_can_target(conversation->sticky_avatar, TRUE);
	show_hidden_avatar(conversation, is_start ? slot : NULL);
}


/* Most of the avatars show once risen, and the rest stays below the edge of the chat */
static void place_typing_faces(Conversation *conversation, GdkRectangle *allocation) {
	GtkRequisition size;
	gtk_widget_get_preferred_size(conversation->typing_faces, NULL, &size);
	int height = gtk_widget_get_height(conversation->message_overlay);
	int risen = (int)((TYPING_HOP_ROOM + MESSAGE_AVATAR_SIZE * TYPING_FACES_SHOWN) * conversation->typing_rise);
	*allocation = (GdkRectangle){TYPING_FACES_MARGIN, height - risen, size.width, size.height};
}


static gboolean position_overlay_child(GtkOverlay *overlay, GtkWidget *widget, GdkRectangle *allocation,
	Conversation *conversation) {
	if (widget == conversation->typing_faces) {
		place_typing_faces(conversation, allocation);
		return TRUE;
	}
	if (widget != conversation->sticky_avatar)
		return FALSE;
	*allocation = (GdkRectangle){0, 0, MESSAGE_AVATAR_SIZE, MESSAGE_AVATAR_SIZE};
	g_autoptr(GArray) rows = list_placed_rows(conversation);
	place_sticky_avatar(conversation, rows, allocation);
	return TRUE;
}


/* The rows only take their new places when the list is laid out, so the avatar is placed in the same pass */
static void replace_sticky_avatar(Conversation *conversation) {
	gtk_widget_queue_allocate(conversation->message_overlay);
}


static void rise_typing_faces(double value, gpointer data) {
	Conversation *conversation = data;
	conversation->typing_rise = value;
	gtk_widget_queue_allocate(conversation->message_overlay);
}


static void create_typing_faces(Conversation *conversation, GtkWidget *overlay) {
	conversation->typing_faces = gtk_fixed_new();
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->typing_faces);
	gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), conversation->typing_faces, TRUE);
	AdwAnimationTarget *target = adw_callback_animation_target_new(rise_typing_faces, conversation, NULL);
	conversation->typing_animation = adw_timed_animation_new(conversation->typing_faces, 0, 1, TYPING_RISE_MS,
		target);
}


static void click_sticky_avatar(GtkGestureClick *gesture, int presses, double x, double y,
	Conversation *conversation) {
	if (conversation->sticky_nick)
		conversation->actions.show_person(conversation->sticky_nick, conversation->actions.data);
}


/* Someone else's message arriving in the open chat makes a short sound when it would notify, with or without the
 * window in focus, while the messages already there when a chat opens make none */
static void sound_new_message(GListModel *messages, guint position, guint removed, guint added, RoomView *view) {
	guint count = g_list_model_get_n_items(messages);
	if (!is_shown_view(view) || !added || position + added != count)
		return;
	g_autoptr(CrimsonMessage) message = g_list_model_get_item(messages, count - 1);
	gboolean is_spoken = message->kind != MESSAGE_EVENT && message->kind != MESSAGE_STATUS;
	if (is_message_arriving(message) && is_spoken && wants_notification(view->room, message))
		play_sound(SOUND_MESSAGE);
}


static gboolean is_message_shown(gpointer item, gpointer data) {
	Conversation *conversation = data;
	return !CRIMSON_MESSAGE(item)->is_membership || conversation->shows_events;
}


static GtkWidget *create_jump_button(Conversation *conversation) {
	conversation->jump_icons = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(conversation->jump_icons), gtk_image_new_from_icon_name("go-down-symbolic"),
		"arrow");
	conversation->jump_count = gtk_label_new(NULL);
	gtk_widget_add_css_class(conversation->jump_count, "unread-badge");
	gtk_stack_add_named(GTK_STACK(conversation->jump_icons), conversation->jump_count, "count");
	conversation->jump_button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(conversation->jump_button), conversation->jump_icons);
	gtk_widget_set_tooltip_text(conversation->jump_button, TEXT_JUMP_TO_LATEST);
	gtk_widget_add_css_class(conversation->jump_button, "circular");
	gtk_widget_add_css_class(conversation->jump_button, "osd");
	gtk_widget_add_css_class(conversation->jump_button, "jump-button");
	gtk_widget_set_halign(conversation->jump_button, GTK_ALIGN_END);
	gtk_widget_set_valign(conversation->jump_button, GTK_ALIGN_END);
	gtk_widget_set_visible(conversation->jump_button, FALSE);
	g_signal_connect(conversation->jump_button, "clicked", G_CALLBACK(jump_down), conversation);
	return conversation->jump_button;
}


static GtkWidget *create_centred_note(GtkWidget *note) {
	gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(note, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(note, FALSE);
	gtk_widget_set_visible(note, FALSE);
	return note;
}


/* The kept rooms' chats sit in one place under the floating avatars, notes and jump button they all share */
static GtkWidget *create_message_area(Conversation *conversation) {
	conversation->shows_events = get_settings()->shows_events;
	conversation->views = g_hash_table_new(NULL, NULL);
	g_queue_init(&conversation->waiting_builds);
	conversation->views_stack = gtk_stack_new();
	gtk_stack_set_hhomogeneous(GTK_STACK(conversation->views_stack), FALSE);
	gtk_stack_set_vhomogeneous(GTK_STACK(conversation->views_stack), FALSE);
	gtk_stack_set_transition_duration(GTK_STACK(conversation->views_stack), FADE_DURATION_MS);
	gtk_widget_set_vexpand(conversation->views_stack, TRUE);
	GtkWidget *overlay = gtk_overlay_new();
	conversation->message_overlay = overlay;
	gtk_overlay_set_child(GTK_OVERLAY(overlay), conversation->views_stack);
	conversation->sticky_avatar = adw_avatar_new(MESSAGE_AVATAR_SIZE, NULL, TRUE);
	gtk_widget_add_css_class(conversation->sticky_avatar, "message-avatar");
	gtk_widget_set_opacity(conversation->sticky_avatar, 0);
	gtk_widget_set_can_target(conversation->sticky_avatar, FALSE);
	gtk_widget_set_cursor_from_name(conversation->sticky_avatar, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(click_sticky_avatar), conversation);
	gtk_widget_add_controller(conversation->sticky_avatar, GTK_EVENT_CONTROLLER(click));
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->sticky_avatar);
	create_typing_faces(conversation, overlay);
	g_signal_connect(overlay, "get-child-position", G_CALLBACK(position_overlay_child), conversation);
	conversation->empty_note = gtk_label_new(TEXT_NO_MESSAGES);
	gtk_widget_add_css_class(conversation->empty_note, "empty-pill");
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_centred_note(conversation->empty_note));
	conversation->loading_spinner = create_spinner();
	gtk_widget_set_size_request(conversation->loading_spinner, LOADING_SPINNER_SIZE, LOADING_SPINNER_SIZE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_centred_note(conversation->loading_spinner));
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_jump_button(conversation));
	gtk_widget_add_css_class(overlay, "chat-background");
	return overlay;
}


static void follow_view_items(RoomView *view) {
	if (!is_shown_view(view))
		return;
	show_empty_note(view->conversation);
	replace_sticky_avatar(view->conversation);
}


static void follow_view_upper(RoomView *view) {
	if (!is_shown_view(view))
		return;
	replace_sticky_avatar(view->conversation);
	reveal_when_near_top(view);
}


/* A room's own list of rows, with the room's latest messages filling the chat */
static RoomView *create_room_view(Conversation *conversation, CrimsonRoom *room) {
	RoomView *view = g_new0(RoomView, 1);
	view->conversation = conversation;
	view->room = g_object_ref(room);
	view->is_pinned = TRUE;
	guint filling = count_filling_messages(conversation);
	view->recent_messages = create_shown_messages(G_LIST_MODEL(room->messages), filling, filling);
	GtkCustomFilter *filter = gtk_custom_filter_new(is_message_shown, conversation, NULL);
	view->shown_messages = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(view->recent_messages)),
		GTK_FILTER(filter));
	view->messages = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(view->shown_messages)));
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_message), view);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_message), view);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_message), view);
	view->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(view->messages)), factory);
	gtk_widget_add_css_class(view->list_view, "messages");
	view->scroller = gtk_scrolled_window_new();
	/* Never stopping sideways scrolling keeps the window from measuring the width of each row's text whenever its
	 * size is checked */
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->scroller), view->list_view);
	g_signal_connect(view->scroller, "edge-reached", G_CALLBACK(reach_edge), view);
	GtkAdjustment *scroll = get_scroll(view);
	g_signal_connect(scroll, "value-changed", G_CALLBACK(follow_scroll), view);
	g_signal_connect_swapped(scroll, "notify::upper", G_CALLBACK(follow_view_upper), view);
	g_signal_connect(view->list_view, "realize", G_CALLBACK(follow_frames), view);
	g_signal_connect(view->list_view, "unrealize", G_CALLBACK(forget_frames), view);
	GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(note_user_scroll), view);
	gtk_widget_add_controller(view->scroller, wheel);
	g_signal_connect_swapped(view->messages, "items-changed", G_CALLBACK(follow_view_items), view);
	g_signal_connect(view->messages, "items-changed", G_CALLBACK(sound_new_message), view);
	gtk_stack_add_child(GTK_STACK(conversation->views_stack), view->scroller);
	g_hash_table_insert(conversation->views, room, view);
	return view;
}


static void free_room_view(RoomView *view) {
	Conversation *conversation = view->conversation;
	g_hash_table_remove(conversation->views, view->room);
	g_clear_handle_id(&view->reveal_source, g_source_remove);
	g_signal_handlers_disconnect_by_data(get_scroll(view), view);
	g_signal_handlers_disconnect_by_data(view->messages, view);
	gtk_stack_remove(GTK_STACK(conversation->views_stack), view->scroller);
	g_object_unref(view->messages);
	g_object_unref(view->shown_messages);
	g_object_unref(view->recent_messages);
	g_object_unref(view->room);
	g_free(view);
}


/* Past the limit, the view shown longest ago is let go, but not the open one */
static void drop_old_views(Conversation *conversation) {
	while (g_hash_table_size(conversation->views) > KEPT_VIEWS) {
		RoomView *oldest = NULL;
		GHashTableIter iterator;
		gpointer room, value;
		g_hash_table_iter_init(&iterator, conversation->views);
		while (g_hash_table_iter_next(&iterator, &room, &value)) {
			RoomView *view = value;
			if (view != conversation->view && (!oldest || view->shown_time < oldest->shown_time))
				oldest = view;
		}
		if (!oldest)
			return;
		free_room_view(oldest);
	}
}


/* A view built ahead of time is also measured at the chat's width, which styles its rows and lays out their text,
 * so showing it later only places them */
static void measure_view(RoomView *view) {
	int width = gtk_widget_get_width(view->conversation->views_stack);
	if (width <= 0)
		width = gtk_widget_get_width(GTK_WIDGET(view->conversation->window->window));
	if (width > 0)
		gtk_widget_measure(view->list_view, GTK_ORIENTATION_VERTICAL, width, NULL, NULL, NULL, NULL);
}


/* One room is built per quiet moment, so building ahead holds up no frame for long */
static gboolean build_next_view(gpointer data) {
	Conversation *conversation = data;
	CrimsonRoom *room = g_queue_pop_head(&conversation->waiting_builds);
	if (!room) {
		conversation->build_source = 0;
		return G_SOURCE_REMOVE;
	}
	if (!g_hash_table_contains(conversation->views, room) && g_hash_table_size(conversation->views) < KEPT_VIEWS)
		measure_view(create_room_view(conversation, room));
	g_object_unref(room);
	return G_SOURCE_CONTINUE;
}


/* Rooms the user is likely to open next are built in quiet moments */
static void build_room_ahead(Conversation *conversation, CrimsonRoom *room) {
	if (g_hash_table_contains(conversation->views, room) || g_queue_find(&conversation->waiting_builds, room))
		return;
	g_queue_push_tail(&conversation->waiting_builds, g_object_ref(room));
	if (!conversation->build_source)
		conversation->build_source = g_idle_add_full(G_PRIORITY_LOW, build_next_view, conversation, NULL);
}


static const char *describe_role(char symbol) {
	switch (symbol) {
	case '~': return TEXT_ROLE_OWNER;
	case '&': return TEXT_ROLE_ADMIN;
	case '@': return TEXT_ROLE_OPERATOR;
	case '%': return TEXT_ROLE_HALF_OPERATOR;
	case '+': return TEXT_ROLE_VOICE;
	default: return "";
	}
}


/* Whether the user can moderate the open channel, as its operator or half-operator */
static gboolean can_moderate(Conversation *conversation, IrcState *state, IrcChannel *channel) {
	g_autofree char *own = fold_state_name(state, state->nick);
	const char *symbols = g_hash_table_lookup(channel->members, own);
	return symbols && strpbrk(symbols, "~&@%");
}


static void append_member_item(GMenu *menu, const char *label, const char *action, const char *nick) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(nick));
	g_menu_append_item(menu, item);
}


static GMenuModel *create_member_menu(Conversation *conversation, const char *nick) {
	GMenu *menu = g_menu_new();
	GMenu *person = g_menu_new();
	append_member_item(person, TEXT_VIEW_PROFILE, "member.profile", nick);
	append_member_item(person, TEXT_SEND_MESSAGE, "member.message", nick);
	append_member_item(person, TEXT_MENTION, "member.mention", nick);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(person));
	g_object_unref(person);
	IrcState *state = get_network_state(conversation->room->network);
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	if (!channel || !can_moderate(conversation, state, channel) || is_own_nick(state, nick))
		return G_MENU_MODEL(menu);
	g_autofree char *folded = fold_state_name(state, nick);
	const char *symbols = g_hash_table_lookup(channel->members, folded);
	gboolean is_operator = symbols && strchr(symbols, '@');
	gboolean has_voice = symbols && strchr(symbols, '+');
	GMenu *moderation = g_menu_new();
	append_member_item(moderation, is_operator ? TEXT_REMOVE_OPERATOR : TEXT_MAKE_OPERATOR,
		is_operator ? "member.deop" : "member.op", nick);
	append_member_item(moderation, has_voice ? TEXT_REMOVE_VOICE : TEXT_GIVE_VOICE,
		has_voice ? "member.devoice" : "member.voice", nick);
	append_member_item(moderation, TEXT_KICK, "member.kick", nick);
	append_member_item(moderation, TEXT_BAN, "member.ban", nick);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(moderation));
	g_object_unref(moderation);
	return G_MENU_MODEL(menu);
}


static void forget_member_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking a person offers what can be done with them, and moderation where the user may moderate */
static void show_person_menu(GtkWidget *anchor, const char *nick, double x, double y, gpointer data) {
	Conversation *conversation = data;
	if (!conversation->room || !nick || !*nick)
		return;
	g_autoptr(GMenuModel) model = create_member_menu(conversation, nick);
	GtkWidget *popover = gtk_popover_menu_new_from_model(model);
	gtk_widget_set_parent(popover, anchor);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_member_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


static void open_member_menu(GtkGestureClick *gesture, int presses, double x, double y, Conversation *conversation) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	show_person_menu(row, gtk_label_get_text(g_object_get_data(G_OBJECT(row), "name")), x, y, conversation);
}


static void show_person(const char *nick, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room && nick)
		show_profile(conversation->window, conversation->room, nick);
}


/* A click on a person in the member list opens their profile */
static void click_member(GtkGestureClick *gesture, int presses, double x, double y, Conversation *conversation) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	show_person(gtk_label_get_text(g_object_get_data(G_OBJECT(row), "name")), conversation);
}


static void set_up_member(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(row, "member-row");
	GtkWidget *avatar = adw_avatar_new(MEMBER_AVATAR_SIZE, NULL, TRUE);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(texts, TRUE);
	GtkWidget *name = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	GtkWidget *role = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(role), 0);
	gtk_widget_add_css_class(role, "member-role");
	gtk_box_append(GTK_BOX(texts), name);
	gtk_box_append(GTK_BOX(texts), role);
	gtk_box_append(GTK_BOX(row), avatar);
	gtk_box_append(GTK_BOX(row), texts);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "name", name);
	g_object_set_data(G_OBJECT(row), "role", role);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_member_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	GtkGesture *open = gtk_gesture_click_new();
	g_signal_connect(open, "released", G_CALLBACK(click_member), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(open));
	gtk_list_item_set_child(item, row);
}


static void bind_member(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	const char *nick = gtk_string_object_get_string(gtk_list_item_get_item(item));
	GtkWidget *row = gtk_list_item_get_child(item);
	IrcState *state = conversation->room ? get_network_state(conversation->room->network) : NULL;
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	IrcUser *user = state ? find_irc_user(state, nick) : NULL;
	g_autofree char *folded = state ? fold_state_name(state, nick) : NULL;
	const char *symbols = channel && folded ? g_hash_table_lookup(channel->members, folded) : NULL;
	adw_avatar_set_text(g_object_get_data(G_OBJECT(row), "avatar"), nick);
	const char *url = conversation->room ? pick_person_picture(conversation->room, nick) : NULL;
	g_autofree char *sized = fill_image_size(url, MEMBER_AVATAR_SIZE * 2);
	show_avatar_image(g_object_get_data(G_OBJECT(row), "avatar"), sized);
	gtk_label_set_text(g_object_get_data(G_OBJECT(row), "name"), nick);
	const char *role = describe_role(get_highest_prefix(symbols));
	gtk_label_set_text(g_object_get_data(G_OBJECT(row), "role"), role);
	gtk_widget_set_visible(g_object_get_data(G_OBJECT(row), "role"), *role != '\0');
	gtk_widget_set_opacity(row, user && user->is_away ? 0.5 : 1.0);
}


static void message_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	open_person(g_variant_get_string(parameter, NULL), data);
}


static void mention_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	mention_in_composer(conversation->composer, g_variant_get_string(parameter, NULL));
}


static void show_member_profile(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		show_profile(conversation->window, conversation->room, g_variant_get_string(parameter, NULL));
}


static void send_channel_mode(Conversation *conversation, const char *mode, const char *nick) {
	if (conversation->room && conversation->room->network->client)
		send_irc_client_command(conversation->room->network->client, "MODE", conversation->room->name, mode, nick,
			NULL);
}


/* The account-extban form of a ban, such as $a:name, where ACCOUNTEXTBAN names the type and EXTBAN its prefix */
static char *describe_account_ban(IrcState *state, const char *account) {
	const char *types = get_irc_support(state, "ACCOUNTEXTBAN");
	const char *extban = get_irc_support(state, "EXTBAN");
	if (!account || !*account || g_str_equal(account, "*") || !types || !*types)
		return NULL;
	g_autofree char *type = g_strndup(types, strcspn(types, ","));
	/* A prefix before the comma goes first, while a server without one takes the type alone */
	g_autofree char *prefix = extban && strchr(extban, ',') ? g_strndup(extban, strcspn(extban, ",")) : g_strdup("");
	return g_strdup_printf("%s%s:%s", prefix, type, account);
}


static void moderate_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	const char *name = g_action_get_name(G_ACTION(action));
	const char *nick = g_variant_get_string(parameter, NULL);
	CrimsonRoom *room = conversation->room;
	if (!room || !room->network->client)
		return;
	static const struct {
		const char *action, *mode;
	} MODES[] = {{"op", "+o"}, {"deop", "-o"}, {"voice", "+v"}, {"devoice", "-v"}};
	for (guint index = 0; index < G_N_ELEMENTS(MODES); index++) {
		if (g_str_equal(name, MODES[index].action))
			send_channel_mode(conversation, MODES[index].mode, nick);
	}
	/* A ban covers the person's account where the server bans by account, and otherwise their host, so a new
	 * nick or address does not get around it */
	if (g_str_equal(name, "ban")) {
		IrcState *state = get_network_state(room->network);
		IrcUser *user = state ? find_irc_user(state, nick) : NULL;
		g_autofree char *account_mask = user ? describe_account_ban(state, user->account) : NULL;
		g_autofree char *mask = account_mask ? g_steal_pointer(&account_mask) : user && user->host
			? g_strdup_printf("*!*@%s", user->host) : g_strdup_printf("%s!*@*", nick);
		send_channel_mode(conversation, "+b", mask);
	}
	if (g_str_equal(name, "kick") || g_str_equal(name, "ban"))
		send_irc_client_command(room->network->client, "KICK", room->name, nick, NULL);
}


static void add_member_actions(Conversation *conversation) {
	static const GActionEntry ENTRIES[] = {
		{"message", message_member, "s", NULL, NULL, {0}},
		{"mention", mention_member, "s", NULL, NULL, {0}},
		{"profile", show_member_profile, "s", NULL, NULL, {0}},
		{"op", moderate_member, "s", NULL, NULL, {0}},
		{"deop", moderate_member, "s", NULL, NULL, {0}},
		{"voice", moderate_member, "s", NULL, NULL, {0}},
		{"devoice", moderate_member, "s", NULL, NULL, {0}},
		{"kick", moderate_member, "s", NULL, NULL, {0}},
		{"ban", moderate_member, "s", NULL, NULL, {0}},
	};
	GSimpleActionGroup *group = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(group), ENTRIES, G_N_ELEMENTS(ENTRIES), conversation);
	/* The members panel and the messages both offer a person's menu, so the actions serve the whole room view */
	gtk_widget_insert_action_group(conversation->split, "member", G_ACTION_GROUP(group));
	g_object_unref(group);
}




typedef struct {
	int rank;
	const char *nick;
} RankedMember;


static int order_members(gconstpointer first, gconstpointer second) {
	const RankedMember *one = first, *other = second;
	if (one->rank != other->rank)
		return one->rank - other->rank;
	return g_ascii_strcasecmp(one->nick, other->nick);
}


/* Members ordered by status, highest first, then by name */
static gboolean fill_members(gpointer data) {
	Conversation *conversation = data;
	conversation->member_source = 0;
	IrcState *state = conversation->room ? get_network_state(conversation->room->network) : NULL;
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	g_autoptr(GArray) ranked = g_array_new(FALSE, FALSE, sizeof(RankedMember));
	if (channel) {
		GHashTableIter iterator;
		gpointer folded, symbols;
		g_hash_table_iter_init(&iterator, channel->members);
		while (g_hash_table_iter_next(&iterator, &folded, &symbols)) {
			IrcUser *user = g_hash_table_lookup(state->users, folded);
			int rank = rank_irc_prefix(state, symbols);
			RankedMember member = {rank < 0 ? G_MAXINT : rank, user ? user->nick : folded};
			g_array_append_val(ranked, member);
		}
	}
	g_array_sort(ranked, order_members);
	g_autoptr(GPtrArray) nicks = g_ptr_array_new();
	for (guint index = 0; index < ranked->len; index++)
		g_ptr_array_add(nicks, (gpointer)g_array_index(ranked, RankedMember, index).nick);
	g_ptr_array_add(nicks, NULL);
	/* The panel's title counts the people in the room */
	g_autofree char *title = ranked->len ? g_strdup_printf(ranked->len == 1 ? TEXT_MEMBERS_ONE : TEXT_MEMBERS_COUNT,
		ranked->len) : g_strdup(TEXT_MEMBERS_TITLE);
	adw_window_title_set_title(ADW_WINDOW_TITLE(conversation->members_title), title);
	/* Most room changes leave the members as they were, and filling the list again would draw all its rows again */
	GListModel *members = G_LIST_MODEL(conversation->members);
	guint count = g_list_model_get_n_items(members);
	gboolean is_same = count == ranked->len;
	for (guint index = 0; is_same && index < count; index++)
		is_same = g_str_equal(gtk_string_list_get_string(conversation->members, index),
			g_ptr_array_index(nicks, index));
	if (!is_same)
		gtk_string_list_splice(conversation->members, 0, count, (const char *const *)nicks->pdata);
	return G_SOURCE_REMOVE;
}


static void refresh_members_later(Conversation *conversation) {
	gboolean is_shown = adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split));
	if (is_shown && !conversation->member_source)
		conversation->member_source = g_idle_add(fill_members, conversation);
}


static GtkWidget *create_member_panel(Conversation *conversation) {
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(panel, "member-panel");
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "title-bar");
	conversation->members_title = adw_window_title_new(TEXT_MEMBERS_TITLE, NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), conversation->members_title);
	/* The panel sits at the window's right edge, so its header carries the window buttons there, and it never
	 * offers the narrow window's Back, which would leave the whole room instead of closing the panel */
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_back_button(ADW_HEADER_BAR(header), FALSE);
	gtk_box_append(GTK_BOX(panel), header);
	conversation->members = gtk_string_list_new(NULL);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_member), conversation);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_member), conversation);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(conversation->members)));
	gtk_single_selection_set_autoselect(selection, FALSE);
	gtk_single_selection_set_can_unselect(selection, TRUE);
	gtk_single_selection_set_selected(selection, GTK_INVALID_LIST_POSITION);
	conversation->member_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
	gtk_widget_add_css_class(conversation->member_view, "member-list");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), conversation->member_view);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(panel), scroller);
	return panel;
}


static void show_members_panel(gpointer data) {
	Conversation *conversation = data;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(conversation->members_button), TRUE);
}


void show_conversation_details(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	if (!room)
		return;
	if (room->kind == ROOM_CHANNEL)
		show_room_details(conversation->window, room, G_CALLBACK(show_members_panel), conversation);
	else if (room->kind == ROOM_PRIVATE)
		show_profile(conversation->window, room, room->name);
}


/* Clicking the header shows a room's card, or the person a private chat is with */
static void click_header(GtkGestureClick *gesture, int presses, double x, double y, Conversation *conversation) {
	show_conversation_details(conversation);
}


static void toggle_members(GtkToggleButton *button, Conversation *conversation) {
	conversation->wants_members = gtk_toggle_button_get_active(button);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		conversation->wants_members);
	refresh_members_later(conversation);
}


/* Docked members are parted from the chat by the chat's own edge, which stops at the composer */
static void mark_docked_members(Conversation *conversation) {
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(conversation->split);
	if (adw_overlay_split_view_get_show_sidebar(split) && !adw_overlay_split_view_get_collapsed(split))
		gtk_widget_add_css_class(conversation->split, "docked-members");
	else
		gtk_widget_remove_css_class(conversation->split, "docked-members");
}


/* Closing the overlaid list by clicking beside it counts as the user closing it */
static void follow_member_panel(AdwOverlaySplitView *split, GParamSpec *spec, Conversation *conversation) {
	mark_docked_members(conversation);
	gboolean is_shown = adw_overlay_split_view_get_show_sidebar(split);
	if (adw_overlay_split_view_get_collapsed(split))
		conversation->wants_members = is_shown;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(conversation->members_button), is_shown);
}


/* A channel shows the member list beside it while the user wants it and there is room, and only lies over the chat
 * when opened there, so hiding it for want of room keeps the user's wish for when the chat widens again */
static void place_member_panel(Conversation *conversation) {
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(conversation->split);
	gboolean wants_members = conversation->wants_members;
	CrimsonRoom *room = conversation->room;
	adw_overlay_split_view_set_show_sidebar(split, room && room->kind == ROOM_CHANNEL && wants_members
		&& !adw_overlay_split_view_get_collapsed(split));
	conversation->wants_members = wants_members;
	mark_docked_members(conversation);
}


static GtkWidget *create_header(Conversation *conversation) {
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "title-bar");
	GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	conversation->avatar = adw_avatar_new(HEADER_AVATAR_SIZE, NULL, TRUE);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	conversation->title = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(conversation->title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(conversation->title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(conversation->title, "conversation-title");
	conversation->subtitle = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(conversation->subtitle), 0);
	gtk_label_set_ellipsize(GTK_LABEL(conversation->subtitle), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(conversation->subtitle, "conversation-subtitle");
	gtk_box_append(GTK_BOX(texts), conversation->title);
	gtk_box_append(GTK_BOX(texts), conversation->subtitle);
	gtk_box_append(GTK_BOX(title), conversation->avatar);
	gtk_box_append(GTK_BOX(title), texts);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), title);
	gtk_widget_set_cursor_from_name(title, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(click_header), conversation);
	gtk_widget_add_controller(title, GTK_EVENT_CONTROLLER(click));
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new(NULL));
	conversation->members_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(conversation->members_button), "system-users-symbolic");
	gtk_widget_set_tooltip_text(conversation->members_button, TEXT_MEMBERS_TOOLTIP);
	g_signal_connect(conversation->members_button, "toggled", G_CALLBACK(toggle_members), conversation);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), conversation->members_button);
	return header;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(page, "chat-background");
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "title-bar");
	gtk_widget_add_css_class(header, "flat");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new(NULL));
	gtk_box_append(GTK_BOX(page), header);
	return page;
}


static GtkWidget *create_room_page(Conversation *conversation) {
	GtkWidget *toolbar = adw_toolbar_view_new();
	gtk_widget_add_css_class(toolbar, "title-bar");
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(conversation));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), create_message_area(conversation));
	conversation->composer = create_composer();
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), get_composer_widget(conversation->composer));
	adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_FLAT);
	conversation->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(conversation->split), GTK_PACK_END);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split), FALSE);
	adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(conversation->split), MEMBERS_WIDTH);
	adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(conversation->split), MEMBERS_WIDTH);
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(conversation->split), toolbar);
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		create_member_panel(conversation));
	g_signal_connect(conversation->split, "notify::show-sidebar", G_CALLBACK(follow_member_panel), conversation);
	g_signal_connect_swapped(conversation->split, "notify::collapsed", G_CALLBACK(place_member_panel), conversation);
	add_member_actions(conversation);
	GtkWidget *bin = adw_breakpoint_bin_new();
	gtk_widget_set_size_request(bin, CHAT_MINIMUM_WIDTH, CHAT_MINIMUM_HEIGHT);
	adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(bin), conversation->split);
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(DOCKED_MEMBERS_CONDITION));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(conversation->split), "collapsed", &collapsed);
	adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(bin), breakpoint);
	return bin;
}


/* Only the rows on screen hold day headings, and the rest are written fresh when they show */
static void relabel_days(gpointer data) {
	Conversation *conversation = data;
	GHashTableIter iterator;
	gpointer room, value;
	g_hash_table_iter_init(&iterator, conversation->views);
	while (g_hash_table_iter_next(&iterator, &room, &value)) {
		for (GtkWidget *item = gtk_widget_get_first_child(((RoomView *)value)->list_view); item;
			item = gtk_widget_get_next_sibling(item)) {
			GtkWidget *row = gtk_widget_get_first_child(item);
			if (row)
				refresh_day_heading(row);
		}
	}
}


Conversation *create_conversation(Window *window) {
	Conversation *conversation = g_new0(Conversation, 1);
	conversation->window = window;
	conversation->actions = (MessageActions){reply_to, react_to, remove_message, show_message, show_person,
		show_person_menu, conversation};
	/* Coming back to the window counts as seeing the open chat, so what arrived meanwhile is read */
	g_signal_connect_swapped(window->window, "notify::is-active", G_CALLBACK(mark_read_when_seen), conversation);
	conversation->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_room_page(conversation), "room");
	conversation->still_page = gtk_picture_new();
	gtk_picture_set_can_shrink(GTK_PICTURE(conversation->still_page), TRUE);
	gtk_picture_set_content_fit(GTK_PICTURE(conversation->still_page), GTK_CONTENT_FIT_FILL);
	gtk_stack_add_child(GTK_STACK(conversation->stack), conversation->still_page);
	gtk_stack_set_transition_duration(GTK_STACK(conversation->stack), PAGE_SLIDE_MS);
	watch_day_change(relabel_days, conversation);
	return conversation;
}


GtkWidget *get_conversation_widget(Conversation *conversation) {
	return conversation->stack;
}


CrimsonRoom *get_conversation_room(Conversation *conversation) {
	return conversation->room;
}


static void fill_header(Conversation *conversation);


static void show_activity(Profile *profile, gpointer data) {
	Conversation *conversation = data;
	CrimsonRoom *room = profile && conversation->room ? find_room(conversation->room->network, profile->nick) : NULL;
	if (!room || room->kind != ROOM_PRIVATE)
		return;
	gint64 now = g_get_real_time();
	room->activity = !profile->is_found ? ACTIVITY_GONE : profile->away ? ACTIVITY_AWAY : ACTIVITY_SEEN;
	room->last_active = now - profile->idle_seconds * G_USEC_PER_SEC;
	room->activity_checked = now;
	save_chat_cache_later(room->network->session->chat);
	if (room == conversation->room)
		fill_header(conversation);
}


/* Keeps the shown time counting, and asks the server again once the last answer is a couple of minutes old */
static gboolean poll_activity(gpointer data) {
	Conversation *conversation = data;
	CrimsonRoom *room = conversation->room;
	if (!room || room->kind != ROOM_PRIVATE)
		return G_SOURCE_CONTINUE;
	fill_header(conversation);
	gboolean is_stale = g_get_real_time() - room->activity_checked >= ACTIVITY_SECONDS * G_USEC_PER_SEC;
	if (is_stale && room->network->state == NETWORK_CONNECTED && !get_network_work(room->network)->profile
		&& !is_target_refused(room->network, room->name))
		request_profile(room->network, room->name, show_activity, conversation);
	return G_SOURCE_CONTINUE;
}


static void follow_activity(Conversation *conversation, CrimsonRoom *previous) {
	g_clear_handle_id(&conversation->activity_source, g_source_remove);
	if (previous)
		cancel_profile(previous->network, conversation);
	CrimsonRoom *room = conversation->room;
	if (!room || room->kind != ROOM_PRIVATE)
		return;
	poll_activity(conversation);
	conversation->activity_source = g_timeout_add_seconds(SECONDS_PER_MINUTE, poll_activity, conversation);
}


static void fill_header(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	g_autofree char *title = describe_room_title(room);
	g_autofree char *topic = room->topic ? strip_irc_formatting(room->topic) : NULL;
	/* A room's topic says what it is about, so it takes the place of the member count unless someone types */
	gboolean shows_topic = room->kind == ROOM_CHANNEL && topic && *topic
		&& !(is_room_typing(room) && get_settings()->shows_typing);
	gboolean is_typing = is_room_typing(room) && get_settings()->shows_typing;
	g_autofree char *activity = room->kind == ROOM_PRIVATE && !is_typing ? describe_room_activity(room) : NULL;
	g_autofree char *subtitle = shows_topic ? g_strdup(topic) : activity ? g_strdup(activity)
		: describe_room_subtitle(room);
	gtk_label_set_text(GTK_LABEL(conversation->title), title);
	show_room_line(conversation->subtitle, subtitle, is_typing);
	/* Without anything to say under it, the name sits in the middle of the header */
	gtk_widget_set_visible(conversation->subtitle, subtitle && *subtitle);
	gtk_widget_set_tooltip_text(conversation->subtitle, shows_topic ? topic : NULL);
	fill_room_avatar(ADW_AVATAR(conversation->avatar), room, HEADER_AVATAR_SIZE);
	gtk_widget_set_visible(conversation->members_button, room->kind == ROOM_CHANNEL);
}


static void follow_connection(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	g_autofree char *wait = room && room->kind != ROOM_SERVER ? describe_connection_wait(room->network) : NULL;
	set_composer_waiting(conversation->composer, wait);
}


static gboolean is_busy_room(CrimsonRoom *room) {
	IrcState *state = get_network_state(room->network);
	IrcChannel *channel = state ? find_irc_channel(state, room->name) : NULL;
	return channel && g_hash_table_size(channel->members) >= TYPING_FACES_MEMBERS;
}


/* The faces are only made again when who types changes, since a new face starts its hop over, and hovering one
 * names who it is */
static void fill_typing_faces(Conversation *conversation) {
	g_autoptr(GPtrArray) nicks = g_hash_table_get_keys_as_ptr_array(conversation->room->typing);
	g_ptr_array_sort_values(nicks, (GCompareFunc)g_strcmp0);
	if (nicks->len > TYPING_FACES_COUNT)
		g_ptr_array_set_size(nicks, TYPING_FACES_COUNT);
	GtkWidget *child = gtk_widget_get_first_child(conversation->typing_faces);
	guint shown = 0;
	while (child && shown < nicks->len
		&& g_strcmp0(g_object_get_data(G_OBJECT(child), NICK_KEY), g_ptr_array_index(nicks, shown)) == 0) {
		child = gtk_widget_get_next_sibling(child);
		shown++;
	}
	if (!child && shown == nicks->len)
		return;
	while ((child = gtk_widget_get_first_child(conversation->typing_faces)))
		gtk_fixed_remove(GTK_FIXED(conversation->typing_faces), child);
	for (guint index = 0; index < nicks->len; index++) {
		const char *nick = g_ptr_array_index(nicks, index);
		GtkWidget *face = adw_avatar_new(MESSAGE_AVATAR_SIZE, nick, TRUE);
		g_object_set_data_full(G_OBJECT(face), NICK_KEY, g_strdup(nick), g_free);
		gtk_widget_add_css_class(face, "typing-face");
		g_autofree char *typing = g_strdup_printf(TEXT_TYPING_ONE, nick);
		gtk_widget_set_tooltip_text(face, typing);
		const char *url = pick_person_picture(conversation->room, nick);
		g_autofree char *sized = fill_image_size(url, MESSAGE_AVATAR_SIZE * 2);
		show_avatar_image(ADW_AVATAR(face), sized);
		gtk_fixed_put(GTK_FIXED(conversation->typing_faces), face,
			index * (MESSAGE_AVATAR_SIZE - TYPING_FACES_OVERLAP), TYPING_HOP_ROOM);
	}
}


/* The avatars of who types bob up with a little overshoot as someone starts typing in a busy room, and sink once
 * nobody does, keeping the last faces while they sink */
static void show_typing_faces(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	gboolean wants = room && room->kind == ROOM_CHANNEL && get_settings()->shows_typing && is_room_typing(room)
		&& is_busy_room(room);
	if (wants)
		fill_typing_faces(conversation);
	if (wants == conversation->shows_typing_faces)
		return;
	conversation->shows_typing_faces = wants;
	/* Resetting moves the avatars back to where the last bob started, so where they are now is read first */
	double rise = conversation->typing_rise;
	AdwTimedAnimation *animation = ADW_TIMED_ANIMATION(conversation->typing_animation);
	adw_animation_reset(conversation->typing_animation);
	adw_timed_animation_set_value_from(animation, rise);
	adw_timed_animation_set_value_to(animation, wants ? 1 : 0);
	adw_timed_animation_set_easing(animation, wants ? ADW_EASE_OUT_BACK : ADW_EASE_IN_CUBIC);
	adw_animation_play(conversation->typing_animation);
}


/* The room counts as read only while its bottom is in view, so its unread messages are the ones waiting below,
 * and the jump button shows their number in place of its arrow */
static void show_waiting_count(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	style_unread_badge(conversation->jump_count, room->unread_count, room->has_mention);
	gtk_stack_set_visible_child_name(GTK_STACK(conversation->jump_icons), room->unread_count ? "count" : "arrow");
}


/* Turning joins and leaves on or off fills each kept list again, since any group and day line may change */
static void follow_event_setting(Conversation *conversation) {
	gboolean shows_events = get_settings()->shows_events;
	if (shows_events == conversation->shows_events)
		return;
	conversation->shows_events = shows_events;
	GHashTableIter iterator;
	gpointer room, value;
	g_hash_table_iter_init(&iterator, conversation->views);
	while (g_hash_table_iter_next(&iterator, &room, &value)) {
		RoomView *view = value;
		gtk_filter_list_model_set_model(view->shown_messages, NULL);
		gtk_filter_list_model_set_model(view->shown_messages, G_LIST_MODEL(view->recent_messages));
	}
}


void refresh_conversation(Conversation *conversation) {
	if (!conversation->room)
		return;
	follow_event_setting(conversation);
	show_waiting_count(conversation);
	follow_connection(conversation);
	fill_header(conversation);
	show_typing_faces(conversation);
	show_empty_note(conversation);
	refresh_members_later(conversation);
	mark_read_when_seen(conversation);
	reveal_when_near_top(conversation->view);
}


/* The first shown message after a moment, whose row shows the line */
static CrimsonMessage *find_message_after(RoomView *view, gint64 time) {
	GListModel *messages = G_LIST_MODEL(view->messages);
	CrimsonMessage *found = NULL;
	for (guint index = g_list_model_get_n_items(messages); index > 0; index--) {
		g_autoptr(CrimsonMessage) message = g_list_model_get_item(messages, index - 1);
		if (message->time <= time)
			break;
		found = message;
	}
	return found;
}


/* The rows on either side of the line draw again, which their messages' change signal does */
static void set_read_line(RoomView *view, gboolean has_line, gint64 time) {
	CrimsonMessage *old = view->has_read_line ? find_message_after(view, view->read_line) : NULL;
	view->read_line = has_line ? time : 0;
	view->has_read_line = has_line;
	CrimsonMessage *new = has_line ? find_message_after(view, time) : NULL;
	if (old && old != new)
		announce_message_change(old);
	if (new)
		announce_message_change(new);
}


static void follow_room(CrimsonRoom *room, Conversation *conversation) {
	/* Messages arriving while the user reads along are read at once, so only unseen ones start a line */
	RoomView *view = conversation->view;
	gboolean is_unread = room->unread_count > 0;
	gboolean is_seen = view->is_pinned && is_room_in_view(conversation->window, room);
	if (is_unread && !view->was_unread && !is_seen)
		set_read_line(view, TRUE, room->read_time);
	view->was_unread = is_unread;
	refresh_conversation(conversation);
}


/* A kept view comes back at its latest messages, with the line where reading stopped this time */
static void show_room_view(Conversation *conversation, RoomView *view) {
	CrimsonRoom *room = view->room;
	view->shown_time = g_get_monotonic_time();
	conversation->view = view;
	view->was_unread = room->unread_count > 0;
	set_read_line(view, view->was_unread, room->read_time);
	pin_to_bottom(view);
	/* The wide window fades quickly from one chat to the next, while the narrow window's own page slide moves it */
	gtk_stack_set_transition_type(GTK_STACK(conversation->views_stack), conversation->is_narrow
		? GTK_STACK_TRANSITION_TYPE_NONE : GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_visible_child(GTK_STACK(conversation->views_stack), view->scroller);
	show_empty_note(conversation);
	drop_old_views(conversation);
}


/* Puts a still image of the shown page in its place, so the page can change underneath before sliding in */
static void freeze_shown_page(Conversation *conversation) {
	GtkStack *stack = GTK_STACK(conversation->stack);
	g_autoptr(GdkPaintable) page = gtk_widget_paintable_new(gtk_stack_get_visible_child(stack));
	g_autoptr(GdkPaintable) still = gdk_paintable_get_current_image(page);
	gtk_picture_set_paintable(GTK_PICTURE(conversation->still_page), still);
	gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_stack_set_visible_child(stack, conversation->still_page);
}


void show_conversation_room(Conversation *conversation, CrimsonRoom *room) {
	if (conversation->room == room)
		return;
	/* A narrow window already showing a chat has no page slide to move the next one in, so it slides here */
	gboolean slides_page = conversation->is_narrow && conversation->room && room
		&& gtk_widget_get_mapped(conversation->stack);
	if (slides_page)
		freeze_shown_page(conversation);
	g_autoptr(CrimsonRoom) previous = conversation->room ? g_object_ref(conversation->room) : NULL;
	if (conversation->room)
		g_clear_signal_handler(&conversation->room_handler, conversation->room);
	if (conversation->view)
		g_clear_handle_id(&conversation->view->reveal_source, g_source_remove);
	switch_composer_room(conversation->composer, room);
	g_set_object(&conversation->room, room);
	/* The same nick in another room can have another picture, so the floating avatar is filled again */
	g_clear_pointer(&conversation->sticky_nick, g_free);
	/* Another room starts with the typing avatars out of sight rather than sinking from the last one */
	adw_animation_reset(conversation->typing_animation);
	conversation->typing_rise = 0;
	conversation->shows_typing_faces = FALSE;
	gtk_stack_set_transition_type(GTK_STACK(conversation->stack), slides_page ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
		: GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_stack_set_visible_child_name(GTK_STACK(conversation->stack), room ? "room" : "empty");
	follow_activity(conversation, previous);
	conversation->view = NULL;
	if (!room)
		return;
	/* History is asked for first, so a room still loading does not show itself as empty for a moment */
	if (!room->has_history)
		request_latest_history(room);
	RoomView *view = g_hash_table_lookup(conversation->views, room);
	show_room_view(conversation, view ? view : create_room_view(conversation, room));
	conversation->room_handler = g_signal_connect(room, "changed", G_CALLBACK(follow_room), conversation);
	fill_header(conversation);
	follow_connection(conversation);
	place_member_panel(conversation);
	refresh_members_later(conversation);
	mark_read_when_seen(conversation);
}


static int compare_room_activity(gconstpointer first, gconstpointer second) {
	gint64 one = (*(CrimsonRoom *const *)first)->last_time, other = (*(CrimsonRoom *const *)second)->last_time;
	return (one < other) - (one > other);
}


/* The server's rooms with the latest activity, which the user is likeliest to open, are built in quiet moments once
 * their messages are here */
void build_rooms_ahead(Conversation *conversation, CrimsonNetwork *network) {
	g_autoptr(GPtrArray) rooms = g_ptr_array_new();
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		CrimsonRoom *room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		if (g_list_model_get_n_items(G_LIST_MODEL(room->messages)))
			g_ptr_array_add(rooms, room);
		g_object_unref(room);
	}
	g_ptr_array_sort(rooms, compare_room_activity);
	for (guint index = 0; index < MIN(rooms->len, KEPT_VIEWS - 1); index++)
		build_room_ahead(conversation, g_ptr_array_index(rooms, index));
}


void clear_read_line(Conversation *conversation, CrimsonRoom *room) {
	RoomView *view = g_hash_table_lookup(conversation->views, room);
	if (!view)
		return;
	view->was_unread = FALSE;
	set_read_line(view, FALSE, 0);
}


void set_conversation_narrow(Conversation *conversation, gboolean is_narrow) {
	conversation->is_narrow = is_narrow;
}


void focus_composer(Conversation *conversation) {
	focus_composer_field(conversation->composer);
}
