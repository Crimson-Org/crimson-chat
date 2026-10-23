#include "../lang/en.h"
#include "settings.h"
#include "common/startup.h"
#include "composer.h"
#include "profile.h"
#include "formatting.h"
#include "images.h"
#include "message-row.h"
#include "room-look.h"
#include "sounds.h"
#include "window.h"
#include "pictures.h"

#define FADE_DURATION_MS 120
#define ACTIVITY_SECONDS 120
#define SECONDS_PER_MINUTE 60
#define HEADER_AVATAR_SIZE 38
#define MEMBER_AVATAR_SIZE 32
#define BOTTOM_SLACK 48.0
/* How many of a room's latest messages the list holds when the room opens, and how many more each time the top is
 * reached, so a room opens as fast with thousands of lines as with a hundred */
#define SHOWN_MESSAGES_PAGE 100
#define CONTEXT_BEFORE_TARGET 10
#define MEMBERS_WIDTH 260
#define MESSAGE_KEY "message"
#define HANDLER_KEY "changed-handler"
/* How far below the top of the chat the avatar of the person being read stays */
#define STICKY_AVATAR_MARGIN 8
/* How far in from the side the avatars of who types in a busy room sit, how many show and how far each lies over
 * the one before, how much of them rises into view and how fast they bob */
#define TYPING_FACES_MARGIN 16
#define TYPING_FACES_COUNT 3
#define TYPING_FACES_OVERLAP 12
#define TYPING_FACES_SHOWN 0.5
#define TYPING_RISE_MS 260
/* Rooms with more people than the user and one other show typing with avatars rather than only the header */
#define TYPING_FACES_MEMBERS 3

struct Conversation {
	Window *window;
	GtkWidget *stack, *split, *avatar, *title, *subtitle, *members_button, *list_view, *scroller;
	/* The button down to the latest message, which shows an arrow or how many new messages wait below */
	GtkWidget *jump_button, *jump_icons, *jump_count;
	/* A still image of the chat that is leaving, which slides away while the real view already holds the next */
	GtkWidget *still;
	/* A narrow window slides whole pages itself, so the chat view does not slide on top of that */
	gboolean is_narrow;
	GtkWidget *member_view, *empty_note, *members_title;
	GtkStringList *members;
	GtkNoSelection *messages;
	/* The room's latest messages, and those without the joins and leaves the setting hides, which are kept */
	GtkSliceListModel *recent_messages;
	GtkFilterListModel *shown_messages;
	gboolean shows_events;
	Composer *composer;
	CrimsonRoom *room;
	gulong room_handler;
	/* The view follows the newest message until the user scrolls away from it */
	gboolean is_pinned, has_user_scrolled;
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
	/* The last moment read when new messages started coming in unseen, which a line in the chat marks until the
	 * user leaves the room */
	gint64 read_line;
	gboolean has_read_line, was_unread;
	/* The avatars of who types in a busy room, partly out of sight at the bottom and rising while they type */
	GtkWidget *typing_faces;
	AdwAnimation *typing_animation;
	double typing_rise;
	gboolean shows_typing_faces;
	MessageActions actions;
};


static GtkAdjustment *get_scroll(Conversation *conversation) {
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(conversation->scroller));
}


/* The list only knows its true length once its last rows are measured, so it is asked to show the last row */
static void scroll_to_bottom(Conversation *conversation) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages));
	if (count)
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), count - 1, GTK_LIST_SCROLL_NONE, NULL);
	GtkAdjustment *scroll = get_scroll(conversation);
	gtk_adjustment_set_value(scroll, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll));
}


static void mark_read_when_seen(Conversation *conversation) {
	if (conversation->room && conversation->is_pinned && is_room_in_view(conversation->window, conversation->room))
		mark_room_read(conversation->room);
}


static void pin_to_bottom(Conversation *conversation) {
	conversation->is_pinned = TRUE;
	conversation->has_user_scrolled = FALSE;
	gtk_widget_set_visible(conversation->jump_button, FALSE);
	scroll_to_bottom(conversation);
}


/* Rows measure themselves as they appear and move the view, so only the user's own scrolling unpins it */
static void follow_scroll(GtkAdjustment *scroll, Conversation *conversation) {
	if (conversation->has_user_scrolled) {
		double bottom = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
		conversation->is_pinned = gtk_adjustment_get_value(scroll) >= bottom - BOTTOM_SLACK;
		gtk_widget_set_visible(conversation->jump_button, !conversation->is_pinned);
	}
	mark_read_when_seen(conversation);
}


/* New lines keep the view at the bottom when it was there, and leave it alone while reading back */
static void follow_growth(GtkAdjustment *scroll, GParamSpec *spec, Conversation *conversation) {
	if (conversation->is_pinned)
		scroll_to_bottom(conversation);
}


static gboolean note_user_scroll(GtkEventControllerScroll *controller, double x, double y, gpointer data) {
	Conversation *conversation = data;
	conversation->has_user_scrolled = TRUE;
	return FALSE;
}


/* Older messages already in memory come back a page at a time, and only past the oldest is the server asked */
static void reveal_older_messages(Conversation *conversation) {
	guint offset = gtk_slice_list_model_get_offset(conversation->recent_messages);
	if (!offset) {
		load_older_history(conversation->room);
		return;
	}
	gtk_slice_list_model_set_offset(conversation->recent_messages, offset > SHOWN_MESSAGES_PAGE
		? offset - SHOWN_MESSAGES_PAGE : 0);
}


static void reach_edge(GtkScrolledWindow *scroller, GtkPositionType position, Conversation *conversation) {
	if (position == GTK_POS_TOP && conversation->room)
		reveal_older_messages(conversation);
	else if (position == GTK_POS_BOTTOM && conversation->has_user_scrolled)
		pin_to_bottom(conversation);
}


static void jump_down(GtkButton *button, Conversation *conversation) {
	pin_to_bottom(conversation);
}


/* The model keeps its messages, so the borrowed one stays valid while the list holds it */
static CrimsonMessage *get_message_at(Conversation *conversation, guint position) {
	CrimsonMessage *message = g_list_model_get_item(G_LIST_MODEL(conversation->messages), position);
	if (message)
		g_object_unref(message);
	return message;
}


/* The line needs something read above it, so a chat with no earlier activity shows none */
static gboolean starts_unread(Conversation *conversation, CrimsonMessage *message, CrimsonMessage *previous) {
	gint64 line = conversation->read_line;
	return conversation->has_read_line && previous && message->time > line && previous->time <= line;
}


static gboolean ends_group_at(Conversation *conversation, guint position, CrimsonMessage *message) {
	CrimsonMessage *next = get_message_at(conversation, position + 1);
	return !next || starts_message_group(next, message, starts_unread(conversation, next, message));
}


/* The first event of a run gathers the events below it, one per line */
static char *gather_notes(Conversation *conversation, guint position, CrimsonMessage *message) {
	GString *notes = g_string_new(NULL);
	CrimsonMessage *current = message;
	for (guint index = position;; index++) {
		g_autofree char *plain = strip_irc_formatting(current->text);
		g_string_append_printf(notes, "%s%s", notes->len ? "\n" : "", plain);
		CrimsonMessage *next = get_message_at(conversation, index + 1);
		if (!next || !continues_note_run(next, current, starts_unread(conversation, next, current)))
			break;
		current = next;
	}
	return g_string_free(notes, FALSE);
}


static void fill_row(Conversation *conversation, GtkListItem *item) {
	CrimsonMessage *message = gtk_list_item_get_item(item);
	guint position = gtk_list_item_get_position(item);
	CrimsonMessage *previous = position ? get_message_at(conversation, position - 1) : NULL;
	RowPlace place = {previous, starts_unread(conversation, message, previous),
		ends_group_at(conversation, position, message), NULL};
	g_autofree char *notes = NULL;
	if (message->kind == MESSAGE_EVENT && !continues_note_run(message, previous, place.starts_unread))
		notes = gather_notes(conversation, position, message);
	place.notes = notes;
	fill_message_row(gtk_list_item_get_child(item), message, &place, conversation->room, &conversation->actions);
}


static void refill_row(CrimsonMessage *message, gpointer item) {
	GtkWidget *row = gtk_list_item_get_child(item);
	Conversation *conversation = g_object_get_data(G_OBJECT(row), "conversation");
	if (conversation->room)
		fill_row(conversation, item);
}


static void set_up_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	GtkWidget *row = create_message_row();
	g_object_set_data(G_OBJECT(row), "conversation", conversation);
	gtk_list_item_set_child(item, row);
	gtk_list_item_set_activatable(item, FALSE);
	gtk_list_item_set_focusable(item, FALSE);
}


static void bind_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	CrimsonMessage *message = gtk_list_item_get_item(item);
	GtkWidget *row = gtk_list_item_get_child(item);
	fill_row(conversation, item);
	gulong handler = g_signal_connect(message, "changed", G_CALLBACK(refill_row), item);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, GSIZE_TO_POINTER(handler));
	g_object_set_data(G_OBJECT(row), MESSAGE_KEY, message);
}


static void unbind_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
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


static gboolean find_shown_position(Conversation *conversation, CrimsonMessage *target, guint *position) {
	GListModel *shown = G_LIST_MODEL(conversation->messages);
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
	CrimsonMessage *target = conversation->room ? find_room_message(conversation->room, message_id) : NULL;
	guint position = 0;
	/* A message older than the ones held comes into the list first, with a few before it for context */
	guint index = 0;
	if (target && g_list_store_find(conversation->room->messages, target, &index)
		&& index < gtk_slice_list_model_get_offset(conversation->recent_messages))
		gtk_slice_list_model_set_offset(conversation->recent_messages, index > CONTEXT_BEFORE_TARGET
			? index - CONTEXT_BEFORE_TARGET : 0);
	if (target && find_shown_position(conversation, target, &position)) {
		conversation->is_pinned = FALSE;
		conversation->has_user_scrolled = TRUE;
		gtk_widget_set_visible(conversation->jump_button, TRUE);
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), position, GTK_LIST_SCROLL_NONE, NULL);
	}
}


static void open_person(const char *nick, gpointer data) {
	Conversation *conversation = data;
	if (!conversation->room || !nick)
		return;
	CrimsonRoom *room = open_private_room(conversation->room->network, nick);
	open_room(conversation->window, room);
}


/* An empty room says so, unless its history is still on the way */
static void show_empty_note(Conversation *conversation) {
	gboolean is_empty = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages)) == 0;
	gboolean is_waiting = conversation->room && conversation->room->is_loading_history;
	gtk_widget_set_visible(conversation->empty_note, is_empty && !is_waiting);
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
	for (GtkWidget *item = gtk_widget_get_first_child(conversation->list_view); item;
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
	int risen = (int)(size.height * TYPING_FACES_SHOWN * conversation->typing_rise);
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
	gtk_widget_set_can_target(conversation->typing_faces, FALSE);
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


/* Someone else's message arriving in the open chat makes a short sound, with or without the window in focus,
 * while the messages already there when a chat opens make none */
static void sound_new_message(GListModel *messages, guint position, guint removed, guint added,
	Conversation *conversation) {
	guint count = g_list_model_get_n_items(messages);
	if (!added || position + added != count)
		return;
	g_autoptr(CrimsonMessage) message = g_list_model_get_item(messages, count - 1);
	gboolean is_spoken = message->kind != MESSAGE_EVENT && message->kind != MESSAGE_STATUS;
	if (is_message_arriving(message) && !message->is_own && is_spoken)
		play_sound(SOUND_MESSAGE);
}


static gboolean is_message_shown(gpointer item, gpointer data) {
	Conversation *conversation = data;
	return !CRIMSON_MESSAGE(item)->is_membership || conversation->shows_events;
}


static GtkWidget *create_message_list(Conversation *conversation) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_message), conversation);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_message), conversation);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_message), conversation);
	conversation->shows_events = get_settings()->shows_events;
	GtkCustomFilter *filter = gtk_custom_filter_new(is_message_shown, conversation, NULL);
	/* The slice reaches to the end of the room as it grows, sized so its offset and size can never overflow */
	conversation->recent_messages = gtk_slice_list_model_new(NULL, 0, G_MAXINT);
	conversation->shown_messages = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(conversation->recent_messages)),
		GTK_FILTER(filter));
	conversation->messages = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(conversation->shown_messages)));
	conversation->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(conversation->messages)), factory);
	gtk_widget_add_css_class(conversation->list_view, "messages");
	conversation->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(conversation->scroller), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(conversation->scroller), conversation->list_view);
	gtk_widget_set_vexpand(conversation->scroller, TRUE);
	g_signal_connect(conversation->scroller, "edge-reached", G_CALLBACK(reach_edge), conversation);
	GtkAdjustment *scroll = get_scroll(conversation);
	g_signal_connect(scroll, "value-changed", G_CALLBACK(follow_scroll), conversation);
	g_signal_connect(scroll, "notify::upper", G_CALLBACK(follow_growth), conversation);
	g_signal_connect(scroll, "notify::page-size", G_CALLBACK(follow_growth), conversation);
	GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(note_user_scroll), conversation);
	gtk_widget_add_controller(conversation->scroller, wheel);
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
	conversation->empty_note = gtk_label_new(TEXT_NO_MESSAGES);
	gtk_widget_add_css_class(conversation->empty_note, "empty-pill");
	gtk_widget_set_halign(conversation->empty_note, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(conversation->empty_note, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(conversation->empty_note, FALSE);
	g_signal_connect_swapped(conversation->messages, "items-changed", G_CALLBACK(show_empty_note), conversation);
	g_signal_connect(conversation->messages, "items-changed", G_CALLBACK(sound_new_message), conversation);
	GtkWidget *overlay = gtk_overlay_new();
	conversation->message_overlay = overlay;
	gtk_overlay_set_child(GTK_OVERLAY(overlay), conversation->scroller);
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
	g_signal_connect_swapped(scroll, "value-changed", G_CALLBACK(replace_sticky_avatar), conversation);
	g_signal_connect_swapped(scroll, "notify::upper", G_CALLBACK(replace_sticky_avatar), conversation);
	g_signal_connect_swapped(conversation->messages, "items-changed", G_CALLBACK(replace_sticky_avatar),
		conversation);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->empty_note);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->jump_button);
	gtk_widget_add_css_class(overlay, "chat-background");
	return overlay;
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
	gtk_string_list_splice(conversation->members, 0, g_list_model_get_n_items(G_LIST_MODEL(conversation->members)),
		(const char *const *)nicks->pdata);
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
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
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


/* Closing the overlaid list by clicking beside it counts as the user closing it */
static void follow_member_panel(AdwOverlaySplitView *split, GParamSpec *spec, Conversation *conversation) {
	gboolean is_shown = adw_overlay_split_view_get_show_sidebar(split);
	if (adw_overlay_split_view_get_collapsed(split))
		conversation->wants_members = is_shown;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(conversation->members_button), is_shown);
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
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), create_message_list(conversation));
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
	add_member_actions(conversation);
	return conversation->split;
}


/* Shows a still image of the open chat in its place, so the chat can change underneath while the image slides */
static gboolean freeze_open_chat(Conversation *conversation) {
	GtkWidget *page = gtk_stack_get_visible_child(GTK_STACK(conversation->stack));
	if (conversation->is_narrow || page == conversation->still || gtk_widget_get_width(page) <= 0
		|| !gtk_widget_get_mapped(page) || g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(conversation->stack)),
		"room") != 0)
		return FALSE;
	g_autoptr(GdkPaintable) live = gtk_widget_paintable_new(page);
	g_autoptr(GdkPaintable) image = gdk_paintable_get_current_image(live);
	gtk_picture_set_paintable(GTK_PICTURE(conversation->still), image);
	gtk_stack_set_transition_type(GTK_STACK(conversation->stack), GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_stack_set_visible_child(GTK_STACK(conversation->stack), conversation->still);
	return TRUE;
}


/* The wide window fades quickly from one chat to the next, while the narrow window's own page slide moves it */
static void fade_to(Conversation *conversation, const char *name) {
	gtk_stack_set_transition_type(GTK_STACK(conversation->stack), conversation->is_narrow
		? GTK_STACK_TRANSITION_TYPE_NONE : GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_visible_child_name(GTK_STACK(conversation->stack), name);
}


static void forget_still(GtkStack *stack, GParamSpec *spec, Conversation *conversation) {
	if (!gtk_stack_get_transition_running(stack))
		gtk_picture_set_paintable(GTK_PICTURE(conversation->still), NULL);
}


/* Only the rows on screen hold day headings, and the rest are written fresh when they show */
static void relabel_days(gpointer data) {
	Conversation *conversation = data;
	for (GtkWidget *item = gtk_widget_get_first_child(conversation->list_view); item;
		item = gtk_widget_get_next_sibling(item)) {
		GtkWidget *row = gtk_widget_get_first_child(item);
		if (row)
			refresh_day_heading(row);
	}
}


Conversation *create_conversation(Window *window) {
	Conversation *conversation = g_new0(Conversation, 1);
	conversation->window = window;
	conversation->is_pinned = TRUE;
	conversation->actions = (MessageActions){reply_to, react_to, remove_message, show_message, show_person,
		show_person_menu, conversation};
	watch_day_change(relabel_days, conversation);
	/* Coming back to the window counts as seeing the open chat, so what arrived meanwhile is read */
	g_signal_connect_swapped(window->window, "notify::is-active", G_CALLBACK(mark_read_when_seen), conversation);
	conversation->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_room_page(conversation), "room");
	conversation->still = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(conversation->still), GTK_CONTENT_FIT_FILL);
	gtk_stack_add_named(GTK_STACK(conversation->stack), conversation->still, "still");
	gtk_stack_set_transition_duration(GTK_STACK(conversation->stack), FADE_DURATION_MS);
	g_signal_connect(conversation->stack, "notify::transition-running", G_CALLBACK(forget_still), conversation);
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


static void fill_typing_faces(Conversation *conversation) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(conversation->typing_faces)))
		gtk_fixed_remove(GTK_FIXED(conversation->typing_faces), child);
	g_autoptr(GPtrArray) nicks = g_hash_table_get_keys_as_ptr_array(conversation->room->typing);
	for (guint index = 0; index < nicks->len && index < TYPING_FACES_COUNT; index++) {
		const char *nick = g_ptr_array_index(nicks, index);
		GtkWidget *face = adw_avatar_new(MESSAGE_AVATAR_SIZE, nick, TRUE);
		gtk_widget_add_css_class(face, "typing-face");
		const char *url = pick_person_picture(conversation->room, nick);
		g_autofree char *sized = fill_image_size(url, MESSAGE_AVATAR_SIZE * 2);
		show_avatar_image(ADW_AVATAR(face), sized);
		gtk_fixed_put(GTK_FIXED(conversation->typing_faces), face,
			index * (MESSAGE_AVATAR_SIZE - TYPING_FACES_OVERLAP), 0);
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


/* Turning joins and leaves on or off fills the list again, since every group and day line may change */
static void follow_event_setting(Conversation *conversation) {
	gboolean shows_events = get_settings()->shows_events;
	if (shows_events == conversation->shows_events)
		return;
	conversation->shows_events = shows_events;
	gtk_filter_list_model_set_model(conversation->shown_messages, NULL);
	gtk_filter_list_model_set_model(conversation->shown_messages, G_LIST_MODEL(conversation->recent_messages));
	if (conversation->is_pinned)
		scroll_to_bottom(conversation);
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
}


/* The first shown message after a moment, whose row shows the line */
static CrimsonMessage *find_message_after(Conversation *conversation, gint64 time) {
	GListModel *messages = G_LIST_MODEL(conversation->messages);
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
static void move_read_line(Conversation *conversation, gint64 time) {
	CrimsonMessage *old = conversation->has_read_line
		? find_message_after(conversation, conversation->read_line) : NULL;
	conversation->read_line = time;
	conversation->has_read_line = TRUE;
	CrimsonMessage *new = find_message_after(conversation, time);
	if (old && old != new)
		g_signal_emit_by_name(old, "changed");
	if (new)
		g_signal_emit_by_name(new, "changed");
}


static void follow_room(CrimsonRoom *room, Conversation *conversation) {
	/* Messages arriving while the user reads along are read at once, so only unseen ones start a line */
	gboolean is_unread = room->unread_count > 0;
	gboolean is_seen = conversation->is_pinned && is_room_in_view(conversation->window, room);
	if (is_unread && !conversation->was_unread && !is_seen)
		move_read_line(conversation, room->read_time);
	conversation->was_unread = is_unread;
	refresh_conversation(conversation);
}


/* A newly opened room starts at its latest message, once the list has measured its rows */
static gboolean settle_at_bottom(gpointer data) {
	Conversation *conversation = data;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages));
	if (count)
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), count - 1, GTK_LIST_SCROLL_NONE, NULL);
	pin_to_bottom(conversation);
	mark_read_when_seen(conversation);
	return G_SOURCE_REMOVE;
}


void show_conversation_room(Conversation *conversation, CrimsonRoom *room) {
	if (conversation->room == room)
		return;
	freeze_open_chat(conversation);
	g_autoptr(CrimsonRoom) previous = conversation->room ? g_object_ref(conversation->room) : NULL;
	if (conversation->room)
		g_clear_signal_handler(&conversation->room_handler, conversation->room);
	switch_composer_room(conversation->composer, room);
	g_set_object(&conversation->room, room);
	/* The same nick in another room can have another picture, so the floating avatar is filled again */
	g_clear_pointer(&conversation->sticky_nick, g_free);
	/* Another room starts with the typing avatars out of sight rather than sinking from the last one */
	adw_animation_reset(conversation->typing_animation);
	conversation->typing_rise = 0;
	conversation->shows_typing_faces = FALSE;
	conversation->was_unread = room && room->unread_count > 0;
	conversation->has_read_line = conversation->was_unread;
	conversation->read_line = conversation->was_unread ? room->read_time : 0;
	guint count = room ? g_list_model_get_n_items(G_LIST_MODEL(room->messages)) : 0;
	gtk_slice_list_model_set_model(conversation->recent_messages, NULL);
	gtk_slice_list_model_set_offset(conversation->recent_messages, count > SHOWN_MESSAGES_PAGE
		? count - SHOWN_MESSAGES_PAGE : 0);
	gtk_slice_list_model_set_model(conversation->recent_messages, room ? G_LIST_MODEL(room->messages) : NULL);
	fade_to(conversation, room ? "room" : "empty");
	follow_activity(conversation, previous);
	if (!room)
		return;
	conversation->room_handler = g_signal_connect(room, "changed", G_CALLBACK(follow_room), conversation);
	fill_header(conversation);
	follow_connection(conversation);
	/* History is asked for first, so a room still loading does not show itself as empty for a moment */
	if (!room->has_history)
		request_latest_history(room);
	show_empty_note(conversation);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		room->kind == ROOM_CHANNEL && conversation->wants_members);
	refresh_members_later(conversation);
	pin_to_bottom(conversation);
	run_after_frame(conversation->list_view, settle_at_bottom, conversation);
}


void set_conversation_narrow(Conversation *conversation, gboolean is_narrow) {
	conversation->is_narrow = is_narrow;
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(conversation->split);
	adw_overlay_split_view_set_collapsed(split, is_narrow);
	adw_overlay_split_view_set_show_sidebar(split, conversation->wants_members && !is_narrow);
}


void focus_composer(Conversation *conversation) {
	focus_composer_field(conversation->composer);
}
