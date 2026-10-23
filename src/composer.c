#include <string.h>

#include <adwaita.h>

#include "../lang/en.h"
#include "composer.h"
#include "formatting.h"
#include "session.h"
#include "window.h"

#define TYPING_INTERVAL_MICROSECONDS (3 * G_USEC_PER_SEC)
#define SEND_SPINNER_SIZE 20
#define SEND_ROOM_MS 250
/* The send button's width and the gap before it, which the field gives up while the button shows */
#define SEND_ROOM 52
/* How far the field has moved aside when the send button starts to grow in */
#define SEND_GROW_POINT 0.5
#define PAUSE_SECONDS 6
#define FIELD_MAXIMUM_HEIGHT 180
#define DRAFTS_KEY "draft"

struct Composer {
	GtkWidget *box, *reply_bar, *reply_name, *reply_text, *field, *placeholder, *send_button;
	GtkTextBuffer *buffer;
	CrimsonRoom *room;
	CrimsonMessage *reply_to;
	gint64 last_typing;
	gboolean is_typing;
	/* Whether the room's server is not connected, which turns the send button into a spinner and holds the text */
	gboolean is_waiting, is_spinner_shown;
	/* The send button only takes room beside the field while there is text to send or a wait to show */
	GtkWidget *send_icons, *field_area;
	AdwAnimation *send_room_animation;
	double send_room;
	gboolean is_send_shown;
	guint pause_source, wait_source;
	gulong change_handler;
	/* Tab completion cycles through matches until another key is pressed */
	GPtrArray *matches;
	guint match_index;
	int match_start;
};


static char *get_field_text(Composer *composer) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(composer->buffer, &start, &end);
	return gtk_text_buffer_get_text(composer->buffer, &start, &end, FALSE);
}


static void cancel_reply(Composer *composer) {
	g_clear_object(&composer->reply_to);
	gtk_revealer_set_reveal_child(GTK_REVEALER(composer->reply_bar), FALSE);
}


void reply_in_composer(Composer *composer, CrimsonMessage *message) {
	g_set_object(&composer->reply_to, message);
	gtk_label_set_text(GTK_LABEL(composer->reply_name), message->sender ? message->sender : "");
	g_autofree char *plain = strip_irc_formatting(message->text);
	gtk_label_set_text(GTK_LABEL(composer->reply_text), plain);
	gtk_revealer_set_reveal_child(GTK_REVEALER(composer->reply_bar), TRUE);
	focus_composer_field(composer);
}


static void stop_typing_notice(Composer *composer) {
	g_clear_handle_id(&composer->pause_source, g_source_remove);
	composer->is_typing = FALSE;
	composer->last_typing = 0;
}


static void send_field(Composer *composer) {
	g_autofree char *text = get_field_text(composer);
	if (!composer->room || composer->is_waiting || strspn(text, " \n\t") == strlen(text))
		return;
	send_room_text(composer->room, text, composer->reply_to);
	gtk_text_buffer_set_text(composer->buffer, "", -1);
	stop_typing_notice(composer);
	cancel_reply(composer);
}


static void click_send(GtkButton *button, Composer *composer) {
	send_field(composer);
	focus_composer_field(composer);
}


static void clear_matches(Composer *composer) {
	g_clear_pointer(&composer->matches, g_ptr_array_unref);
}


/* Ranks each matching nick by how recently it spoke in the room, newest first, stopping once every one is found */
static GHashTable *rank_recent_speakers(CrimsonRoom *room, GPtrArray *nicks) {
	GHashTable *ranks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint index = 0; index < nicks->len; index++)
		g_hash_table_add(wanted, g_utf8_casefold(g_ptr_array_index(nicks, index), -1));
	GListModel *messages = G_LIST_MODEL(room->messages);
	guint count = g_list_model_get_n_items(messages);
	for (guint index = count; index > 0 && g_hash_table_size(ranks) < g_hash_table_size(wanted); index--) {
		g_autoptr(CrimsonMessage) message = g_list_model_get_item(messages, index - 1);
		g_autofree char *sender = message->sender ? g_utf8_casefold(message->sender, -1) : NULL;
		if (sender && g_hash_table_contains(wanted, sender) && !g_hash_table_contains(ranks, sender))
			g_hash_table_insert(ranks, g_steal_pointer(&sender), GUINT_TO_POINTER(count - index + 1));
	}
	return ranks;
}


static int compare_by_recent_speech(gconstpointer first, gconstpointer second, gpointer data) {
	const char *one = *(const char *const *)first, *other = *(const char *const *)second;
	g_autofree char *one_folded = g_utf8_casefold(one, -1);
	g_autofree char *other_folded = g_utf8_casefold(other, -1);
	guint one_rank = GPOINTER_TO_UINT(g_hash_table_lookup(data, one_folded));
	guint other_rank = GPOINTER_TO_UINT(g_hash_table_lookup(data, other_folded));
	/* Anyone who has not spoken comes after those who have, in alphabetical order */
	if (one_rank != other_rank)
		return !one_rank ? 1 : !other_rank ? -1 : one_rank < other_rank ? -1 : 1;
	return g_ascii_strcasecmp(one, other);
}


static void collect_matches(Composer *composer, const char *prefix) {
	composer->matches = g_ptr_array_new_with_free_func(g_free);
	IrcState *state = get_network_state(composer->room->network);
	g_autofree char *wanted = g_utf8_casefold(prefix, -1);
	IrcChannel *channel = state && composer->room->kind == ROOM_CHANNEL ? find_irc_channel(state,
		composer->room->name) : NULL;
	if (channel) {
		GHashTableIter iterator;
		gpointer folded;
		g_hash_table_iter_init(&iterator, channel->members);
		while (g_hash_table_iter_next(&iterator, &folded, NULL)) {
			IrcUser *user = g_hash_table_lookup(state->users, folded);
			g_autofree char *nick = user ? g_utf8_casefold(user->nick, -1) : NULL;
			if (nick && g_str_has_prefix(nick, wanted) && !is_own_nick(state, user->nick))
				g_ptr_array_add(composer->matches, g_strdup(user->nick));
		}
	}
	if (composer->room->kind == ROOM_PRIVATE) {
		g_autofree char *name = g_utf8_casefold(composer->room->name, -1);
		if (g_str_has_prefix(name, wanted))
			g_ptr_array_add(composer->matches, g_strdup(composer->room->name));
	}
	g_autoptr(GHashTable) ranks = rank_recent_speakers(composer->room, composer->matches);
	g_ptr_array_sort_with_data(composer->matches, compare_by_recent_speech, ranks);
	composer->match_index = 0;
}


/* Tab finishes the nick before the cursor, with a colon when it starts the message, and Tab again cycles */
static void complete_nick(Composer *composer) {
	GtkTextIter cursor, start;
	gtk_text_buffer_get_iter_at_mark(composer->buffer, &cursor, gtk_text_buffer_get_insert(composer->buffer));
	if (!composer->matches) {
		start = cursor;
		while (!gtk_text_iter_starts_line(&start)) {
			gtk_text_iter_backward_char(&start);
			if (g_unichar_isspace(gtk_text_iter_get_char(&start))) {
				gtk_text_iter_forward_char(&start);
				break;
			}
		}
		g_autofree char *prefix = gtk_text_iter_get_text(&start, &cursor);
		if (!*prefix)
			return;
		collect_matches(composer, prefix);
		composer->match_start = gtk_text_iter_get_offset(&start);
	} else {
		composer->match_index = (composer->match_index + 1) % MAX(composer->matches->len, 1);
	}
	if (!composer->matches->len)
		return;
	gtk_text_buffer_get_iter_at_offset(composer->buffer, &start, composer->match_start);
	const char *nick = g_ptr_array_index(composer->matches, composer->match_index);
	g_autofree char *inserted = g_strconcat(nick, composer->match_start == 0 ? ": " : " ", NULL);
	gtk_text_buffer_delete(composer->buffer, &start, &cursor);
	gtk_text_buffer_insert(composer->buffer, &start, inserted, -1);
}


/* Deleting past either end of the text does nothing, so the field is kept from answering it with the error sound */
static gboolean is_deleting_past_edge(Composer *composer, guint key) {
	if (gtk_text_buffer_get_has_selection(composer->buffer))
		return FALSE;
	GtkTextIter cursor;
	gtk_text_buffer_get_iter_at_mark(composer->buffer, &cursor, gtk_text_buffer_get_insert(composer->buffer));
	if (key == GDK_KEY_BackSpace)
		return gtk_text_iter_is_start(&cursor);
	if (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete)
		return gtk_text_iter_is_end(&cursor);
	return FALSE;
}


static gboolean press_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state,
	Composer *composer) {
	if (is_deleting_past_edge(composer, key))
		return TRUE;
	if (key != GDK_KEY_Tab && key != GDK_KEY_ISO_Left_Tab)
		clear_matches(composer);
	gboolean has_shift = state & GDK_SHIFT_MASK;
	if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !has_shift) {
		send_field(composer);
		return TRUE;
	}
	if (key == GDK_KEY_Tab && composer->room) {
		complete_nick(composer);
		return TRUE;
	}
	if (key == GDK_KEY_Escape && composer->reply_to) {
		cancel_reply(composer);
		return TRUE;
	}
	return FALSE;
}


static gboolean pause_typing(gpointer data) {
	Composer *composer = data;
	composer->pause_source = 0;
	if (composer->is_typing && composer->room)
		send_typing(composer->room, "paused");
	composer->last_typing = 0;
	return G_SOURCE_REMOVE;
}


/* The send button stays put at the row's end while the field moves aside for it, and the button grows in with a
 * little bounce once the field is halfway there */
static void move_field_aside(double value, gpointer data) {
	Composer *composer = data;
	composer->send_room = value;
	gtk_widget_set_margin_end(composer->field_area, (int)(value * SEND_ROOM + 0.5));
	if (composer->is_send_shown && value >= SEND_GROW_POINT)
		gtk_widget_remove_css_class(composer->send_button, "tucked");
}


/* Going, the button shrinks away at once while the field takes the room back */
static void refresh_send_button(Composer *composer) {
	g_autofree char *text = get_field_text(composer);
	gtk_widget_set_sensitive(composer->send_button, *text && !composer->is_waiting);
	gboolean is_shown = *text || composer->is_spinner_shown;
	if (is_shown == composer->is_send_shown)
		return;
	composer->is_send_shown = is_shown;
	gtk_widget_set_can_target(composer->send_button, is_shown);
	if (!is_shown)
		gtk_widget_add_css_class(composer->send_button, "tucked");
	AdwTimedAnimation *animation = ADW_TIMED_ANIMATION(composer->send_room_animation);
	adw_timed_animation_set_value_from(animation, composer->send_room);
	adw_timed_animation_set_value_to(animation, is_shown ? 1 : 0);
	adw_animation_play(composer->send_room_animation);
}


/* Typing notices go at most every three seconds and say done when the field is emptied */
static void change_text(GtkTextBuffer *buffer, Composer *composer) {
	g_autofree char *text = get_field_text(composer);
	gboolean is_empty = !*text;
	gtk_widget_set_visible(composer->placeholder, is_empty);
	refresh_send_button(composer);
	if (!composer->room || *text == '/')
		return;
	if (is_empty) {
		if (composer->is_typing)
			send_typing(composer->room, "done");
		stop_typing_notice(composer);
		return;
	}
	gint64 now = g_get_monotonic_time();
	if (now - composer->last_typing >= TYPING_INTERVAL_MICROSECONDS) {
		send_typing(composer->room, "active");
		composer->last_typing = now;
		composer->is_typing = TRUE;
	}
	g_clear_handle_id(&composer->pause_source, g_source_remove);
	composer->pause_source = g_timeout_add_seconds(PAUSE_SECONDS, pause_typing, composer);
}


static void insert_emoji(GtkEmojiChooser *chooser, const char *emoji, Composer *composer) {
	gtk_text_buffer_insert_at_cursor(composer->buffer, emoji, -1);
	focus_composer_field(composer);
}


static GtkWidget *create_reply_bar(Composer *composer) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(bar, "reply-bar");
	GtkWidget *icon = gtk_image_new_from_icon_name("mail-reply-sender-symbolic");
	gtk_widget_add_css_class(icon, "accent");
	gtk_box_append(GTK_BOX(bar), icon);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(texts, TRUE);
	composer->reply_name = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(composer->reply_name), 0);
	gtk_widget_add_css_class(composer->reply_name, "reply-name");
	composer->reply_text = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(composer->reply_text), 0);
	gtk_label_set_ellipsize(GTK_LABEL(composer->reply_text), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(composer->reply_text), TRUE);
	gtk_widget_add_css_class(composer->reply_text, "reply-snippet");
	gtk_box_append(GTK_BOX(texts), composer->reply_name);
	gtk_box_append(GTK_BOX(texts), composer->reply_text);
	gtk_box_append(GTK_BOX(bar), texts);
	GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_set_tooltip_text(close, TEXT_CANCEL_REPLY);
	gtk_widget_add_css_class(close, "flat");
	gtk_widget_add_css_class(close, "circular");
	g_signal_connect_swapped(close, "clicked", G_CALLBACK(cancel_reply), composer);
	gtk_box_append(GTK_BOX(bar), close);
	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_child(GTK_REVEALER(revealer), bar);
	return revealer;
}


/* A wrapped line makes the text view scroll to the cursor before the field has grown, and it only learns its new
 * height once that scroll ends, so a scroll while the whole text fits goes back to the top and the view relearns */
static void keep_text_in_view(GtkAdjustment *range, Composer *composer) {
	if (gtk_adjustment_get_value(range) <= 0 || gtk_adjustment_get_upper(range) > gtk_widget_get_height(composer->field))
		return;
	gtk_adjustment_set_value(range, 0);
	gtk_widget_queue_allocate(composer->field);
}




static GtkWidget *create_field(Composer *composer) {
	composer->field = gtk_text_view_new();
	composer->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer->field));
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(composer->field), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(composer->field), FALSE);
	gtk_widget_add_css_class(composer->field, "composer-field");
	composer->change_handler = g_signal_connect(composer->buffer, "changed", G_CALLBACK(change_text), composer);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_key), composer);
	gtk_widget_add_controller(composer->field, keys);
	GtkWidget *scroller = gtk_scrolled_window_new();
	/* An automatic scroll bar would make the empty field as tall as the bar's smallest size */
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), FIELD_MAXIMUM_HEIGHT);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), composer->field);
	GtkAdjustment *range = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
	/* A text view reports a wrapped line only through its scroll range, so the field measures itself again */
	g_signal_connect_swapped(range, "notify::upper", G_CALLBACK(gtk_widget_queue_resize), scroller);
	g_signal_connect(range, "value-changed", G_CALLBACK(keep_text_in_view), composer);
	gtk_widget_set_hexpand(scroller, TRUE);
	gtk_widget_set_valign(scroller, GTK_ALIGN_CENTER);
	composer->placeholder = gtk_label_new(TEXT_WRITE_MESSAGE);
	gtk_label_set_xalign(GTK_LABEL(composer->placeholder), 0);
	gtk_label_set_ellipsize(GTK_LABEL(composer->placeholder), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(composer->placeholder, "composer-placeholder");
	gtk_widget_set_valign(composer->placeholder, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(composer->placeholder, FALSE);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), scroller);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), composer->placeholder);
	gtk_widget_set_hexpand(overlay, TRUE);
	return overlay;
}


Composer *create_composer(void) {
	Composer *composer = g_new0(Composer, 1);
	composer->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(composer->box, "composer");
	composer->reply_bar = create_reply_bar(composer);
	gtk_box_append(GTK_BOX(composer->box), composer->reply_bar);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(row, "composer-row");
	GtkWidget *emoji = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(emoji), "face-smile-symbolic");
	gtk_widget_set_tooltip_text(emoji, TEXT_EMOJI);
	gtk_widget_add_css_class(emoji, "flat");
	gtk_widget_add_css_class(emoji, "circular");
	gtk_widget_add_css_class(emoji, "emoji-button");
	gtk_widget_set_valign(emoji, GTK_ALIGN_END);
	GtkWidget *chooser = gtk_emoji_chooser_new();
	g_signal_connect(chooser, "emoji-picked", G_CALLBACK(insert_emoji), composer);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(emoji), chooser);
	gtk_box_append(GTK_BOX(row), emoji);
	composer->field_area = create_field(composer);
	gtk_box_append(GTK_BOX(row), composer->field_area);
	composer->send_button = gtk_button_new();
	composer->send_icons = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(composer->send_icons), gtk_image_new_from_icon_name("crimson-send-symbolic"), "send");
	GtkWidget *spinner = create_spinner();
	gtk_widget_set_size_request(spinner, SEND_SPINNER_SIZE, SEND_SPINNER_SIZE);
	gtk_stack_add_named(GTK_STACK(composer->send_icons), spinner, "waiting");
	gtk_widget_set_halign(composer->send_icons, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(composer->send_icons, GTK_ALIGN_CENTER);
	gtk_button_set_child(GTK_BUTTON(composer->send_button), composer->send_icons);
	gtk_widget_set_tooltip_text(composer->send_button, TEXT_SEND);
	gtk_widget_add_css_class(composer->send_button, "circular");
	gtk_widget_add_css_class(composer->send_button, "suggested-action");
	gtk_widget_add_css_class(composer->send_button, "send-button");
	gtk_widget_set_valign(composer->send_button, GTK_ALIGN_END);
	gtk_widget_set_sensitive(composer->send_button, FALSE);
	gtk_widget_add_css_class(composer->send_button, "tucked");
	g_signal_connect(composer->send_button, "clicked", G_CALLBACK(click_send), composer);
	gtk_widget_set_halign(composer->send_button, GTK_ALIGN_END);
	gtk_widget_set_can_target(composer->send_button, FALSE);
	GtkWidget *layers = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(layers), row);
	gtk_overlay_add_overlay(GTK_OVERLAY(layers), composer->send_button);
	AdwAnimationTarget *target = adw_callback_animation_target_new(move_field_aside, composer, NULL);
	composer->send_room_animation = adw_timed_animation_new(layers, 0, 1, SEND_ROOM_MS, target);
	adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(composer->send_room_animation), ADW_EASE_OUT_CUBIC);
	gtk_box_append(GTK_BOX(composer->box), layers);
	return composer;
}


GtkWidget *get_composer_widget(Composer *composer) {
	return composer->box;
}


void switch_composer_room(Composer *composer, CrimsonRoom *room) {
	if (composer->room) {
		g_autofree char *text = get_field_text(composer);
		g_object_set_data_full(G_OBJECT(composer->room), DRAFTS_KEY, g_strdup(text), g_free);
		if (composer->is_typing)
			send_typing(composer->room, "done");
	}
	stop_typing_notice(composer);
	cancel_reply(composer);
	clear_matches(composer);
	g_set_object(&composer->room, room);
	const char *draft = room ? g_object_get_data(G_OBJECT(room), DRAFTS_KEY) : NULL;
	g_signal_handler_block(composer->buffer, composer->change_handler);
	gtk_text_buffer_set_text(composer->buffer, draft ? draft : "", -1);
	g_signal_handler_unblock(composer->buffer, composer->change_handler);
	gtk_widget_set_visible(composer->placeholder, !draft || !*draft);
	refresh_send_button(composer);
	gtk_label_set_text(GTK_LABEL(composer->placeholder), room && room->kind == ROOM_SERVER ? TEXT_SEND_COMMAND
		: TEXT_WRITE_MESSAGE);
}


void focus_composer_field(Composer *composer) {
	gtk_widget_grab_focus(composer->field);
}


void mention_in_composer(Composer *composer, const char *nick) {
	g_autofree char *text = get_field_text(composer);
	g_autofree char *mention = *text ? g_strdup_printf("%s ", nick) : g_strdup_printf("%s: ", nick);
	gtk_text_buffer_insert_at_cursor(composer->buffer, mention, -1);
	gtk_widget_grab_focus(composer->field);
}


static void show_send_look(Composer *composer, gboolean is_waiting) {
	composer->is_spinner_shown = is_waiting;
	gtk_stack_set_visible_child_name(GTK_STACK(composer->send_icons), is_waiting ? "waiting" : "send");
	if (is_waiting)
		gtk_widget_add_css_class(composer->send_button, "waiting");
	else
		gtk_widget_remove_css_class(composer->send_button, "waiting");
	refresh_send_button(composer);
}


static gboolean show_send_waiting(gpointer data) {
	Composer *composer = data;
	composer->wait_source = 0;
	show_send_look(composer, TRUE);
	return G_SOURCE_REMOVE;
}


void set_composer_waiting(Composer *composer, const char *message) {
	gboolean is_waiting = message != NULL;
	gtk_widget_set_tooltip_text(composer->send_button, message ? message : TEXT_SEND);
	if (composer->is_waiting == is_waiting)
		return;
	composer->is_waiting = is_waiting;
	/* Sending stops at once, while the button only turns into a spinner once the wait has lasted */
	g_clear_handle_id(&composer->wait_source, g_source_remove);
	if (is_waiting)
		composer->wait_source = g_timeout_add(WAIT_SHOWN_MS, show_send_waiting, composer);
	else
		show_send_look(composer, FALSE);
	refresh_send_button(composer);
}
