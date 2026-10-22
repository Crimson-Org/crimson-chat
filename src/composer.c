#include <string.h>

#include "../lang/en.h"
#include "composer.h"
#include "formatting.h"
#include "session.h"

#define TYPING_INTERVAL_MICROSECONDS (3 * G_USEC_PER_SEC)
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
	guint pause_source;
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
	if (!composer->room || strspn(text, " \n\t") == strlen(text))
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
	g_ptr_array_sort_values(composer->matches, (GCompareFunc)g_ascii_strcasecmp);
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


static gboolean press_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state,
	Composer *composer) {
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


/* Typing notices go at most every three seconds and say done when the field is emptied */
static void change_text(GtkTextBuffer *buffer, Composer *composer) {
	g_autofree char *text = get_field_text(composer);
	gboolean is_empty = !*text;
	gtk_widget_set_visible(composer->placeholder, is_empty);
	gtk_widget_set_sensitive(composer->send_button, !is_empty);
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
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(row, "composer-row");
	GtkWidget *emoji = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(emoji), "face-smile-symbolic");
	gtk_widget_set_tooltip_text(emoji, TEXT_EMOJI);
	gtk_widget_add_css_class(emoji, "flat");
	gtk_widget_add_css_class(emoji, "circular");
	gtk_widget_set_valign(emoji, GTK_ALIGN_END);
	GtkWidget *chooser = gtk_emoji_chooser_new();
	g_signal_connect(chooser, "emoji-picked", G_CALLBACK(insert_emoji), composer);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(emoji), chooser);
	gtk_box_append(GTK_BOX(row), emoji);
	gtk_box_append(GTK_BOX(row), create_field(composer));
	composer->send_button = gtk_button_new_from_icon_name("crimson-send-symbolic");
	gtk_widget_set_tooltip_text(composer->send_button, TEXT_SEND);
	gtk_widget_add_css_class(composer->send_button, "circular");
	gtk_widget_add_css_class(composer->send_button, "suggested-action");
	gtk_widget_add_css_class(composer->send_button, "send-button");
	gtk_widget_set_valign(composer->send_button, GTK_ALIGN_END);
	gtk_widget_set_sensitive(composer->send_button, FALSE);
	g_signal_connect(composer->send_button, "clicked", G_CALLBACK(click_send), composer);
	gtk_box_append(GTK_BOX(row), composer->send_button);
	gtk_box_append(GTK_BOX(composer->box), row);
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
	gtk_widget_set_sensitive(composer->send_button, draft && *draft);
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
