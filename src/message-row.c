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
#define PARTS_KEY "parts"
/* Lines up to this long keep their time on the same line */
#define INLINE_TIME_CHARS 40

static const char *const QUICK_REACTIONS[] = {"👍", "❤️", "😂", "😮", "😢", "🙏", NULL};

/* What a bubble's buttons and menus need, kept alive as long as the row shows the message */
typedef struct {
	CrimsonMessage *message;
	const MessageActions *actions;
	GtkWidget *bubble;
} RowContext;


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


static void add_previews(GtkWidget *box, CrimsonMessage *message) {
	if (!get_settings()->shows_images || message->is_redacted)
		return;
	g_autoptr(GPtrArray) links = find_image_links(message->text);
	for (guint index = 0; index < links->len; index++) {
		const char *url = g_ptr_array_index(links, index);
		GdkTexture *texture = get_loaded_image(url);
		if (texture)
			gtk_box_append(GTK_BOX(box), create_preview(url, texture));
		else
			preload_image(url, refresh_message, G_OBJECT(message));
	}
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


/* A mention in a channel is marked in the accent colour along the bubble's side, tail included */
static gboolean is_marked_mention(CrimsonMessage *message, CrimsonRoom *room) {
	return message->is_highlight && room->kind == ROOM_CHANNEL;
}


/* The widgets a row keeps for any message it shows, hidden where that message has no use for them, so the list
 * gives a row another message by changing its text and look instead of building it again */
typedef struct {
	GtkWidget *day_pill, *read_line, *event_pill, *status_line;
	GtkWidget *line, *avatar, *spacer, *tailed, *bubble, *tail, *sender, *quote_slot;
	GtkWidget *text_line, *body, *reveal, *previews, *footer, *bot, *time, *state, *reactions_slot;
	/* The colour class of the sender's name, which the next message's replaces */
	char *name_color;
	RowContext context;
} RowParts;


static void free_parts(gpointer data) {
	RowParts *parts = data;
	g_clear_object(&parts->context.message);
	g_free(parts->name_color);
	g_free(parts);
}


static void set_class(GtkWidget *widget, const char *css_class, gboolean has_class) {
	if (has_class)
		gtk_widget_add_css_class(widget, css_class);
	else
		gtk_widget_remove_css_class(widget, css_class);
}


static GtkWidget *create_wrapping_label(const char *css_class) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_widget_add_css_class(label, css_class);
	return label;
}


static GtkWidget *create_footer(RowParts *parts) {
	parts->footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_halign(parts->footer, GTK_ALIGN_END);
	gtk_widget_add_css_class(parts->footer, "message-footer");
	parts->bot = gtk_label_new(TEXT_BOT);
	gtk_widget_add_css_class(parts->bot, "bot-tag");
	gtk_box_append(GTK_BOX(parts->footer), parts->bot);
	parts->time = gtk_label_new(NULL);
	gtk_box_append(GTK_BOX(parts->footer), parts->time);
	parts->state = gtk_image_new();
	gtk_box_append(GTK_BOX(parts->footer), parts->state);
	return parts->footer;
}


/* Deleting only marks a message, and both the chat and the bouncer keep its text, so an eye beside the note shows
 * what it said */
static GtkWidget *create_text_line(RowParts *parts) {
	parts->text_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	parts->body = create_wrapping_label("body");
	ask_before_label_links(parts->body);
	gtk_label_set_max_width_chars(GTK_LABEL(parts->body), TEXT_WIDTH_CHARS);
	gtk_widget_set_focusable(parts->body, FALSE);
	gtk_box_append(GTK_BOX(content), parts->body);
	parts->reveal = gtk_toggle_button_new();
	gtk_widget_add_css_class(parts->reveal, "flat");
	gtk_widget_add_css_class(parts->reveal, "circular");
	gtk_widget_add_css_class(parts->reveal, "reveal-button");
	gtk_widget_set_valign(parts->reveal, GTK_ALIGN_START);
	g_signal_connect(parts->reveal, "toggled", G_CALLBACK(toggle_deleted_text), parts->body);
	gtk_box_append(GTK_BOX(content), parts->reveal);
	gtk_box_append(GTK_BOX(parts->text_line), content);
	parts->previews = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_box_append(GTK_BOX(parts->text_line), parts->previews);
	gtk_box_append(GTK_BOX(parts->text_line), create_footer(parts));
	return parts->text_line;
}


/* The tail lies over the bubble's edge rather than beside it, so the bubble's shadow never falls across it */
static GtkWidget *create_bubble(RowParts *parts) {
	RowContext *context = &parts->context;
	parts->bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_add_css_class(parts->bubble, "bubble");
	context->bubble = parts->bubble;
	parts->sender = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(parts->sender), 0);
	gtk_widget_add_css_class(parts->sender, "sender");
	gtk_widget_set_halign(parts->sender, GTK_ALIGN_START);
	make_person_target(parts->sender, context);
	gtk_box_append(GTK_BOX(parts->bubble), parts->sender);
	parts->quote_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(parts->bubble), parts->quote_slot);
	gtk_box_append(GTK_BOX(parts->bubble), create_text_line(parts));
	add_menu_gestures(parts->bubble, context);
	parts->tailed = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(parts->tailed), parts->bubble);
	parts->tail = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(parts->tail, "bubble-tail");
	gtk_widget_set_valign(parts->tail, GTK_ALIGN_START);
	gtk_widget_set_can_target(parts->tail, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(parts->tailed), parts->tail);
	return parts->tailed;
}


static GtkWidget *create_message_line(RowParts *parts) {
	parts->line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(parts->line, "message-line");
	parts->avatar = adw_avatar_new(MESSAGE_AVATAR_SIZE, NULL, TRUE);
	gtk_widget_set_valign(parts->avatar, GTK_ALIGN_START);
	gtk_widget_add_css_class(parts->avatar, "message-avatar");
	make_person_target(parts->avatar, &parts->context);
	gtk_box_append(GTK_BOX(parts->line), parts->avatar);
	parts->spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request(parts->spacer, MESSAGE_AVATAR_SIZE, -1);
	gtk_box_append(GTK_BOX(parts->line), parts->spacer);
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(column), create_bubble(parts));
	parts->reactions_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(column), parts->reactions_slot);
	gtk_box_append(GTK_BOX(parts->line), column);
	return parts->line;
}


GtkWidget *create_message_row(void) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(row, "message-row");
	RowParts *parts = g_new0(RowParts, 1);
	g_object_set_data_full(G_OBJECT(row), PARTS_KEY, parts, free_parts);
	parts->day_pill = create_pill(NULL, "day-pill");
	parts->read_line = create_read_line();
	parts->event_pill = create_pill(NULL, "event-pill");
	parts->status_line = create_wrapping_label("status-line");
	GtkWidget *pieces[] = {parts->day_pill, parts->read_line, parts->event_pill, parts->status_line,
		create_message_line(parts)};
	for (guint index = 0; index < G_N_ELEMENTS(pieces); index++) {
		gtk_widget_set_visible(pieces[index], FALSE);
		gtk_box_append(GTK_BOX(row), pieces[index]);
	}
	return row;
}


gboolean continues_note_run(CrimsonMessage *message, CrimsonMessage *previous, gboolean starts_unread) {
	return message->kind == MESSAGE_EVENT && previous && previous->kind == MESSAGE_EVENT && !starts_unread
		&& is_same_day(previous->time, message->time);
}


static void show_day(GtkWidget *row, RowParts *parts, CrimsonMessage *message) {
	gtk_widget_set_visible(parts->day_pill, message != NULL);
	set_class(row, "day-start", message != NULL);
	g_object_set_data(G_OBJECT(row), DAY_PILL_KEY, message ? parts->day_pill : NULL);
	if (!message)
		return;
	g_autofree char *day = format_day_heading(message->time);
	gtk_label_set_text(GTK_LABEL(parts->day_pill), day);
	/* The pill keeps its moment, so a new day only changes its words rather than the whole row */
	g_object_set_data_full(G_OBJECT(parts->day_pill), DAY_TIME_KEY, g_memdup2(&message->time, sizeof message->time),
		g_free);
}


static void show_notes(RowParts *parts, CrimsonMessage *message, const char *notes) {
	gtk_widget_set_visible(parts->event_pill, notes != NULL);
	if (notes) {
		gtk_label_set_text(GTK_LABEL(parts->event_pill), notes);
		set_class(parts->event_pill, "merged", strchr(notes, '\n') != NULL);
	}
	gboolean is_status = message && message->kind == MESSAGE_STATUS;
	gtk_widget_set_visible(parts->status_line, is_status);
	if (!is_status)
		return;
	g_autofree char *plain = strip_irc_formatting(message->text);
	g_autofree char *time = format_clock_time(message->time);
	g_autofree char *line = g_strdup_printf("%s  %s", time, plain);
	gtk_label_set_text(GTK_LABEL(parts->status_line), line);
}


/* The avatar shows on a group's first row and an empty space of its size on the rest */
static GtkWidget *show_avatar(RowParts *parts, CrimsonMessage *message, CrimsonRoom *room, gboolean is_first) {
	gboolean has_avatars = room->kind == ROOM_CHANNEL && !message->is_own;
	gboolean shows_avatar = has_avatars && is_first && message->sender;
	gtk_widget_set_visible(parts->avatar, shows_avatar);
	gtk_widget_set_visible(parts->spacer, has_avatars && !shows_avatar);
	if (!shows_avatar)
		return has_avatars ? parts->spacer : NULL;
	adw_avatar_set_text(ADW_AVATAR(parts->avatar), message->sender);
	const char *url = pick_person_picture(room, message->sender);
	g_autofree char *sized = fill_image_size(url, MESSAGE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(parts->avatar), sized);
	gtk_widget_set_tooltip_text(parts->avatar, message->sender);
	return parts->avatar;
}


static void show_sender(RowParts *parts, CrimsonMessage *message, CrimsonRoom *room, gboolean is_first) {
	gboolean shows_name = is_first && !message->is_own && message->sender && room->kind == ROOM_CHANNEL
		&& message->kind != MESSAGE_ACTION;
	gtk_widget_set_visible(parts->sender, shows_name);
	if (parts->name_color)
		gtk_widget_remove_css_class(parts->sender, parts->name_color);
	g_clear_pointer(&parts->name_color, g_free);
	if (!shows_name)
		return;
	gtk_label_set_text(GTK_LABEL(parts->sender), message->sender);
	parts->name_color = g_strdup_printf("name-color-%d", pick_name_color(message->sender));
	gtk_widget_add_css_class(parts->sender, parts->name_color);
}


static void show_footer(RowParts *parts, CrimsonMessage *message) {
	gtk_widget_set_visible(parts->bot, message->is_bot);
	g_autofree char *moment = format_clock_time(message->time);
	gtk_label_set_text(GTK_LABEL(parts->time), moment);
	gtk_widget_set_visible(parts->state, message->is_own);
	gtk_image_set_from_icon_name(GTK_IMAGE(parts->state), message->is_pending ? "crimson-clock-symbolic"
		: "crimson-sent-symbolic");
	gtk_widget_set_tooltip_text(parts->state, message->is_pending ? TEXT_SENDING : TEXT_SENT);
}


/* A short line keeps its time beside it, as in Telegram, while longer text and pictures put it underneath */
static void show_text(RowParts *parts, CrimsonMessage *message, const char *text) {
	g_autofree char *markup = build_body_markup(message, text);
	gboolean can_reveal = message->is_redacted && *text;
	gtk_widget_set_visible(parts->reveal, can_reveal);
	g_object_set_data_full(G_OBJECT(parts->reveal), "original", build_text_markup(message, text), g_free);
	g_object_set_data_full(G_OBJECT(parts->reveal), "note", g_strdup(markup), g_free);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(parts->reveal), FALSE);
	toggle_deleted_text(GTK_TOGGLE_BUTTON(parts->reveal), GTK_LABEL(parts->body));
	set_class(parts->body, "redacted", message->is_redacted);
	set_class(parts->body, "body", !message->is_redacted);
	gboolean is_short = !strchr(text, '\n') && g_utf8_strlen(text, -1) <= INLINE_TIME_CHARS && !strstr(text, "://");
	gtk_orientable_set_orientation(GTK_ORIENTABLE(parts->text_line), is_short ? GTK_ORIENTATION_HORIZONTAL
		: GTK_ORIENTATION_VERTICAL);
	gtk_box_set_spacing(GTK_BOX(parts->text_line), is_short ? 10 : 3);
	gtk_widget_set_valign(parts->footer, is_short ? GTK_ALIGN_END : GTK_ALIGN_FILL);
	gtk_widget_set_hexpand(parts->footer, is_short);
	clear_children(parts->previews);
	if (!is_short)
		add_previews(parts->previews, message);
	gtk_widget_set_visible(parts->previews, gtk_widget_get_first_child(parts->previews) != NULL);
}


static void show_bubble(RowParts *parts, CrimsonMessage *message, CrimsonRoom *room, gboolean is_first,
	gboolean ends_group) {
	gboolean is_mention = is_marked_mention(message, room);
	set_class(parts->bubble, "own", message->is_own);
	set_class(parts->bubble, "mention", is_mention);
	set_class(parts->bubble, "notice", message->kind == MESSAGE_NOTICE);
	set_class(parts->bubble, "first", is_first);
	/* Only a group's last bubble rounds its lower corner on the avatar's side */
	set_class(parts->bubble, "group-end", ends_group);
	g_object_set_data(G_OBJECT(parts->bubble), "room", room);
	show_sender(parts, message, room, is_first);
	TextQuote text_quote = {0};
	gboolean has_text_quote = message->kind == MESSAGE_TEXT && read_text_quote(message->text, &text_quote);
	clear_children(parts->quote_slot);
	if (message->reply_to || has_text_quote)
		gtk_box_append(GTK_BOX(parts->quote_slot), create_reply_quote(message, room, &parts->context,
			has_text_quote ? &text_quote : NULL));
	gtk_widget_set_visible(parts->quote_slot, message->reply_to || has_text_quote);
	g_autofree char *text = g_strdup(has_text_quote ? text_quote.rest : message->text ? message->text : "");
	if (has_text_quote)
		clear_text_quote(&text_quote);
	show_text(parts, message, text);
	show_footer(parts, message);
	gtk_widget_set_visible(parts->tail, is_first);
	gtk_widget_set_halign(parts->tail, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	set_class(parts->tail, "own", message->is_own);
	set_class(parts->tail, "mention", is_mention);
}


static void show_reactions(RowParts *parts, CrimsonMessage *message, CrimsonRoom *room) {
	clear_children(parts->reactions_slot);
	gboolean has_reactions = message->reaction_order->len > 0;
	gtk_widget_set_visible(parts->reactions_slot, has_reactions);
	if (!has_reactions)
		return;
	GtkWidget *reactions = create_reactions(message, room, &parts->context);
	gtk_widget_set_halign(reactions, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(parts->reactions_slot), reactions);
}


static GtkWidget *show_message_line(RowParts *parts, CrimsonMessage *message, CrimsonRoom *room, gboolean is_first,
	gboolean ends_group) {
	GtkAlign side = message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START;
	gtk_widget_set_halign(parts->line, side);
	gtk_widget_set_halign(parts->tailed, side);
	/* A message arriving while its chat is open rises in with its tail */
	set_class(parts->tailed, "fresh-message", is_message_arriving(message));
	show_bubble(parts, message, room, is_first, ends_group);
	show_reactions(parts, message, room);
	return show_avatar(parts, message, room, is_first);
}


void fill_message_row(GtkWidget *row, CrimsonMessage *message, const RowPlace *place, CrimsonRoom *room,
	const MessageActions *actions) {
	RowParts *parts = g_object_get_data(G_OBJECT(row), PARTS_KEY);
	g_set_object(&parts->context.message, message);
	parts->context.actions = actions;
	CrimsonMessage *previous = place->previous;
	/* An event inside a run is already a line of the pill on the run's first row */
	gboolean is_folded = message->kind == MESSAGE_EVENT && !place->notes;
	set_class(row, "folded-note", is_folded);
	gboolean is_new_day = !previous || !is_same_day(previous->time, message->time);
	show_day(row, parts, !is_folded && is_new_day && message->kind != MESSAGE_STATUS ? message : NULL);
	gtk_widget_set_visible(parts->read_line, !is_folded && place->starts_unread);
	show_notes(parts, is_folded ? NULL : message, is_folded ? NULL : place->notes);
	gboolean is_spoken = !is_folded && message->kind != MESSAGE_EVENT && message->kind != MESSAGE_STATUS;
	gtk_widget_set_visible(parts->line, is_spoken);
	/* The new messages start a group of their own, so the first one shows who wrote it again */
	gboolean is_first = is_spoken && starts_message_group(message, previous, place->starts_unread);
	set_class(row, "group-start", is_first);
	GtkWidget *slot = is_spoken ? show_message_line(parts, message, room, is_first, place->ends_group) : NULL;
	g_object_set_data(G_OBJECT(row), AVATAR_SLOT_KEY, slot);
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
