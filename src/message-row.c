#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "message-row.h"
#include "room-look.h"
#include "session.h"
#include "settings.h"

#define AVATAR_SIZE 34
#define GROUP_GAP_MICROSECONDS (5 * 60 * G_USEC_PER_SEC)
#define TEXT_WIDTH_CHARS 62
#define PREVIEW_WIDTH 320
#define PREVIEW_HEIGHT 220
#define REPLY_SNIPPET_CHARS 80
#define PERSON_KEY "person"
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


static gboolean starts_group(CrimsonMessage *message, CrimsonMessage *previous) {
	if (!previous || previous->kind == MESSAGE_EVENT || previous->kind == MESSAGE_STATUS)
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


static void add_note(GtkWidget *row, CrimsonMessage *message) {
	g_autofree char *plain = strip_irc_formatting(message->text);
	if (message->kind == MESSAGE_EVENT) {
		gtk_box_append(GTK_BOX(row), create_pill(plain, "event-pill"));
		return;
	}
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
	for (int index = 0; QUICK_REACTIONS[index]; index++) {
		GtkWidget *button = gtk_button_new_with_label(QUICK_REACTIONS[index]);
		gtk_widget_add_css_class(button, "flat");
		gtk_widget_add_css_class(button, "circular");
		gtk_widget_add_css_class(button, "quick-reaction");
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
	gboolean can_react = message->id && room && can_network(room->network, "message-tags");
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


static GtkWidget *create_reply_quote(CrimsonMessage *message, CrimsonRoom *room, RowContext *context) {
	CrimsonMessage *original = find_room_message(room, message->reply_to);
	GtkWidget *quote = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(quote, "reply-quote");
	GtkWidget *name = gtk_label_new(original && original->sender ? original->sender : TEXT_EARLIER_MESSAGE);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_add_css_class(name, "reply-name");
	if (original && original->sender) {
		g_autofree char *color = g_strdup_printf("name-color-%d", pick_name_color(original->sender));
		gtk_widget_add_css_class(quote, color);
	}
	g_autofree char *plain = original ? strip_irc_formatting(original->text) : g_strdup(TEXT_NOT_LOADED);
	GtkWidget *snippet = gtk_label_new(plain);
	gtk_label_set_xalign(GTK_LABEL(snippet), 0);
	gtk_label_set_ellipsize(GTK_LABEL(snippet), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(snippet), REPLY_SNIPPET_CHARS);
	gtk_label_set_single_line_mode(GTK_LABEL(snippet), TRUE);
	gtk_widget_add_css_class(snippet, "reply-snippet");
	gtk_box_append(GTK_BOX(quote), name);
	gtk_box_append(GTK_BOX(quote), snippet);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(show_replied_message), context);
	gtk_widget_add_controller(quote, GTK_EVENT_CONTROLLER(click));
	gtk_widget_set_cursor_from_name(quote, "pointer");
	return quote;
}


static char *build_body_markup(CrimsonMessage *message) {
	if (message->is_redacted)
		return g_markup_printf_escaped("<i>%s</i>", TEXT_MESSAGE_DELETED);
	g_autofree char *markup = format_irc_markup(message->text);
	if (message->kind != MESSAGE_ACTION)
		return g_steal_pointer(&markup);
	g_autofree char *sender = g_markup_escape_text(message->sender ? message->sender : "", -1);
	return g_strdup_printf("<i>%s %s</i>", sender, markup);
}


static void free_link(gpointer url, GClosure *closure) {
	g_free(url);
}


static void open_preview(GtkGestureClick *gesture, int presses, double x, double y, gpointer url) {
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(widget)), NULL, NULL, NULL);
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


static GtkWidget *create_bubble(CrimsonMessage *message, CrimsonRoom *room, gboolean is_first, RowContext *context) {
	GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_add_css_class(bubble, "bubble");
	if (message->is_own)
		gtk_widget_add_css_class(bubble, "own");
	if (message->is_highlight && room->kind == ROOM_CHANNEL)
		gtk_widget_add_css_class(bubble, "mention");
	if (message->kind == MESSAGE_NOTICE)
		gtk_widget_add_css_class(bubble, "notice");
	if (is_first)
		gtk_widget_add_css_class(bubble, "first");
	if (message->is_fresh) {
		gtk_widget_add_css_class(bubble, "fresh");
		message->is_fresh = FALSE;
	}
	g_object_set_data(G_OBJECT(bubble), "room", room);
	context->bubble = bubble;
	gboolean shows_name = is_first && !message->is_own && message->sender && room->kind == ROOM_CHANNEL
		&& message->kind != MESSAGE_ACTION;
	if (shows_name)
		gtk_box_append(GTK_BOX(bubble), create_sender_label(message, context));
	if (message->reply_to)
		gtk_box_append(GTK_BOX(bubble), create_reply_quote(message, room, context));
	g_autofree char *markup = build_body_markup(message);
	GtkWidget *body = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(body), markup);
	gtk_label_set_wrap(GTK_LABEL(body), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(body), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(body), TEXT_WIDTH_CHARS);
	gtk_label_set_selectable(GTK_LABEL(body), TRUE);
	gtk_widget_set_focusable(body, FALSE);
	gtk_widget_add_css_class(body, message->is_redacted ? "redacted" : "body");
	/* A short line keeps its time beside it, as in Telegram, while longer text and pictures put it underneath */
	const char *text = message->text ? message->text : "";
	gboolean is_short = !strchr(text, '\n') && g_utf8_strlen(text, -1) <= INLINE_TIME_CHARS && !strstr(text, "://");
	if (is_short) {
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_box_append(GTK_BOX(line), body);
		GtkWidget *footer = create_footer(message);
		gtk_widget_set_valign(footer, GTK_ALIGN_END);
		gtk_widget_set_hexpand(footer, TRUE);
		gtk_box_append(GTK_BOX(line), footer);
		gtk_box_append(GTK_BOX(bubble), line);
	} else {
		gtk_box_append(GTK_BOX(bubble), body);
		add_previews(bubble, message);
		gtk_box_append(GTK_BOX(bubble), create_footer(message));
	}
	add_menu_gestures(bubble, context);
	return bubble;
}


static GtkWidget *create_message_line(CrimsonMessage *message, CrimsonRoom *room, gboolean is_first,
	RowContext *context) {
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(line, "message-line");
	gtk_widget_set_halign(line, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
	gboolean has_avatars = room->kind == ROOM_CHANNEL && !message->is_own;
	if (has_avatars && is_first && message->sender) {
		GtkWidget *avatar = adw_avatar_new(AVATAR_SIZE, message->sender, TRUE);
		gtk_widget_set_valign(avatar, GTK_ALIGN_START);
		const char *url = find_metadata(room, message->sender, "avatar");
		g_autofree char *sized = fill_image_size(url, AVATAR_SIZE * 2);
		show_avatar_image(ADW_AVATAR(avatar), sized);
		make_person_target(avatar, context);
		gtk_box_append(GTK_BOX(line), avatar);
	} else if (has_avatars) {
		GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_size_request(spacer, AVATAR_SIZE, -1);
		gtk_box_append(GTK_BOX(line), spacer);
	}
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(column), create_bubble(message, room, is_first, context));
	if (message->reaction_order->len) {
		GtkWidget *reactions = create_reactions(message, room, context);
		gtk_widget_set_halign(reactions, message->is_own ? GTK_ALIGN_END : GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(column), reactions);
	}
	gtk_box_append(GTK_BOX(line), column);
	return line;
}


void fill_message_row(GtkWidget *row, CrimsonMessage *message, CrimsonMessage *previous, CrimsonRoom *room,
	const MessageActions *actions) {
	clear_children(row);
	gboolean is_new_day = !previous || !is_same_day(previous->time, message->time);
	if (is_new_day && message->kind != MESSAGE_STATUS) {
		g_autofree char *day = format_day_heading(message->time);
		gtk_box_append(GTK_BOX(row), create_pill(day, "day-pill"));
	}
	if (message->kind == MESSAGE_EVENT || message->kind == MESSAGE_STATUS) {
		add_note(row, message);
		return;
	}
	RowContext *context = g_new0(RowContext, 1);
	context->message = g_object_ref(message);
	context->actions = actions;
	g_object_set_data_full(G_OBJECT(row), "context", context, free_context);
	gboolean is_first = starts_group(message, previous);
	if (is_first)
		gtk_widget_add_css_class(row, "group-start");
	else
		gtk_widget_remove_css_class(row, "group-start");
	gtk_box_append(GTK_BOX(row), create_message_line(message, room, is_first, context));
}
