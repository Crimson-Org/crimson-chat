#include "../lang/en.h"
#include "formatting.h"
#include "message-search.h"
#include "room-look.h"
#include "session.h"

#define SEARCH_WIDTH 400
#define SEARCH_HEIGHT 620
/* Fewer letters than this find too much to be of use */
#define SHORTEST_SEARCH 2
#define FOUND_TEXT_LINES 3
#define SEARCH_KEY "search"

/* One open search dialog, with the room it searches and who hears the pick */
typedef struct {
	CrimsonRoom *room;
	MessagePicked picked;
	gpointer data;
	AdwDialog *dialog;
	GtkWidget *pages, *results;
	GPtrArray *found;
} MessageSearch;


static void free_search(MessageSearch *search) {
	cancel_message_search(search->room->network, search);
	g_object_unref(search->room);
	g_clear_pointer(&search->found, g_ptr_array_unref);
	g_free(search);
}


static GtkWidget *create_found_row(CrimsonMessage *message) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(box, "found-message");
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *sender = gtk_label_new(message->sender ? message->sender : "");
	gtk_label_set_xalign(GTK_LABEL(sender), 0);
	gtk_label_set_ellipsize(GTK_LABEL(sender), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(sender, TRUE);
	gtk_widget_add_css_class(sender, "heading");
	g_autofree char *day = format_day_heading(message->time);
	g_autofree char *clock = format_clock_time(message->time);
	g_autofree char *moment = g_strdup_printf("%s %s", day, clock);
	GtkWidget *time = gtk_label_new(moment);
	gtk_widget_add_css_class(time, "dim-label");
	gtk_box_append(GTK_BOX(top), sender);
	gtk_box_append(GTK_BOX(top), time);
	g_autofree char *plain = strip_irc_formatting(message->text);
	GtkWidget *text = gtk_label_new(plain);
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_lines(GTK_LABEL(text), FOUND_TEXT_LINES);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), top);
	gtk_box_append(GTK_BOX(box), text);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	return row;
}


static void show_found(GPtrArray *messages, gpointer data) {
	MessageSearch *search = data;
	if (!messages) {
		gtk_stack_set_visible_child_name(GTK_STACK(search->pages), "nothing");
		return;
	}
	g_clear_pointer(&search->found, g_ptr_array_unref);
	search->found = g_ptr_array_ref(messages);
	gtk_list_box_remove_all(GTK_LIST_BOX(search->results));
	for (guint index = 0; index < messages->len; index++)
		gtk_list_box_append(GTK_LIST_BOX(search->results), create_found_row(g_ptr_array_index(messages, index)));
	gtk_stack_set_visible_child_name(GTK_STACK(search->pages), messages->len ? "results" : "nothing");
}


static void change_search(GtkSearchEntry *entry, MessageSearch *search) {
	/* Spaces around the words find nothing more, and a line's end loses them on the way back */
	g_autofree char *words = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	cancel_message_search(search->room->network, search);
	if (g_utf8_strlen(words, -1) < SHORTEST_SEARCH) {
		gtk_stack_set_visible_child_name(GTK_STACK(search->pages), "hint");
		return;
	}
	gtk_stack_set_visible_child_name(GTK_STACK(search->pages), "waiting");
	search_room_messages(search->room, words, show_found, search);
}


static void pick_found(GtkListBox *list, GtkListBoxRow *row, MessageSearch *search) {
	int index = gtk_list_box_row_get_index(row);
	if (!search->found || index < 0 || (guint)index >= search->found->len)
		return;
	g_autoptr(CrimsonMessage) message = g_object_ref(g_ptr_array_index(search->found, index));
	search->picked(message, search->data);
	adw_dialog_close(search->dialog);
}


static GtkWidget *create_status(const char *icon, const char *title) {
	GtkWidget *status = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
	gtk_widget_add_css_class(status, "compact");
	return status;
}


void show_message_search(Window *window, CrimsonRoom *room, MessagePicked picked, gpointer data) {
	MessageSearch *search = g_new0(MessageSearch, 1);
	search->room = g_object_ref(room);
	search->picked = picked;
	search->data = data;
	search->dialog = adw_dialog_new();
	adw_dialog_set_title(search->dialog, TEXT_SEARCH_MESSAGES);
	adw_dialog_set_content_width(search->dialog, SEARCH_WIDTH);
	adw_dialog_set_content_height(search->dialog, SEARCH_HEIGHT);
	g_object_set_data_full(G_OBJECT(search->dialog), SEARCH_KEY, search, (GDestroyNotify)free_search);
	GtkWidget *entry = gtk_search_entry_new();
	g_autofree char *title = describe_room_title(room);
	g_autofree char *placeholder = g_strdup_printf(TEXT_SEARCH_PLACEHOLDER, title);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry), placeholder);
	gtk_widget_set_hexpand(entry, TRUE);
	g_signal_connect(entry, "search-changed", G_CALLBACK(change_search), search);
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), entry);
	search->results = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(search->results), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(search->results, "boxed-list");
	gtk_widget_add_css_class(search->results, "found-messages");
	gtk_widget_set_valign(search->results, GTK_ALIGN_START);
	g_signal_connect(search->results, "row-activated", G_CALLBACK(pick_found), search);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), search->results);
	GtkWidget *waiting = create_spinner();
	gtk_widget_set_halign(waiting, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(waiting, GTK_ALIGN_CENTER);
	search->pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(search->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(search->pages), create_status("system-search-symbolic", TEXT_SEARCH_HINT), "hint");
	gtk_stack_add_named(GTK_STACK(search->pages), waiting, "waiting");
	gtk_stack_add_named(GTK_STACK(search->pages), create_status("edit-find-symbolic", TEXT_SEARCH_NOTHING),
		"nothing");
	gtk_stack_add_named(GTK_STACK(search->pages), scroller, "results");
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), search->pages);
	adw_dialog_set_child(search->dialog, toolbar);
	adw_dialog_set_focus(search->dialog, entry);
	adw_dialog_present(search->dialog, GTK_WIDGET(window->window));
}
