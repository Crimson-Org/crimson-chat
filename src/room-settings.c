#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "modes.h"
#include "room-settings.h"
#include "session.h"

#define SETTINGS_KEY "settings"

/* Room settings every server shares that are on or off, and what each means */
static const struct {
	char letter;
	const char *title, *note;
	gboolean is_about_writing;
} SWITCHES[] = {
	{'i', TEXT_SETTING_INVITE_ONLY, TEXT_SETTING_INVITE_ONLY_NOTE, FALSE},
	{'s', TEXT_SETTING_HIDDEN, TEXT_SETTING_HIDDEN_NOTE, FALSE},
	{'m', TEXT_SETTING_MUTED, TEXT_SETTING_MUTED_NOTE, TRUE},
	{'n', TEXT_SETTING_MEMBERS_WRITE, TEXT_SETTING_MEMBERS_WRITE_NOTE, TRUE},
	{'t', TEXT_SETTING_TOPIC_LOCK, TEXT_SETTING_TOPIC_LOCK_NOTE, TRUE},
};

/* The lists a room keeps, and the RPL_ISUPPORT token a server names a list with where not every server has it */
static const struct {
	char letter;
	const char *token, *title;
} LISTS[] = {
	{'b', NULL, TEXT_LIST_BANS},
	{'e', "EXCEPTS", TEXT_LIST_EXCEPTIONS},
	{'I', "INVEX", TEXT_LIST_INVITES},
};

/* The settings page of one room, with the rows that follow what the server says */
typedef struct {
	CrimsonRoom *room;
	GtkWidget *switches[G_N_ELEMENTS(SWITCHES)];
	GtkWidget *password, *limit, *topic, *note;
	GtkWidget *lists[G_N_ELEMENTS(LISTS)];
	/* Set while rows show what the server says, so only the user's own changes go to the server */
	gboolean is_filling;
	gulong changed_handler;
} RoomSettings;

/* One list page, with the rows its entries fill */
typedef struct {
	CrimsonRoom *room;
	char letter;
	GtkWidget *entries;
	gulong changed_handler;
} ListPage;


static void free_settings(RoomSettings *settings) {
	g_signal_handler_disconnect(settings->room, settings->changed_handler);
	g_object_unref(settings->room);
	g_free(settings);
}


static void free_list_page(ListPage *page) {
	g_signal_handler_disconnect(page->room, page->changed_handler);
	g_object_unref(page->room);
	g_free(page);
}


/* A row the user is typing in keeps what they typed */
static void fill_text_row(GtkWidget *row, const char *text) {
	if (!(gtk_widget_get_state_flags(row) & GTK_STATE_FLAG_FOCUS_WITHIN))
		gtk_editable_set_text(GTK_EDITABLE(row), text ? text : "");
}


static IrcChannel *find_room_channel(CrimsonRoom *room, IrcState **state) {
	*state = get_network_state(room->network);
	return *state && room->is_joined ? find_irc_channel(*state, room->name) : NULL;
}


static gboolean has_list(IrcState *state, guint index) {
	return strchr(state->list_modes, LISTS[index].letter)
		&& (!LISTS[index].token || get_irc_support(state, LISTS[index].token));
}


static void send_mode(CrimsonRoom *room, const char *change, const char *value) {
	if (room->network->client)
		send_irc_client_command(room->network->client, "MODE", room->name, change, value, NULL);
}


static char *describe_count(GPtrArray *entries) {
	if (!entries)
		return g_strdup("");
	if (!entries->len)
		return g_strdup(TEXT_LIST_EMPTY);
	return g_strdup_printf(entries->len == 1 ? TEXT_LIST_COUNT_ONE : TEXT_LIST_COUNT, entries->len);
}


static GPtrArray *find_entries(IrcChannel *channel, char letter) {
	char key[2] = {letter, '\0'};
	return channel ? g_hash_table_lookup(channel->lists, key) : NULL;
}


static void fill_settings(RoomSettings *settings) {
	IrcState *state = NULL;
	IrcChannel *channel = find_room_channel(settings->room, &state);
	gboolean can_change = channel && is_irc_channel_moderator(state, channel);
	settings->is_filling = TRUE;
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++) {
		char key[2] = {SWITCHES[index].letter, '\0'};
		adw_switch_row_set_active(ADW_SWITCH_ROW(settings->switches[index]), channel
			&& g_hash_table_contains(channel->modes, key));
		gtk_widget_set_sensitive(settings->switches[index], can_change);
	}
	const char *key = channel ? g_hash_table_lookup(channel->modes, "k") : NULL;
	const char *limit = channel ? g_hash_table_lookup(channel->modes, "l") : NULL;
	fill_text_row(settings->password, key);
	fill_text_row(settings->limit, limit);
	fill_text_row(settings->topic, settings->room->topic);
	gtk_widget_set_sensitive(settings->password, can_change);
	gtk_widget_set_sensitive(settings->limit, can_change);
	gboolean is_topic_locked = channel && g_hash_table_contains(channel->modes, "t");
	gtk_widget_set_sensitive(settings->topic, channel && (can_change || !is_topic_locked));
	gtk_widget_set_visible(settings->note, !can_change);
	for (guint index = 0; index < G_N_ELEMENTS(LISTS); index++) {
		GPtrArray *entries = find_entries(channel, LISTS[index].letter);
		g_autofree char *count = describe_count(entries);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(settings->lists[index]), count);
		/* Servers often keep lists from people who cannot change them, and such a list is left out */
		gtk_widget_set_visible(settings->lists[index], state && has_list(state, index) && (entries || can_change));
	}
	settings->is_filling = FALSE;
}


static void change_switch(AdwSwitchRow *row, GParamSpec *spec, RoomSettings *settings) {
	if (settings->is_filling)
		return;
	char letter = (char)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "letter"));
	char change[3] = {adw_switch_row_get_active(row) ? '+' : '-', letter, '\0'};
	send_mode(settings->room, change, NULL);
}


/* An empty password or limit takes the setting away */
static void apply_password(AdwEntryRow *row, RoomSettings *settings) {
	const char *key = gtk_editable_get_text(GTK_EDITABLE(row));
	send_mode(settings->room, *key ? "+k" : "-k", *key ? key : "*");
}


static void apply_limit(AdwEntryRow *row, RoomSettings *settings) {
	gint64 limit = g_ascii_strtoll(gtk_editable_get_text(GTK_EDITABLE(row)), NULL, 10);
	g_autofree char *value = g_strdup_printf("%" G_GINT64_FORMAT, limit);
	send_mode(settings->room, limit > 0 ? "+l" : "-l", limit > 0 ? value : NULL);
}


static void apply_topic(AdwEntryRow *row, RoomSettings *settings) {
	if (settings->room->network->client)
		send_irc_client_command(settings->room->network->client, "TOPIC", settings->room->name,
			gtk_editable_get_text(GTK_EDITABLE(row)), NULL);
}


/* A nickname alone covers that nickname from any address, as people expect a ban by name to work */
static void add_list_entry(AdwEntryRow *row, ListPage *page) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
	if (!*text)
		return;
	g_autofree char *mask = strpbrk(text, "!@$:") ? g_strdup(text) : g_strconcat(text, "!*@*", NULL);
	char change[3] = {'+', page->letter, '\0'};
	send_mode(page->room, change, mask);
	gtk_editable_set_text(GTK_EDITABLE(row), "");
}


static void remove_list_entry(GtkButton *button, ListPage *page) {
	char change[3] = {'-', page->letter, '\0'};
	send_mode(page->room, change, g_object_get_data(G_OBJECT(button), "mask"));
}


static GtkWidget *create_entry_row(ListPage *page, const IrcListEntry *entry) {
	g_autofree char *title = describe_mask(entry->mask);
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
	adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
	gtk_widget_set_tooltip_text(row, entry->mask);
	if (entry->setter && *entry->setter) {
		g_autofree char *setter = get_source_nick(entry->setter);
		g_autofree char *day = entry->time ? format_day_heading(entry->time * G_USEC_PER_SEC) : NULL;
		g_autofree char *by = day ? g_strdup_printf(TEXT_LIST_SET_BY, setter, day) : g_strdup(setter);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), by);
	}
	GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
	gtk_widget_set_tooltip_text(remove, TEXT_LIST_REMOVE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_LIST_REMOVE, -1);
	gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(remove, "flat");
	g_object_set_data_full(G_OBJECT(remove), "mask", g_strdup(entry->mask), g_free);
	g_signal_connect(remove, "clicked", G_CALLBACK(remove_list_entry), page);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
	return row;
}


static void fill_list_page(ListPage *page) {
	IrcState *state = NULL;
	IrcChannel *channel = find_room_channel(page->room, &state);
	GPtrArray *entries = find_entries(channel, page->letter);
	gtk_list_box_remove_all(GTK_LIST_BOX(page->entries));
	gboolean can_change = channel && is_irc_channel_moderator(state, channel);
	for (guint index = 0; entries && index < entries->len; index++) {
		GtkWidget *row = create_entry_row(page, g_ptr_array_index(entries, index));
		gtk_widget_set_sensitive(row, can_change);
		gtk_list_box_append(GTK_LIST_BOX(page->entries), row);
	}
	gtk_widget_set_visible(page->entries, entries && entries->len);
}


static GtkWidget *create_group(const char *title) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	return group;
}


static AdwNavigationPage *wrap_page(GtkWidget *content, const char *title) {
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
	return adw_navigation_page_new(toolbar, title);
}


static AdwNavigationPage *create_list_page(CrimsonRoom *room, guint index) {
	ListPage *page = g_new0(ListPage, 1);
	page->room = g_object_ref(room);
	page->letter = LISTS[index].letter;
	GtkWidget *content = adw_preferences_page_new();
	GtkWidget *group = create_group(NULL);
	GtkWidget *adding = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(adding), TEXT_LIST_ADD);
	adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(adding), TRUE);
	g_signal_connect(adding, "apply", G_CALLBACK(add_list_entry), page);
	g_signal_connect(adding, "entry-activated", G_CALLBACK(add_list_entry), page);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), adding);
	page->entries = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->entries), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(page->entries, "boxed-list");
	gtk_widget_set_margin_top(page->entries, 12);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), page->entries);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(content), ADW_PREFERENCES_GROUP(group));
	IrcState *state = NULL;
	IrcChannel *channel = find_room_channel(room, &state);
	gtk_widget_set_sensitive(adding, channel && is_irc_channel_moderator(state, channel));
	AdwNavigationPage *navigation = wrap_page(content, LISTS[index].title);
	g_object_set_data_full(G_OBJECT(navigation), SETTINGS_KEY, page, (GDestroyNotify)free_list_page);
	page->changed_handler = g_signal_connect_swapped(room, "changed", G_CALLBACK(fill_list_page), page);
	fill_list_page(page);
	return navigation;
}


static void open_list(AdwActionRow *row, RoomSettings *settings) {
	guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "list"));
	GtkWidget *view = gtk_widget_get_ancestor(GTK_WIDGET(row), ADW_TYPE_NAVIGATION_VIEW);
	if (view)
		adw_navigation_view_push(ADW_NAVIGATION_VIEW(view), create_list_page(settings->room, index));
}


static GtkWidget *create_text_row(const char *title, GCallback apply, RoomSettings *settings) {
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
	g_signal_connect(row, "apply", apply, settings);
	return row;
}


/* The server is asked for the room's settings and lists each time the page opens, and the rows follow its answers */
static void request_settings(CrimsonRoom *room) {
	IrcState *state = NULL;
	IrcChannel *channel = find_room_channel(room, &state);
	send_mode(room, NULL, NULL);
	gboolean can_change = channel && is_irc_channel_moderator(state, channel);
	/* Everyone may see the bans, while the other lists are only asked for by those who can change them */
	for (guint index = 0; state && index < (can_change ? G_N_ELEMENTS(LISTS) : 1); index++) {
		char listing[3] = {'+', LISTS[index].letter, '\0'};
		if (has_list(state, index))
			send_mode(room, listing, NULL);
	}
}


AdwNavigationPage *create_room_settings_page(CrimsonRoom *room) {
	RoomSettings *settings = g_new0(RoomSettings, 1);
	settings->room = g_object_ref(room);
	GtkWidget *content = adw_preferences_page_new();
	GtkWidget *joining = create_group(TEXT_SETTINGS_JOINING);
	GtkWidget *writing = create_group(TEXT_SETTINGS_WRITING);
	settings->note = gtk_label_new(TEXT_SETTINGS_READ_ONLY);
	gtk_widget_add_css_class(settings->note, "dim-label");
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(joining), settings->note);
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++) {
		GtkWidget *row = adw_switch_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), SWITCHES[index].title);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), SWITCHES[index].note);
		g_object_set_data(G_OBJECT(row), "letter", GINT_TO_POINTER(SWITCHES[index].letter));
		g_signal_connect(row, "notify::active", G_CALLBACK(change_switch), settings);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(SWITCHES[index].is_about_writing ? writing : joining), row);
		settings->switches[index] = row;
	}
	settings->password = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(settings->password), TEXT_SETTING_PASSWORD);
	adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(settings->password), TRUE);
	g_signal_connect(settings->password, "apply", G_CALLBACK(apply_password), settings);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(joining), settings->password);
	settings->limit = create_text_row(TEXT_SETTING_LIMIT, G_CALLBACK(apply_limit), settings);
	adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(settings->limit), GTK_INPUT_PURPOSE_DIGITS);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(joining), settings->limit);
	settings->topic = create_text_row(TEXT_SETTING_TOPIC, G_CALLBACK(apply_topic), settings);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(writing), settings->topic);
	GtkWidget *people = create_group(TEXT_SETTINGS_LISTS);
	for (guint index = 0; index < G_N_ELEMENTS(LISTS); index++) {
		GtkWidget *row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), LISTS[index].title);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
		g_object_set_data(G_OBJECT(row), "list", GUINT_TO_POINTER(index));
		g_signal_connect(row, "activated", G_CALLBACK(open_list), settings);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(people), row);
		settings->lists[index] = row;
	}
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(content), ADW_PREFERENCES_GROUP(joining));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(content), ADW_PREFERENCES_GROUP(writing));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(content), ADW_PREFERENCES_GROUP(people));
	AdwNavigationPage *page = wrap_page(content, TEXT_ROOM_SETTINGS);
	g_object_set_data_full(G_OBJECT(page), SETTINGS_KEY, settings, (GDestroyNotify)free_settings);
	settings->changed_handler = g_signal_connect_swapped(room, "changed", G_CALLBACK(fill_settings), settings);
	fill_settings(settings);
	request_settings(room);
	return page;
}
