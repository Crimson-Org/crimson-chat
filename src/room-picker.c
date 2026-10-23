#include <string.h>

#include "../lang/en.h"
#include "room-picker.h"
#include "wizards.h"

#define PICKER_WIDTH 460
#define PICKER_HEIGHT 680
/* The busiest rooms shown, since a big network lists tens of thousands */
#define ROOMS_SHOWN 3000
#define LOADING_MARGIN 32
#define NAME_KEY "room-name"
#define HANDLER_KEY "toggled-handler"

typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *stack, *search, *typed_button, *join_button, *loading_label;
	/* The seconds left before a server that waits after connecting lists its rooms */
	guint wait_left, wait_source;
	GtkStringList *names;
	GtkStringFilter *filter;
	/* Listed names to their rooms, and the names the user ticked */
	GHashTable *rooms, *chosen;
} Picker;


static void free_picker(Picker *picker) {
	g_clear_handle_id(&picker->wait_source, g_source_remove);
	if (picker->network)
		cancel_room_listing(picker->network);
	g_hash_table_unref(picker->rooms);
	g_hash_table_unref(picker->chosen);
	g_object_unref(picker->names);
	g_free(picker);
}


static void free_room_copy(ListedRoom *room) {
	g_free(room->name);
	g_free(room->topic);
	g_free(room);
}


static gboolean is_joined(Picker *picker, const char *name) {
	CrimsonRoom *room = find_room(picker->network, name);
	return room && room->is_joined;
}


static void refresh_join_button(Picker *picker) {
	guint count = g_hash_table_size(picker->chosen);
	g_autofree char *label = count == 1 ? g_strdup(TEXT_JOIN_ONE_ROOM) : g_strdup_printf(TEXT_JOIN_ROOMS, count);
	gtk_button_set_label(GTK_BUTTON(picker->join_button), count ? label : TEXT_JOIN);
	gtk_widget_set_sensitive(picker->join_button, count > 0);
}


static void join_rooms(GPtrArray *names, Picker *picker) {
	for (guint index = 0; index < names->len; index++)
		join_channel(picker->network, g_ptr_array_index(names, index), NULL);
	choose_network(picker->window, picker->network);
	adw_dialog_close(picker->dialog);
}


static void join_chosen(GtkButton *button, Picker *picker) {
	g_autoptr(GPtrArray) names = g_hash_table_get_keys_as_ptr_array(picker->chosen);
	join_rooms(names, picker);
}


/* A typed name that the server did not list still joins at once */
static void join_typed(GtkWidget *widget, Picker *picker) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(picker->search));
	if (!text || !*text)
		return;
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_ptr_array_add(names, (gpointer)text);
	join_rooms(names, picker);
}


static gboolean is_listed(Picker *picker, const char *text) {
	GHashTableIter iterator;
	gpointer name;
	g_hash_table_iter_init(&iterator, picker->rooms);
	while (g_hash_table_iter_next(&iterator, &name, NULL)) {
		if (g_ascii_strcasecmp(name, text) == 0 || (*text != '#' && g_ascii_strcasecmp((char *)name + 1, text) == 0))
			return TRUE;
	}
	return FALSE;
}


static void change_search(GtkSearchEntry *entry, Picker *picker) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
	gtk_string_filter_set_search(picker->filter, text);
	gboolean is_new = text && *text && !is_listed(picker, text);
	gtk_widget_set_visible(picker->typed_button, is_new);
	if (!is_new)
		return;
	g_autofree char *label = g_strdup_printf(TEXT_JOIN_TYPED_ROOM, *text == '#' ? "" : "#", text);
	gtk_button_set_label(GTK_BUTTON(picker->typed_button), label);
}


static void toggle_room(GtkCheckButton *check, Picker *picker) {
	const char *name = g_object_get_data(G_OBJECT(check), NAME_KEY);
	if (!name)
		return;
	if (gtk_check_button_get_active(check))
		g_hash_table_add(picker->chosen, g_strdup(name));
	else
		g_hash_table_remove(picker->chosen, name);
	refresh_join_button(picker);
}


static GtkWidget *create_row_label(const char *css_class) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
	gtk_widget_add_css_class(label, css_class);
	return label;
}


/* The whole row is the tick box, so a click anywhere on a room picks it */
static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *item, Picker *picker) {
	GtkWidget *check = gtk_check_button_new();
	gtk_widget_add_css_class(check, "picker-row");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(texts, TRUE);
	GtkWidget *name = create_row_label("picker-name");
	GtkWidget *topic = create_row_label("picker-topic");
	gtk_box_append(GTK_BOX(texts), name);
	gtk_box_append(GTK_BOX(texts), topic);
	GtkWidget *users = create_row_label("picker-users");
	gtk_box_append(GTK_BOX(box), texts);
	gtk_box_append(GTK_BOX(box), users);
	gtk_check_button_set_child(GTK_CHECK_BUTTON(check), box);
	g_object_set_data(G_OBJECT(check), "name", name);
	g_object_set_data(G_OBJECT(check), "topic", topic);
	g_object_set_data(G_OBJECT(check), "users", users);
	gulong handler = g_signal_connect(check, "toggled", G_CALLBACK(toggle_room), picker);
	g_object_set_data(G_OBJECT(check), HANDLER_KEY, GSIZE_TO_POINTER(handler));
	gtk_list_item_set_child(item, check);
	gtk_list_item_set_activatable(item, FALSE);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, Picker *picker) {
	GtkWidget *check = gtk_list_item_get_child(item);
	const char *name = gtk_string_object_get_string(gtk_list_item_get_item(item));
	ListedRoom *room = g_hash_table_lookup(picker->rooms, name);
	gboolean joined = is_joined(picker, name);
	gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(check), HANDLER_KEY));
	g_signal_handler_block(check, handler);
	g_object_set_data(G_OBJECT(check), NAME_KEY, (gpointer)name);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), joined || g_hash_table_contains(picker->chosen, name));
	g_signal_handler_unblock(check, handler);
	gtk_widget_set_sensitive(check, !joined);
	gtk_label_set_text(g_object_get_data(G_OBJECT(check), "name"), name);
	GtkWidget *topic = g_object_get_data(G_OBJECT(check), "topic");
	gtk_label_set_text(GTK_LABEL(topic), room && room->topic ? room->topic : "");
	gtk_widget_set_visible(topic, room && room->topic && *room->topic);
	/* The topic is cut to one line in the list, so pointing at the room shows all of it */
	gtk_widget_set_tooltip_text(check, room && room->topic && *room->topic ? room->topic : NULL);
	g_autofree char *users = joined ? g_strdup(TEXT_ALREADY_JOINED) : g_strdup_printf("%u", room ? room->users : 0);
	gtk_label_set_text(g_object_get_data(G_OBJECT(check), "users"), users);
}


static void show_rooms(GPtrArray *rooms, gpointer data) {
	Picker *picker = data;
	if (!rooms) {
		picker->network = NULL;
		adw_dialog_close(picker->dialog);
		return;
	}
	guint shown = MIN(rooms->len, ROOMS_SHOWN);
	for (guint index = 0; index < shown; index++) {
		ListedRoom *room = g_ptr_array_index(rooms, index);
		ListedRoom *copy = g_new0(ListedRoom, 1);
		copy->name = g_strdup(room->name);
		copy->topic = g_strdup(room->topic);
		copy->users = room->users;
		g_hash_table_replace(picker->rooms, copy->name, copy);
		gtk_string_list_append(picker->names, room->name);
	}
	g_clear_handle_id(&picker->wait_source, g_source_remove);
	gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "rooms");
	gtk_widget_grab_focus(picker->search);
}


static GtkWidget *create_loading_page(Picker *picker) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), adw_spinner_new());
	picker->loading_label = gtk_label_new(TEXT_FINDING_ROOMS);
	gtk_label_set_wrap(GTK_LABEL(picker->loading_label), TRUE);
	gtk_label_set_justify(GTK_LABEL(picker->loading_label), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(picker->loading_label, "dim-label");
	gtk_widget_set_margin_start(picker->loading_label, LOADING_MARGIN);
	gtk_widget_set_margin_end(picker->loading_label, LOADING_MARGIN);
	gtk_box_append(GTK_BOX(box), picker->loading_label);
	return box;
}


static gboolean count_down(gpointer data) {
	Picker *picker = data;
	if (picker->wait_left > 1)
		picker->wait_left--;
	g_autofree char *text = g_strdup_printf(TEXT_ROOMS_WAIT, picker->wait_left);
	gtk_label_set_text(GTK_LABEL(picker->loading_label), text);
	return G_SOURCE_CONTINUE;
}


/* The server lists its rooms only after a while, so the picker counts down to when it asks again */
static void wait_for_rooms(guint seconds, gpointer data) {
	Picker *picker = data;
	picker->wait_left = seconds + 1;
	count_down(picker);
	if (!picker->wait_source)
		picker->wait_source = g_timeout_add_seconds(1, count_down, picker);
}


static GtkWidget *create_rooms_page(Picker *picker) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "picker");
	picker->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(picker->search), TEXT_SEARCH_ROOMS);
	g_signal_connect(picker->search, "search-changed", G_CALLBACK(change_search), picker);
	g_signal_connect(picker->search, "activate", G_CALLBACK(join_typed), picker);
	gtk_box_append(GTK_BOX(box), picker->search);
	picker->typed_button = gtk_button_new();
	gtk_widget_add_css_class(picker->typed_button, "flat");
	gtk_widget_set_visible(picker->typed_button, FALSE);
	g_signal_connect(picker->typed_button, "clicked", G_CALLBACK(join_typed), picker);
	gtk_box_append(GTK_BOX(box), picker->typed_button);
	GtkExpression *expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
	picker->filter = gtk_string_filter_new(expression);
	GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(picker->names)),
		GTK_FILTER(picker->filter));
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), picker);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), picker);
	GtkWidget *view = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(filtered))), factory);
	gtk_widget_add_css_class(view, "picker-list");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(box), scroller);
	picker->join_button = gtk_button_new_with_label(TEXT_JOIN);
	gtk_widget_add_css_class(picker->join_button, "pill");
	gtk_widget_add_css_class(picker->join_button, "suggested-action");
	gtk_widget_set_halign(picker->join_button, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(picker->join_button, 4);
	g_signal_connect(picker->join_button, "clicked", G_CALLBACK(join_chosen), picker);
	gtk_box_append(GTK_BOX(box), picker->join_button);
	refresh_join_button(picker);
	return box;
}


AdwNavigationPage *create_room_picker_page(Window *window, CrimsonNetwork *network, AdwDialog *dialog) {
	Picker *picker = g_new0(Picker, 1);
	picker->window = window;
	picker->network = network;
	picker->dialog = dialog;
	picker->names = gtk_string_list_new(NULL);
	picker->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)free_room_copy);
	picker->chosen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	picker->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(picker->stack), create_loading_page(picker), "loading");
	gtk_stack_add_named(GTK_STACK(picker->stack), create_rooms_page(picker), "rooms");
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_help_button(TEXT_HELP_ROOMS));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), picker->stack);
	AdwNavigationPage *page = adw_navigation_page_new(toolbar, TEXT_ROOMS);
	g_object_set_data_full(G_OBJECT(page), "picker", picker, (GDestroyNotify)free_picker);
	/* Without a connection there is no list to wait for, and a typed name still joins once it is back */
	if (network->state == NETWORK_CONNECTED)
		request_room_listing(network, show_rooms, wait_for_rooms, picker);
	else
		gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "rooms");
	return page;
}


void show_room_picker(Window *window, CrimsonNetwork *network) {
	AdwDialog *dialog = adw_dialog_new();
	adw_dialog_set_content_width(dialog, PICKER_WIDTH);
	adw_dialog_set_content_height(dialog, PICKER_HEIGHT);
	GtkWidget *pages = adw_navigation_view_new();
	adw_navigation_view_push(ADW_NAVIGATION_VIEW(pages), create_room_picker_page(window, network, dialog));
	adw_dialog_set_child(dialog, pages);
	adw_dialog_present(dialog, GTK_WIDGET(window->window));
}
