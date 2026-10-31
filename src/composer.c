#include <string.h>

#include <adwaita.h>

#include "lang/en.h"
#include "common/identity.h"
#include "composer.h"
#include "formatting.h"
#include "session.h"
#include "uploads.h"
#include "window.h"

#define TYPING_INTERVAL_MICROSECONDS (3 * G_USEC_PER_SEC)
#define SEND_SPINNER_SIZE 20
#define SEND_ROOM_MS 220
/* The send button's width and the gap before it, which the field gives up while the button shows */
#define SEND_ROOM 52
/* How far the field has moved aside when the send button starts to grow in */
#define SEND_GROW_POINT 0.2
#define PAUSE_SECONDS 6
#define FIELD_MAXIMUM_HEIGHT 180
#define DRAFTS_KEY "draft"
/* The cache folder a pasted picture is saved in while it uploads */
#define PASTED_FOLDER "pasted"
/* The most people or commands suggested at once */
#define SUGGESTIONS_SHOWN 6

struct Composer {
	GtkWidget *box, *reply_bar, *reply_name, *reply_text, *field, *placeholder, *send_button, *share_button;
	GtkTextBuffer *buffer;
	CrimsonRoom *room;
	CrimsonMessage *reply_to;
	gint64 last_typing;
	gboolean is_typing;
	/* Whether the room's server is not connected, which turns the send button into a spinner and holds the text */
	gboolean is_waiting, is_spinner_shown;
	/* Why the room takes no messages from the user, such as needing a login, which greys out the send button for
	 * anything but commands, or NULL */
	char *refusal;
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
	/* The list over the field that suggests people after @ and commands after /, with what each row puts in, and
	 * where the typed part it replaces starts */
	GtkWidget *suggestions, *suggestion_rows;
	GPtrArray *suggested;
	int suggestion_start;
	gboolean suggests_commands;
};

/* The commands the composer suggests while one is typed, with what each does */
static const struct {
	const char *name, *hint;
} COMMANDS[] = {
	{"me", TEXT_COMMAND_ME}, {"join", TEXT_COMMAND_JOIN}, {"msg", TEXT_COMMAND_MSG},
	{"invite", TEXT_COMMAND_INVITE}, {"topic", TEXT_COMMAND_TOPIC}, {"whois", TEXT_COMMAND_WHOIS},
	{"nick", TEXT_COMMAND_NICK}, {"away", TEXT_COMMAND_AWAY}, {"back", TEXT_COMMAND_BACK},
	{"list", TEXT_COMMAND_LIST}, {"kick", TEXT_COMMAND_KICK}, {"leave", TEXT_COMMAND_LEAVE},
	{"notice", TEXT_COMMAND_NOTICE}, {"quote", TEXT_COMMAND_QUOTE},
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


/* Commands still go to a room that refuses messages, so the user can log in or join from it */
static gboolean can_send_text(Composer *composer, const char *text) {
	return !composer->is_waiting && (!composer->refusal || *text == '/');
}


static void send_field(Composer *composer) {
	g_autofree char *text = get_field_text(composer);
	if (!composer->room || !can_send_text(composer, text) || strspn(text, " \n\t") == strlen(text))
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


static void hide_suggestions(Composer *composer) {
	g_clear_pointer(&composer->suggested, g_ptr_array_unref);
	if (composer->suggestions)
		gtk_popover_popdown(GTK_POPOVER(composer->suggestions));
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


static GtkWidget *create_suggestion_row(const char *title, const char *hint) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(box, "suggestion");
	GtkWidget *name = gtk_label_new(title);
	gtk_widget_add_css_class(name, "heading");
	gtk_box_append(GTK_BOX(box), name);
	if (hint) {
		GtkWidget *note = gtk_label_new(hint);
		gtk_label_set_ellipsize(GTK_LABEL(note), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class(note, "dim-label");
		gtk_box_append(GTK_BOX(box), note);
	}
	return box;
}


/* Puts the picked suggestion in place of what was typed for it, a nick with a colon when it starts the message */
static void take_suggestion(Composer *composer, guint index) {
	if (!composer->suggested || index >= composer->suggested->len)
		return;
	GtkTextIter cursor, start;
	gtk_text_buffer_get_iter_at_mark(composer->buffer, &cursor, gtk_text_buffer_get_insert(composer->buffer));
	gtk_text_buffer_get_iter_at_offset(composer->buffer, &start, composer->suggestion_start);
	const char *picked = g_ptr_array_index(composer->suggested, index);
	g_autofree char *inserted = composer->suggests_commands ? g_strconcat("/", picked, " ", NULL)
		: g_strconcat(picked, composer->suggestion_start == 0 ? ": " : " ", NULL);
	hide_suggestions(composer);
	gtk_text_buffer_delete(composer->buffer, &start, &cursor);
	gtk_text_buffer_insert(composer->buffer, &start, inserted, -1);
	hide_suggestions(composer);
}


static void pick_suggestion(GtkListBox *list, GtkListBoxRow *row, Composer *composer) {
	take_suggestion(composer, (guint)gtk_list_box_row_get_index(row));
	gtk_widget_grab_focus(composer->field);
}


static void create_suggestions(Composer *composer) {
	composer->suggestion_rows = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(composer->suggestion_rows), GTK_SELECTION_SINGLE);
	gtk_widget_set_can_focus(composer->suggestion_rows, FALSE);
	g_signal_connect(composer->suggestion_rows, "row-activated", G_CALLBACK(pick_suggestion), composer);
	composer->suggestions = gtk_popover_new();
	gtk_widget_add_css_class(composer->suggestions, "suggestions");
	gtk_popover_set_child(GTK_POPOVER(composer->suggestions), composer->suggestion_rows);
	gtk_popover_set_autohide(GTK_POPOVER(composer->suggestions), FALSE);
	gtk_popover_set_has_arrow(GTK_POPOVER(composer->suggestions), FALSE);
	gtk_popover_set_position(GTK_POPOVER(composer->suggestions), GTK_POS_TOP);
	gtk_widget_set_can_focus(composer->suggestions, FALSE);
	gtk_widget_set_halign(composer->suggestions, GTK_ALIGN_START);
	gtk_widget_set_parent(composer->suggestions, composer->field_area);
}


/* The word before the cursor picks what is suggested, people after @ and commands while the first word starts
 * with / */
static void suggest(Composer *composer) {
	GtkTextIter cursor, start;
	gtk_text_buffer_get_iter_at_mark(composer->buffer, &cursor, gtk_text_buffer_get_insert(composer->buffer));
	start = cursor;
	while (!gtk_text_iter_starts_line(&start)) {
		gtk_text_iter_backward_char(&start);
		if (g_unichar_isspace(gtk_text_iter_get_char(&start))) {
			gtk_text_iter_forward_char(&start);
			break;
		}
	}
	g_autofree char *word = gtk_text_iter_get_text(&start, &cursor);
	gboolean is_first_word = gtk_text_iter_get_offset(&start) == 0;
	g_autoptr(GPtrArray) shown = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) hints = g_ptr_array_new();
	composer->suggests_commands = is_first_word && *word == '/' && composer->room->kind != ROOM_SERVER;
	if (composer->suggests_commands) {
		for (guint index = 0; index < G_N_ELEMENTS(COMMANDS) && shown->len < SUGGESTIONS_SHOWN; index++) {
			if (g_str_has_prefix(COMMANDS[index].name, word + 1)) {
				g_ptr_array_add(shown, g_strdup(COMMANDS[index].name));
				g_ptr_array_add(hints, (gpointer)COMMANDS[index].hint);
			}
		}
	} else if (*word == '@') {
		collect_matches(composer, word + 1);
		for (guint index = 0; index < composer->matches->len && shown->len < SUGGESTIONS_SHOWN; index++) {
			g_ptr_array_add(shown, g_strdup(g_ptr_array_index(composer->matches, index)));
			g_ptr_array_add(hints, NULL);
		}
		clear_matches(composer);
	}
	if (!shown->len) {
		hide_suggestions(composer);
		return;
	}
	if (!composer->suggestions)
		create_suggestions(composer);
	gtk_list_box_remove_all(GTK_LIST_BOX(composer->suggestion_rows));
	for (guint index = 0; index < shown->len; index++) {
		const char *name = g_ptr_array_index(shown, index);
		g_autofree char *title = composer->suggests_commands ? g_strconcat("/", name, NULL) : g_strdup(name);
		gtk_list_box_append(GTK_LIST_BOX(composer->suggestion_rows), create_suggestion_row(title,
			g_ptr_array_index(hints, index)));
	}
	gtk_list_box_select_row(GTK_LIST_BOX(composer->suggestion_rows),
		gtk_list_box_get_row_at_index(GTK_LIST_BOX(composer->suggestion_rows), 0));
	g_clear_pointer(&composer->suggested, g_ptr_array_unref);
	composer->suggested = g_steal_pointer(&shown);
	composer->suggestion_start = gtk_text_iter_get_offset(&start);
	gtk_popover_popup(GTK_POPOVER(composer->suggestions));
}


/* While suggestions show, the arrow keys move through them and Tab or Enter takes the chosen one */
static gboolean steer_suggestions(Composer *composer, guint key) {
	if (!composer->suggested)
		return FALSE;
	GtkListBox *rows = GTK_LIST_BOX(composer->suggestion_rows);
	GtkListBoxRow *selected = gtk_list_box_get_selected_row(rows);
	int index = selected ? gtk_list_box_row_get_index(selected) : 0;
	int count = (int)composer->suggested->len;
	if (key == GDK_KEY_Down || key == GDK_KEY_Up) {
		index = (index + (key == GDK_KEY_Down ? 1 : count - 1)) % count;
		gtk_list_box_select_row(rows, gtk_list_box_get_row_at_index(rows, index));
		return TRUE;
	}
	if (key == GDK_KEY_Tab || key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
		take_suggestion(composer, (guint)index);
		return TRUE;
	}
	if (key == GDK_KEY_Escape) {
		hide_suggestions(composer);
		return TRUE;
	}
	return FALSE;
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
	if (steer_suggestions(composer, key))
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
 * little bounce soon after the field starts moving */
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
	gtk_widget_set_sensitive(composer->send_button, *text && can_send_text(composer, text));
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
	if (composer->room)
		suggest(composer);
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


/* The chooser is made on first use, since a new one lays out every emoji in the background and holds up frames */
static void create_emoji_chooser(GtkMenuButton *button, gpointer data) {
	if (gtk_menu_button_get_popover(button))
		return;
	GtkWidget *chooser = gtk_emoji_chooser_new();
	g_signal_connect(chooser, "emoji-picked", G_CALLBACK(insert_emoji), data);
	gtk_menu_button_set_popover(button, chooser);
}


static void finish_choosing_file(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(CrimsonRoom) room = data;
	g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (file)
		share_file(room, file);
}


static gboolean can_share(Composer *composer) {
	return composer->room && gtk_widget_get_visible(composer->share_button);
}


static void share_file_list(Composer *composer, GSList *files) {
	for (GSList *item = files; item; item = item->next)
		share_file(composer->room, item->data);
}


static gboolean drop_files(GtkDropTarget *target, const GValue *value, double x, double y, Composer *composer) {
	if (!can_share(composer) || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
		return FALSE;
	share_file_list(composer, g_value_get_boxed(value));
	return TRUE;
}


static void finish_pasting_files(GObject *source, GAsyncResult *result, gpointer data) {
	Composer *composer = data;
	const GValue *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), result, NULL);
	if (value && can_share(composer))
		share_file_list(composer, g_value_get_boxed(value));
}


/* A pasted picture, such as a screenshot, is shared from a file in the cache, which replaces the last one */
static void finish_pasting_picture(GObject *source, GAsyncResult *result, gpointer data) {
	Composer *composer = data;
	g_autoptr(GdkTexture) texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result, NULL);
	if (!texture || !can_share(composer))
		return;
	g_autofree char *folder = g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug,
		PASTED_FOLDER, NULL);
	g_autoptr(GFile) old = g_file_new_for_path(folder);
	g_autoptr(GFileEnumerator) entries = g_file_enumerate_children(old, G_FILE_ATTRIBUTE_STANDARD_NAME,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	for (GFileInfo *info = entries ? g_file_enumerator_next_file(entries, NULL, NULL) : NULL; info;
		info = g_file_enumerator_next_file(entries, NULL, NULL)) {
		g_autoptr(GFile) child = g_file_get_child(old, g_file_info_get_name(info));
		g_file_delete(child, NULL, NULL);
		g_object_unref(info);
	}
	g_mkdir_with_parents(folder, 0700);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree char *stamp = g_date_time_format(now, TEXT_PASTED_PICTURE_NAME);
	g_autofree char *path = g_build_filename(folder, stamp, NULL);
	if (!gdk_texture_save_to_png(texture, path))
		return;
	g_autoptr(GFile) file = g_file_new_for_path(path);
	share_file(composer->room, file);
}


/* Pasting files or a picture shares them where the server takes files, and anything else pastes as text */
static void paste_into_field(GtkTextView *field, Composer *composer) {
	GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(field));
	GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
	if (!can_share(composer))
		return;
	if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST)) {
		g_signal_stop_emission_by_name(field, "paste-clipboard");
		gdk_clipboard_read_value_async(clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, finish_pasting_files,
			composer);
	} else if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
		g_signal_stop_emission_by_name(field, "paste-clipboard");
		gdk_clipboard_read_texture_async(clipboard, NULL, finish_pasting_picture, composer);
	}
}


void accept_dropped_files(Composer *composer, GtkWidget *area) {
	GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(target, "drop", G_CALLBACK(drop_files), composer);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(target));
}


static void choose_file(GtkButton *button, Composer *composer) {
	if (!composer->room)
		return;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_CHOOSE_FILE_TITLE);
	gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(composer->box)), NULL, finish_choosing_file,
		g_object_ref(composer->room));
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
	g_signal_connect(composer->field, "paste-clipboard", G_CALLBACK(paste_into_field), composer);
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
	/* The emoji button sits inside the field at its end, level with the first line when the text wraps */
	GtkWidget *emoji = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(emoji), "face-smile-symbolic");
	gtk_widget_set_tooltip_text(emoji, TEXT_EMOJI);
	gtk_widget_add_css_class(emoji, "flat");
	gtk_widget_add_css_class(emoji, "circular");
	gtk_widget_add_css_class(emoji, "emoji-button");
	gtk_widget_set_valign(emoji, GTK_ALIGN_START);
	gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(emoji), create_emoji_chooser, composer, NULL);
	GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(inside), scroller);
	gtk_box_append(GTK_BOX(inside), emoji);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_widget_add_css_class(overlay, "composer-pill");
	gtk_overlay_set_child(GTK_OVERLAY(overlay), inside);
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
	composer->share_button = gtk_button_new_from_icon_name("crimson-attach-symbolic");
	gtk_widget_set_tooltip_text(composer->share_button, TEXT_SHARE_FILE);
	gtk_widget_add_css_class(composer->share_button, "flat");
	gtk_widget_add_css_class(composer->share_button, "circular");
	gtk_widget_add_css_class(composer->share_button, "share-button");
	gtk_widget_set_valign(composer->share_button, GTK_ALIGN_END);
	gtk_widget_set_visible(composer->share_button, FALSE);
	g_signal_connect(composer->share_button, "clicked", G_CALLBACK(choose_file), composer);
	gtk_box_append(GTK_BOX(row), composer->share_button);
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
	hide_suggestions(composer);
	g_set_object(&composer->room, room);
	const char *draft = room ? g_object_get_data(G_OBJECT(room), DRAFTS_KEY) : NULL;
	g_signal_handler_block(composer->buffer, composer->change_handler);
	gtk_text_buffer_set_text(composer->buffer, draft ? draft : "", -1);
	g_signal_handler_unblock(composer->buffer, composer->change_handler);
	gtk_widget_set_visible(composer->placeholder, !draft || !*draft);
	refresh_send_button(composer);
	gtk_label_set_text(GTK_LABEL(composer->placeholder), room && room->kind == ROOM_SERVER ? TEXT_SEND_COMMAND
		: TEXT_WRITE_MESSAGE);
	refresh_composer_sharing(composer);
}


void refresh_composer_sharing(Composer *composer) {
	g_autofree char *address = composer->room && composer->room->kind != ROOM_SERVER
		? find_file_upload_url(composer->room->network) : NULL;
	gtk_widget_set_visible(composer->share_button, address != NULL);
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
	gtk_widget_set_tooltip_text(composer->send_button, message ? message : composer->refusal ? composer->refusal
		: TEXT_SEND);
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


void set_composer_refusal(Composer *composer, const char *refusal) {
	if (g_strcmp0(composer->refusal, refusal) == 0)
		return;
	g_free(composer->refusal);
	composer->refusal = g_strdup(refusal);
	if (!composer->is_waiting)
		gtk_widget_set_tooltip_text(composer->send_button, refusal ? refusal : TEXT_SEND);
	refresh_send_button(composer);
}
