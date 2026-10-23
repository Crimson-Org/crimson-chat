#include <string.h>

#include "../lang/en.h"
#include "dialogs.h"
#include "formatting.h"
#include "images.h"
#include "message-row.h"
#include "room-look.h"
#include "session.h"
#include "settings.h"
#include "pictures.h"

#define GROUP_GAP_MICROSECONDS (5 * 60 * G_USEC_PER_SEC)
#define TEXT_WIDTH_CHARS 62
#define PREVIEW_WIDTH 320
#define PREVIEW_HEIGHT 220
#define REPLY_SNIPPET_CHARS 80
#define PERSON_KEY "person"
#define AVATAR_SLOT_KEY "avatar-slot"
#define DAY_PILL_KEY "day-pill"
#define DAY_TIME_KEY "day-time"
/* Lines up to this long keep their time on the same line */
#define INLINE_TIME_CHARS 40

static const char *const QUICK_REACTIONS[] = {"👍", "❤️", "😂", "😮", "😢", "🙏", NULL};

/* What a bubble's buttons and menus need, kept alive as long as the row shows the message */
typedef struct {
	CrimsonMessage *message;
	const MessageActions *actions;
	GtkWidget *bubble;
} RowContext;


GtkWidget *create_message_row(void) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(row, "message-row");
	return row;
}


static void clear_children(GtkWidget *box) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(box)))
		gtk_box_remove(GTK_BOX(box), child);
}


gboolean starts_message_group(CrimsonMessage *message, CrimsonMessage *previous, gboolean starts_unread) {
	if (starts_unread || !previous || previous->kind == MESSAGE_EVENT || previous->kind == MESSAGE_STATUS)
		return TRUE;
	if (g_strcmp0(previous->sender, message->sender) != 0 || message->time - previous->time > GROUP_GAP_MICROSECONDS)
		return TRUE;
	return !is_same_day(previous->time, message->time);
}


static GtkWidget *create_pill(const char *text, const char *css_class) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(label, css_class);
	return label;
}


/* A thin line with a dim label between the messages already read and the new ones */
static GtkWidget *create_read_line(void) {
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(line, "read-line");
	GtkWidget *before = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_hexpand(before, TRUE);
	gtk_widget_set_valign(before, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(line), before);
	GtkWidget *label = gtk_label_new(TEXT_LAST_READ);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(line), label);
	GtkWidget *after = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_hexpand(after, TRUE);
	gtk_widget_set_valign(after, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(line), after);
	return line;
}


static void add_note(GtkWidget *row, CrimsonMessage *message, const char *notes) {
	if (message->kind == MESSAGE_EVENT) {
		GtkWidget *pill = create_pill(notes, "event-pill");
		if (strchr(notes, '\n'))
			gtk_widget_add_css_class(pill, "merged");
		gtk_box_append(GTK_BOX(row), pill);
		return;
	}
	g_autofree char *plain = strip_irc_formatting(message->text);
	g_autofree char *time = format_clock_time(message->time);
	g_autofree char *line = g_strdup_printf("%s  %s", time, plain);
	GtkWidget *label = gtk_label_new(line);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_widget_add_css_class(label, "status-line");
	gtk_box_append(GTK_BOX(row), label);
}


static void pick_reaction(GtkButton *button, RowContext *context) {
	const char *reaction = g_object_get_data(G_OBJECT(button), "reaction");
	GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	if (popover)
		gtk_popover_popdown(GTK_POPOVER(popover));
	context->actions->react(context->message, reaction, context->actions->data);
}


static void pick_emoji(GtkEmojiChooser *chooser, const char *emoji, RowContext *context) {
	context->actions->react(context->message, emoji, context->actions->data);
}


static void open_emoji_chooser(GtkButton *button, RowContext *context) {
	GtkWidget *menu = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	if (menu)
		gtk_popover_popdown(GTK_POPOVER(menu));
	GtkWidget *chooser = gtk_emoji_chooser_new();
	gtk_widget_set_parent(chooser, context->bubble);
	g_signal_connect(chooser, "emoji-picked", G_CALLBACK(pick_emoji), context);
	g_signal_connect(chooser, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
	gtk_popover_popup(GTK_POPOVER(chooser));
}


static void run_menu_action(GtkButton *button, RowContext *context) {
	const char *name = g_object_get_data(G_OBJECT(button), "action");
	GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	if (popover)
		gtk_popover_popdown(GTK_POPOVER(popover));
	if (g_str_equal(name, "reply")) {
		context->actions->reply(context->message, context->actions->data);
	} else if (g_str_equal(name, "copy")) {
		g_autofree char *plain = strip_irc_formatting(context->message->text);
		gdk_clipboard_set_text(gtk_widget_get_clipboard(context->bubble), plain);
	} else if (g_str_equal(name, "delete")) {
		context->actions->remove(context->message, context->actions->data);
	}
}


static GtkWidget *create_menu_button(const char *label, const char *icon, const char *action, RowContext *context) {
	GtkWidget *button = gtk_button_new();
	GtkWidget *content = adw_button_content_new();
	adw_button_content_set_label(ADW_BUTTON_CONTENT(content), label);
	adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
	gtk_widget_set_halign(content, GTK_ALIGN_START);
	gtk_button_set_child(GTK_BUTTON(button), content);
	gtk_widget_add_css_class(button, "flat");
	g_object_set_data(G_OBJECT(button), "action", (gpointer)action);
	g_signal_connect(button, "clicked", G_CALLBACK(run_menu_action), context);
	return button;
}


static GtkWidget *create_reaction_strip(RowContext *context) {
	GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_add_css_class(strip, "reaction-strip");
	/* The reactions already given stand out, since picking one again takes it back */
	CrimsonRoom *room = g_object_get_data(G_OBJECT(context->bubble), "room");
	IrcState *state = room ? get_network_state(room->network) : NULL;
	for (int index = 0; QUICK_REACTIONS[index]; index++) {
		GtkWidget *button = gtk_button_new_with_label(QUICK_REACTIONS[index]);
		gtk_widget_add_css_class(button, "flat");
		gtk_widget_add_css_class(button, "circular");
		gtk_widget_add_css_class(button, "quick-reaction");
		if (state && state->nick && has_reacted(context->message, QUICK_REACTIONS[index], state->nick)) {
			gtk_widget_add_css_class(button, "mine");
			gtk_widget_set_tooltip_text(button, TEXT_TAKE_BACK_REACTION);
		}
		g_object_set_data(G_OBJECT(button), "reaction", (gpointer)QUICK_REACTIONS[index]);
		g_signal_connect(button, "clicked", G_CALLBACK(pick_reaction), context);
		gtk_box_append(GTK_BOX(strip), button);
	}
	GtkWidget *more = gtk_button_new_from_icon_name("face-smile-symbolic");
	gtk_widget_set_tooltip_text(more, TEXT_MORE_REACTIONS);
	gtk_widget_add_css_class(more, "flat");
	gtk_widget_add_css_class(more, "circular");
	g_signal_connect(more, "clicked", G_CALLBACK(open_emoji_chooser), context);
	gtk_box_append(GTK_BOX(strip), more);
	return strip;
}


/* The menu a right click or a long press opens, with quick reactions on top as in other messengers */
static void open_message_menu(RowContext *context, double x, double y) {
	CrimsonMessage *message = context->message;
	CrimsonRoom *room = g_object_get_data(G_OBJECT(context->bubble), "room");
	GtkWidget *popover = gtk_popover_new();
	gtk_widget_add_css_class(popover, "message-menu");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	/* Reactions only show where the server passes them on to others */
	gboolean can_react = message->id && room && can_network(room->network, "message-tags")
		&& allows_client_tag(room->network, "draft/react");
	if (can_react) {
		gtk_box_append(GTK_BOX(box), create_reaction_strip(context));
		gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	}
	if (room && room->kind != ROOM_SERVER)
		gtk_box_append(GTK_BOX(box), create_menu_button(TEXT_REPLY, "mail-reply-sender-symbolic", "reply", context));
	gtk_box_append(GTK_BOX(box), create_menu_button(TEXT_COPY_TEXT, "edit-copy-symbolic", "copy", context));
	if (message->id && room && can_network(room->network, "draft/message-redaction") && !message->is_redacted)
		gtk_box_append(GTK_BOX(box), create_menu_button(TEXT_DELETE, "user-trash-symbolic", "delete", context));
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	gtk_widget_set_parent(popover, context->bubble);
	GdkRectangle point = {(int)x, (int)y, 1, 1};
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &point);
	g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


static void press_bubble(GtkGestureClick *gesture, int presses, double x, double y, RowContext *context) {
	/* The sender's name inside the bubble has its own menu about the person */
	for (GtkWidget *picked = gtk_widget_pick(context->bubble, x, y, GTK_PICK_DEFAULT); picked;
		picked = gtk_widget_get_parent(picked)) {
		if (g_object_get_data(G_OBJECT(picked), PERSON_KEY))
			return;
		if (picked == context->bubble)
			break;
	}
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	open_message_menu(context, x, y);
}


static void hold_bubble(GtkGestureLongPress *gesture, double x, double y, RowContext *context) {
	open_message_menu(context, x, y);
}


static void free_context(gpointer data) {
	RowContext *context = data;
	g_object_unref(context->message);
	g_free(context);
}


static void add_menu_gestures(GtkWidget *bubble, RowContext *context) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	/* The bubble's menu comes before the text label's own copy and paste menu */
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
	g_signal_connect(click, "pressed", G_CALLBACK(press_bubble), context);
	gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(click));
	GtkGesture *hold = gtk_gesture_long_press_new();
	gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(hold), TRUE);
	g_signal_connect(hold, "pressed", G_CALLBACK(hold_bubble), context);
	gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(hold));
}


static void show_replied_message(GtkGestureClick *gesture, int presses, double x, double y, RowContext *context) {
	context->actions->show_message(context->message->reply_to, context->actions->data);
}


/* Where a server strips reply tags, a reply starts with >"Name: the start of their message" and its own text, which
 * shows as the same quote a tagged reply gets */
typedef struct {
	char *sender, *excerpt;
	const char *rest;
} TextQuote;


static gboolean read_text_quote(const char *text, TextQuote *quote) {
	if (!text || !g_str_has_prefix(text, ">\""))
		return FALSE;
	const char *start = text + 2;
	const char *colon = strstr(start, ": ");
	const char *close = colon ? strstr(colon + 2, "\" ") : NULL;
	if (!colon || !close || colon == start || memchr(start, ' ', (size_t)(colon - start)))
		return FALSE;
	quote->sender = g_strndup(start, (gsize)(colon - start));
	quote->excerpt = g_strndup(colon + 2, (gsize)(close - colon - 2));
	quote->rest = close + 2;
	return TRUE;
}


static void clear_text_quote(TextQuote *quote) {
	g_free(quote->sender);
	g_free(quote->excerpt);
}


static GtkWidget *create_reply_quote(CrimsonMessage *message, CrimsonRoom *room, RowContext *context,
	TextQuote *text_quote) {
	CrimsonMessage *original = message->reply_to ? find_room_message(room, message->reply_to) : NULL;
	const char *sender = text_quote ? text_quote->sender : original ? original->sender : NULL;
	GtkWidget *quote = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(quote, "reply-quote");
	GtkWidget *name = gtk_label_new(sender ? sender : TEXT_EARLIER_MESSAGE);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_add_css_class(name, "reply-name");
	if (sender) {
		g_autofree char *color = g_strdup_printf("name-color-%d", pick_name_color(sender));
		gtk_widget_add_css_class(quote, color);
	}
	g_autofree char *plain = original ? strip_irc_formatting(original->text) : text_quote
		? g_strdup(text_quote->excerpt) : g_strdup(TEXT_NOT_LOADED);
	GtkWidget *snippet = gtk_label_new(plain);
	gtk_label_set_xalign(GTK_LABEL(snippet), 0);
	gtk_label_set_ellipsize(GTK_LABEL(snippet), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(snippet), REPLY_SNIPPET_CHARS);
	gtk_label_set_single_line_mode(GTK_LABEL(snippet), TRUE);
	gtk_widget_add_css_class(snippet, "reply-snippet");
	gtk_box_append(GTK_BOX(quote), name);
	gtk_box_append(GTK_BOX(quote), snippet);
	if (!message->reply_to)
		return quote;
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(show_replied_message), context);
	gtk_widget_add_controller(quote, GTK_EVENT_CONTROLLER(click));
	gtk_widget_set_cursor_from_name(quote, "pointer");
	return quote;
}


static char *build_text_markup(CrimsonMessage *message, const char *text) {
	/* Markdown stars and underscores from other apps show as the emphasis they stand for */
	g_autofree char *emphasised = convert_markdown(text);
	g_autofree char *markup = format_irc_markup(emphasised);
	if (message->kind != MESSAGE_ACTION)
		return g_steal_pointer(&markup);
	g_autofree char *sender = g_markup_escape_text(message->sender ? message->sender : "", -1);
	return g_strdup_printf("<i>%s %s</i>", sender, markup);
}


static char *build_body_markup(CrimsonMessage *message, const char *text) {
	if (message->is_redacted)
		return g_markup_printf_escaped("<i>%s</i>", TEXT_MESSAGE_DELETED);
	return build_text_markup(message, text);
}


static void toggle_deleted_text(GtkToggleButton *button, GtkLabel *body) {
	gboolean is_shown = gtk_toggle_button_get_active(button);
	gtk_label_set_markup(body, g_object_get_data(G_OBJECT(button), is_shown ? "original" : "note"));
	gtk_button_set_icon_name(GTK_BUTTON(button), is_shown ? "view-conceal-symbolic" : "view-reveal-symbolic");
	gtk_widget_set_tooltip_text(GTK_WIDGET(button), is_shown ? TEXT_HIDE_DELETED : TEXT_SHOW_DELETED);
}


/* Deleting only marks a message, and both the chat and the bouncer keep its text, so an eye beside the note shows
 * what it said */
static GtkWidget *add_deleted_reveal(GtkWidget *body, CrimsonMessage *message, const char *text) {
	if (!message->is_redacted || !*text)
		return body;
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(button), "view-reveal-symbolic");
	gtk_widget_set_tooltip_text(button, TEXT_SHOW_DELETED);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_add_css_class(button, "reveal-button");
	gtk_widget_set_valign(button, GTK_ALIGN_START);
	g_object_set_data_full(G_OBJECT(button), "original", build_text_markup(message, text), g_free);
	g_object_set_data_full(G_OBJECT(button), "note", build_body_markup(message, text), g_free);
	g_signal_connect(button, "toggled", G_CALLBACK(toggle_deleted_text), body);
	gtk_box_append(GTK_BOX(line), body);
	gtk_box_append(GTK_BOX(line), button);
	return line;
}


static void free_link(gpointer url, GClosure *closure) {
	g_free(url);
}


static void open_preview(GtkGestureClick *gesture, int presses, double x, double y, gpointer url) {
	confirm_open_link(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), url);
}


static void refresh_message(GObject *owner) {
	announce_message_change(CRIMSON_MESSAGE(owner));
}


static GtkWidget *wrap_in_clamp(GtkWidget *child, GtkOrientation orientation, int size) {
	GtkWidget *clamp = adw_clamp_new();
	gtk_orientable_set_orientation(GTK_ORIENTABLE(clamp), orientation);
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), size);
	adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), size);
	adw_clamp_set_child(ADW_CLAMP(clamp), child);
	return clamp;
}


/* A picture shows once it has loaded, scaled to fit the preview box, and its message is redrawn then */
static GtkWidget *create_preview(const char *url, GdkTexture *texture) {
	double width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
	double scale = MIN(1.0, MIN(PREVIEW_WIDTH / width, PREVIEW_HEIGHT / height));
	GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
	gtk_widget_set_size_request(picture, (int)(width * scale), (int)(height * scale));
	gtk_widget_add_css_class(picture, "image-preview");
	gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
	gtk_widget_set_cursor_from_name(picture, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect_data(click, "released", G_CALLBACK(open_preview), g_strdup(url), free_link, 0);
	gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
	GtkWidget *frame = wrap_in_clamp(wrap_in_clamp(picture, GTK_ORIENTATION_HORIZONTAL, (int)(width * scale)),
		GTK_ORIENTATION_VERTICAL, (int)(height * scale));
	gtk_widget_set_halign(frame, GTK_ALIGN_START);
	return frame;
}


static void add_previews(GtkWidget *bubble, CrimsonMessage *message) {
	if (!get_settings()->shows_images || message->is_redacted)
		return;
	g_autoptr(GPtrArray) links = find_image_links(message->text);
	for (guint index = 0; index < links->len; index++) {
		const char *url = g_ptr_array_index(links, index);
		GdkTexture *texture = get_loaded_image(url);
		if (texture)
			gtk_box_append(GTK_BOX(bubble), create_preview(url, texture));
		else
			preload_image(url, refresh_message, G_OBJECT(message));
	}
}


static GtkWidget *create_footer(CrimsonMessage *message) {
	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_halign(footer, GTK_ALIGN_END);
	gtk_widget_add_css_class(footer, "message-footer");
	if (message->is_bot) {
		GtkWidget *bot = gtk_label_new(TEXT_BOT);
		gtk_widget_add_css_class(bot, "bot-tag");
		gtk_box_append(GTK_BOX(footer), bot);
	}
	g_autofree char *moment = format_clock_time(message->time);
	gtk_box_append(GTK_BOX(footer), gtk_label_new(moment));
	if (message->is_own) {
		GtkWidget *status = gtk_image_new_from_icon_name(message->is_pending ? "crimson-clock-symbolic"
			: "crimson-sent-symbolic");
		gtk_widget_set_tooltip_text(status, message->is_pending ? TEXT_SENDING : TEXT_SENT);
		gtk_box_append(GTK_BOX(footer), status);
	}
	return footer;
}


static void tap_reaction(GtkButton *button, RowContext *context) {
	context->actions->react(context->message, g_object_get_data(G_OBJECT(button), "reaction"),
		context->actions->data);
}


static GtkWidget *create_reactions(CrimsonMessage *message, CrimsonRoom *room, RowContext *context) {
	GtkWidget *box = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 8);
	gtk_widget_add_css_class(box, "reactions");
	IrcState *state = get_network_state(room->network);
	for (guint index = 0; index < message->reaction_order->len; index++) {
		const char *reaction = g_ptr_array_index(message->reaction_order, index);
		GPtrArray *nicks = g_hash_table_lookup(message->reactions, reaction);
		g_autofree char *label = g_strdup_printf("%s %u", reaction, nicks->len);
		GtkWidget *button = gtk_button_new_with_label(label);
		gtk_widget_add_css_class(button, "reaction-pill");
		if (state && state->nick && has_reacted(message, reaction, state->nick))
			gtk_widget_add_css_class(button, "mine");
		g_ptr_array_add(nicks, NULL);
		g_autofree char *who = g_strjoinv(", ", (char **)nicks->pdata);
		g_ptr_array_remove_index(nicks, nicks->len - 1);
		gtk_widget_set_tooltip_text(button, who);
		g_object_set_data_full(G_OBJECT(button), "reaction", g_strdup(reaction), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(tap_reaction), context);
		gtk_flow_box_append(GTK_FLOW_BOX(box), button);
	}
	return box;
}


static void click_person(GtkGestureClick *gesture, int presses, double x, double y, RowContext *context) {
	context->actions->show_person(context->message->sender, context->actions->data);
}


static void press_person(GtkGestureClick *gesture, int presses, double x, double y, RowContext *context) {
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	context->actions->person_menu(widget, context->message->sender, x, y, context->actions->data);
}


/* A person's name or picture opens their profile on a click and their menu on a right click */
static void make_person_target(GtkWidget *widget, RowContext *context) {
	g_object_set_data(G_OBJECT(widget), PERSON_KEY, GINT_TO_POINTER(TRUE));
	gtk_widget_set_cursor_from_name(widget, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(click_person), context);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
	GtkGesture *menu = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu, "pressed", G_CALLBACK(press_person), context);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(menu));
}


static GtkWidget *create_sender_label(CrimsonMessage *message, RowContext *context) {
	GtkWidget *name = gtk_label_new(message->sender);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_add_css_class(name, "sender");
	g_autofree char *color = g_strdup_printf("name-color-%d", pick_name_color(message->sender));
	gtk_widget_add_css_class(name, color);
	gtk_widget_set_halign(name, GTK_ALIGN_START);
	make_person_target(name, context);
	return name;
}


/* A mention in a channel is marked in the accent colour along the bubble's side, tail included */
static gboolean is_marked_mention(CrimsonMessage *message, CrimsonRoom *room) {
	return message->is_highlight && room->kind == ROOM_CHANNEL;
}


static GtkWidget *create_bubble(CrimsonMessage *message, CrimsonRoom *room, gboolean is_first, RowContext *context) {
	GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_add_css_class(bubble, "bubble");
	if (message->is_own)
		gtk_widget_add_css_class(bubble, "own");
	if (is_marked_mention(message, room))
		gtk_widget_add_css_class(bubble, "mention");
	if (message->kind == MESSAGE_NOTICE)
		gtk_widget_add_css_class(bubble, "notice");
	if (is_first)
		gtk_widget_add_css_class(bubble, "first");
	g_object_set_data(G_OBJECT(bubble), "room", room);
	context->bubble = bubble;
	gboolean shows_name = is_first && !message->is_own && message->sender && room->kind == ROOM_CHANNEL
		&& message->kind != MESSAGE_ACTION;
	if (shows_name)
		gtk_box_append(GTK_BOX(bubble), create_sender_label(message, context));
	TextQuote text_quote = {0};
	gboolean has_text_quote = message->kind == MESSAGE_TEXT && read_text_quote(message->text, &text_quote);
	if (message->reply_to || has_text_quote)
		gtk_box_append(GTK_BOX(bubble), create_reply_quote(message, room, context, has_text_quote ? &text_quote
			: NULL));
	g_autofree char *text = g_strdup(has_text_quote ? text_quote.rest : message->text ? message->text : "");
	if (has_text_quote)
		clear_text_quote(&text_quote);
	g_autofree char *markup = build_body_markup(message, text);
	GtkWidget *body = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(body), markup);
	ask_before_label_links(body);
	gtk_label_set_wrap(GTK_LABEL(body), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(body), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(body), TEXT_WIDTH_CHARS);
	gtk_label_set_selectable(GTK_LABEL(body), TRUE);
	gtk_widget_set_focusable(body, FALSE);
	gtk_widget_add_css_class(body, message->is_redacted ? "redacted" : "body");
	GtkWidget *content = add_deleted_reveal(body, message, text);
	/* A short line keeps its time beside it, as in Telegram, while longer text and pictures put it underneath */
	gboolean is_short = !strchr(text, '\n') && g_utf8_strlen(text, -1) <= INLINE_TIME_CHARS && !strstr(text, "://");
	if (is_short) {
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_box_append(GTK_BOX(line), content);
		GtkWidget *footer = create_footer(message);
		gtk_widget_set_valign(footer, GTK_ALIGN_END);
		gtk_widget_set_hexpand(footer, TRUE);
		gtk_box_append(GTK_BOX(line), footer);
		gtk_box_append(GTK_BOX(bubble), line);
	} else {
		gtk_box_append(GTK_BOX(bubble), content);
		add_previews(bubble, message);
		gtk_box_append(GTK_BOX(bubble), create_footer(message));
	}
	add_menu_gestures(bubble, context);
	return bubble;
}


/* The tail lies over the bubble's edge rather than beside it, so the bubble's shadow never falls across it */
static GtkWidget *create_tail(CrimsonMessage *message, CrimsonRoom *room) {
	GtkWidget *tail = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(tail, "bubble-tail");
	if (message->is_own)
		gtk_widget_add_css_class(tail, "own");
	if (is_marked_mention(message, room))
		gtk_widget_add_css_class(tail, "mention");
	gtk_widget_set_halign(tail, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	gtk_widget_set_valign(tail, GTK_ALIGN_START);
	gtk_widget_set_can_target(tail, FALSE);
	return tail;
}


static GtkWidget *create_message_line(CrimsonMessage *message, CrimsonRoom *room, gboolean is_first,
	RowContext *context) {
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(line, "message-line");
	gtk_widget_set_halign(line, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	gboolean has_avatars = room->kind == ROOM_CHANNEL && !message->is_own;
	if (has_avatars && is_first && message->sender) {
		GtkWidget *avatar = adw_avatar_new(MESSAGE_AVATAR_SIZE, message->sender, TRUE);
		gtk_widget_set_valign(avatar, GTK_ALIGN_START);
		const char *url = pick_person_picture(room, message->sender);
		g_autofree char *sized = fill_image_size(url, MESSAGE_AVATAR_SIZE * 2);
		show_avatar_image(ADW_AVATAR(avatar), sized);
		gtk_widget_set_tooltip_text(avatar, message->sender);
		gtk_widget_add_css_class(avatar, "message-avatar");
		make_person_target(avatar, context);
		gtk_box_append(GTK_BOX(line), avatar);
		g_object_set_data(G_OBJECT(line), AVATAR_SLOT_KEY, avatar);
	} else if (has_avatars) {
		GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_size_request(spacer, MESSAGE_AVATAR_SIZE, -1);
		gtk_box_append(GTK_BOX(line), spacer);
		g_object_set_data(G_OBJECT(line), AVATAR_SLOT_KEY, spacer);
	}
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *tailed = gtk_overlay_new();
	gtk_widget_set_halign(tailed, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	/* A message arriving while its chat is open rises in with its tail */
	if (is_message_arriving(message))
		gtk_widget_add_css_class(tailed, "fresh-message");
	gtk_overlay_set_child(GTK_OVERLAY(tailed), create_bubble(message, room, is_first, context));
	if (is_first)
		gtk_overlay_add_overlay(GTK_OVERLAY(tailed), create_tail(message, room));
	gtk_box_append(GTK_BOX(column), tailed);
	if (message->reaction_order->len) {
		GtkWidget *reactions = create_reactions(message, room, context);
		gtk_widget_set_halign(reactions, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(column), reactions);
	}
	gtk_box_append(GTK_BOX(line), column);
	return line;
}


gboolean continues_note_run(CrimsonMessage *message, CrimsonMessage *previous, gboolean starts_unread) {
	return message->kind == MESSAGE_EVENT && previous && previous->kind == MESSAGE_EVENT && !starts_unread
		&& is_same_day(previous->time, message->time);
}


void fill_message_row(GtkWidget *row, CrimsonMessage *message, const RowPlace *place, CrimsonRoom *room,
	const MessageActions *actions) {
	CrimsonMessage *previous = place->previous;
	gboolean starts_unread = place->starts_unread;
	clear_children(row);
	g_object_set_data(G_OBJECT(row), DAY_PILL_KEY, NULL);
	gtk_widget_remove_css_class(row, "day-start");
	g_object_set_data(G_OBJECT(row), AVATAR_SLOT_KEY, NULL);
	/* An event inside a run is already a line of the pill on the run's first row */
	gboolean is_folded = message->kind == MESSAGE_EVENT && !place->notes;
	if (is_folded)
		gtk_widget_add_css_class(row, "folded-note");
	else
		gtk_widget_remove_css_class(row, "folded-note");
	if (is_folded)
		return;
	gboolean is_new_day = !previous || !is_same_day(previous->time, message->time);
	gboolean shows_day = is_new_day && message->kind != MESSAGE_STATUS;
	if (shows_day) {
		g_autofree char *day = format_day_heading(message->time);
		GtkWidget *pill = create_pill(day, "day-pill");
		/* The pill keeps its moment, so a new day only changes its words rather than the whole row */
		g_object_set_data_full(G_OBJECT(pill), DAY_TIME_KEY, g_memdup2(&message->time, sizeof message->time), g_free);
		g_object_set_data(G_OBJECT(row), DAY_PILL_KEY, pill);
		gtk_box_append(GTK_BOX(row), pill);
		gtk_widget_add_css_class(row, "day-start");
	}
	if (starts_unread)
		gtk_box_append(GTK_BOX(row), create_read_line());
	if (message->kind == MESSAGE_EVENT || message->kind == MESSAGE_STATUS) {
		add_note(row, message, place->notes);
		return;
	}
	RowContext *context = g_new0(RowContext, 1);
	context->message = g_object_ref(message);
	context->actions = actions;
	g_object_set_data_full(G_OBJECT(row), "context", context, free_context);
	/* The new messages start a group of their own, so the first one shows who wrote it again */
	gboolean is_first = starts_message_group(message, previous, starts_unread);
	if (is_first)
		gtk_widget_add_css_class(row, "group-start");
	else
		gtk_widget_remove_css_class(row, "group-start");
	GtkWidget *line = create_message_line(message, room, is_first, context);
	g_object_set_data(G_OBJECT(row), AVATAR_SLOT_KEY, g_object_get_data(G_OBJECT(line), AVATAR_SLOT_KEY));
	gtk_box_append(GTK_BOX(row), line);
	/* Only a group's last bubble rounds its lower corner on the avatar's side */
	if (place->ends_group)
		gtk_widget_add_css_class(context->bubble, "group-end");
}


GtkWidget *find_avatar_slot(GtkWidget *row) {
	return g_object_get_data(G_OBJECT(row), AVATAR_SLOT_KEY);
}


void refresh_day_heading(GtkWidget *row) {
	GtkWidget *pill = g_object_get_data(G_OBJECT(row), DAY_PILL_KEY);
	const gint64 *time = pill ? g_object_get_data(G_OBJECT(pill), DAY_TIME_KEY) : NULL;
	if (!time)
		return;
	g_autofree char *day = format_day_heading(*time);
	gtk_label_set_text(GTK_LABEL(pill), day);
}


gboolean is_group_start(GtkWidget *row) {
	return gtk_widget_has_css_class(row, "group-start");
}
