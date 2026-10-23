#include <string.h>

#include "../lang/en.h"
#include "dialogs.h"
#include "images.h"
#include "pictures.h"
#include "room-look.h"
#include "wizards.h"

#define DIALOG_WIDTH 480
#define DIALOG_HEIGHT 640
#define DEFAULT_TLS_PORT 6697
#define DEFAULT_PLAIN_PORT 6667
#define SERVER_AVATAR_SIZE 96
/* The people offered for a new private chat: how many show at once, their picture size and how far back each room
 * is read to see who spoke last */
#define PEOPLE_SHOWN 10
#define PERSON_AVATAR_SIZE 32
#define PEOPLE_SCANNED_LINES 500

/* The rows of the server form and what the form is for */
typedef struct {
	Window *window;
	AdwDialog *dialog;
	GtkWidget *save_button;
	GtkWidget *name, *host, *port, *tls, *server_password;
	Session *editing;
	CrimsonNetwork *bouncer_network;
	/* The server the form changes, whose picture sits at the top */
	CrimsonNetwork *network;
	GtkWidget *avatar;
} ServerForm;

/* The rows of the bouncer form */
typedef struct {
	Window *window;
	AdwDialog *dialog;
	GtkWidget *save_button, *name, *host, *port, *username, *password;
	Session *editing;
} BouncerForm;


static GtkWidget *add_entry_row(GtkWidget *group, const char *title, const char *value) {
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	return row;
}


static GtkWidget *add_password_row(GtkWidget *group, const char *title, const char *value) {
	GtkWidget *row = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	return row;
}


static GtkWidget *add_port_row(GtkWidget *group, int port) {
	GtkWidget *row = adw_spin_row_new_with_range(1, G_MAXUINT16, 1);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_PORT);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), port);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	return row;
}


static GtkWidget *add_group(GtkWidget *page, const char *title, const char *description) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	if (description)
		adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), description);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
	return group;
}


static const char *read_row(GtkWidget *row) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
	return text && *text ? text : NULL;
}


GtkWidget *create_form_header(AdwDialog *dialog, const char *action, GtkWidget **save) {
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL);
	g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), dialog);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
	*save = gtk_button_new_with_label(action);
	gtk_widget_add_css_class(*save, "suggested-action");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), *save);
	return header;
}


/* A form dialog with Cancel and a suggested button in its header, around a preferences page */
static AdwDialog *create_form_dialog(const char *title, const char *action, GtkWidget *page, GtkWidget **save) {
	AdwDialog *dialog = adw_dialog_new();
	adw_dialog_set_title(dialog, title);
	adw_dialog_set_content_width(dialog, DIALOG_WIDTH);
	adw_dialog_set_content_height(dialog, DIALOG_HEIGHT);
	GtkWidget *header = create_form_header(dialog, action, save);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
	adw_dialog_set_child(dialog, toolbar);
	return dialog;
}


static void check_server_form(GtkEditable *editable, ServerForm *form) {
	gtk_widget_set_sensitive(form->save_button, read_row(form->host) != NULL);
}


/* Moving between encrypted and plain connections takes the port along when it was the usual one */
static void switch_tls(AdwSwitchRow *row, GParamSpec *spec, ServerForm *form) {
	gboolean uses_tls = adw_switch_row_get_active(row);
	int port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(form->port));
	if (uses_tls && port == DEFAULT_PLAIN_PORT)
		adw_spin_row_set_value(ADW_SPIN_ROW(form->port), DEFAULT_TLS_PORT);
	else if (!uses_tls && port == DEFAULT_TLS_PORT)
		adw_spin_row_set_value(ADW_SPIN_ROW(form->port), DEFAULT_PLAIN_PORT);
}


static void put_attribute(GHashTable *attributes, const char *name, const char *value) {
	g_hash_table_insert(attributes, g_strdup(name), g_strdup(value ? value : ""));
}


static GHashTable *collect_attributes(ServerForm *form) {
	GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autofree char *port = g_strdup_printf("%d", (int)adw_spin_row_get_value(ADW_SPIN_ROW(form->port)));
	put_attribute(attributes, "host", read_row(form->host));
	put_attribute(attributes, "port", port);
	put_attribute(attributes, "tls", adw_switch_row_get_active(ADW_SWITCH_ROW(form->tls)) ? "1" : "0");
	if (read_row(form->name))
		put_attribute(attributes, "name", read_row(form->name));
	if (read_row(form->server_password))
		put_attribute(attributes, "pass", read_row(form->server_password));
	return attributes;
}


static AccountRecord *collect_account(ServerForm *form) {
	AccountRecord *record = g_new0(AccountRecord, 1);
	record->id = g_strdup(form->editing->record->id);
	record->name = g_strdup(read_row(form->name));
	record->host = g_strdup(read_row(form->host));
	record->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(form->port));
	record->uses_tls = adw_switch_row_get_active(ADW_SWITCH_ROW(form->tls));
	/* The nickname and real name belong to the profile, so the server settings keep them as they are */
	record->nick = g_strdup(form->editing->record->nick);
	record->username = g_strdup(form->editing->record->username);
	record->realname = g_strdup(form->editing->record->realname);
	record->channels = g_strdupv(form->editing->record->channels);
	record->password = g_strdup(read_row(form->server_password));
	/* The login lives in the account dialog, so the server settings keep it as it is */
	record->sasl_username = g_strdup(form->editing->record->sasl_username);
	record->sasl_password = g_strdup(form->editing->record->sasl_password);
	record->trusted_certificate = g_strdup(form->editing->record->trusted_certificate);
	return record;
}


static void save_server_form(GtkButton *button, ServerForm *form) {
	if (form->avatar)
		save_picture_choice(form->avatar);
	if (form->bouncer_network) {
		g_autoptr(GHashTable) attributes = collect_attributes(form);
		change_bouncer_network(form->bouncer_network, attributes);
	} else {
		AccountRecord *record = collect_account(form);
		change_account(form->editing, record);
		free_account_record(record);
	}
	adw_dialog_close(form->dialog);
}


static const char *lookup_attribute(CrimsonNetwork *network, const char *name) {
	return network ? g_hash_table_lookup(network->attributes, name) : NULL;
}


static void build_server_form(ServerForm *form, GtkWidget *page) {
	const AccountRecord *record = form->editing ? form->editing->record : NULL;
	CrimsonNetwork *network = form->bouncer_network;
	/* The server's picture leads the form, the way a profile leads with its person, and clicking it changes it */
	if (form->network) {
		GtkWidget *picture = adw_preferences_group_new();
		GtkWidget *avatar = adw_avatar_new(SERVER_AVATAR_SIZE, form->network->name, TRUE);
		gtk_widget_add_css_class(avatar, "room-avatar");
		gtk_widget_set_halign(avatar, GTK_ALIGN_CENTER);
		g_autofree char *sized = fill_image_size(pick_network_picture(form->network), SERVER_AVATAR_SIZE * 2);
		show_avatar_image(ADW_AVATAR(avatar), sized);
		GtkWidget *choice = offer_picture_choice(avatar, form->window, form->network, PICTURE_SERVER, NULL, TRUE);
		form->avatar = avatar;
		gtk_widget_set_halign(choice, GTK_ALIGN_CENTER);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(picture), choice);
		adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(picture));
	}
	GtkWidget *server = add_group(page, TEXT_SERVER, NULL);
	const char *port_text = lookup_attribute(network, "port");
	int port = record ? record->port : port_text ? (int)g_ascii_strtoll(port_text, NULL, 10) : DEFAULT_TLS_PORT;
	form->name = add_entry_row(server, TEXT_NAME, record ? record->name : lookup_attribute(network, "name"));
	form->host = add_entry_row(server, TEXT_ADDRESS, record ? record->host : lookup_attribute(network, "host"));
	form->port = add_port_row(server, port);
	form->tls = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(form->tls), TEXT_USE_TLS);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(form->tls), NULL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(form->tls), record ? record->uses_tls
		: g_strcmp0(lookup_attribute(network, "tls"), "0") != 0);
	g_signal_connect(form->tls, "notify::active", G_CALLBACK(switch_tls), form);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(server), form->tls);
	form->server_password = add_password_row(server, TEXT_SERVER_PASSWORD, record ? record->password : NULL);
	g_signal_connect(form->host, "changed", G_CALLBACK(check_server_form), form);
}


static void open_server_form(Window *window, Session *editing, CrimsonNetwork *bouncer_network,
	CrimsonNetwork *network) {
	ServerForm *form = g_new0(ServerForm, 1);
	form->window = window;
	form->editing = editing;
	form->bouncer_network = bouncer_network;
	form->network = network;
	GtkWidget *page = adw_preferences_page_new();
	build_server_form(form, page);
	form->dialog = create_form_dialog(TEXT_SERVER_SETTINGS, TEXT_SAVE, page, &form->save_button);
	g_object_set_data_full(G_OBJECT(form->dialog), "form", form, g_free);
	g_signal_connect(form->save_button, "clicked", G_CALLBACK(save_server_form), form);
	check_server_form(NULL, form);
	adw_dialog_present(form->dialog, GTK_WIDGET(window->window));
	gtk_widget_grab_focus(form->host);
}


static void check_bouncer_form(GtkEditable *editable, BouncerForm *form) {
	gtk_widget_set_sensitive(form->save_button, read_row(form->host) && read_row(form->username)
		&& read_row(form->password));
}


static void save_bouncer_form(GtkButton *button, BouncerForm *form) {
	AccountRecord *record = g_new0(AccountRecord, 1);
	record->id = g_strdup(form->editing->record->id);
	record->is_bouncer = TRUE;
	record->uses_tls = TRUE;
	record->name = g_strdup(read_row(form->name));
	record->host = g_strdup(read_row(form->host));
	record->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(form->port));
	record->nick = g_strdup(read_row(form->username));
	record->sasl_username = g_strdup(read_row(form->username));
	record->sasl_password = g_strdup(read_row(form->password));
	record->trusted_certificate = g_strdup(form->editing->record->trusted_certificate);
	change_account(form->editing, record);
	free_account_record(record);
	adw_dialog_close(form->dialog);
}


static void finish_bouncer_removal(AdwAlertDialog *alert, const char *response, gpointer data) {
	Session *session = data;
	AdwDialog *settings = g_object_get_data(G_OBJECT(alert), "settings");
	if (g_strcmp0(response, "remove") != 0)
		return;
	if (settings)
		adw_dialog_close(settings);
	Window *window = get_main_window();
	for (guint index = 0; index < session->networks->len; index++)
		forget_network_in_window(window, g_ptr_array_index(session->networks, index));
	remove_account(session);
}


/* Asks before disconnecting from the bouncer, over its settings when they are open and over the window
 * otherwise; the servers stay on the bouncer */
static void ask_bouncer_removal(Window *window, Session *session, AdwDialog *settings) {
	g_autofree char *title = g_strdup_printf(TEXT_DISCONNECT_BOUNCER_TITLE, describe_account_name(session));
	AdwDialog *alert = adw_alert_dialog_new(title, TEXT_DISCONNECT_BOUNCER_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "remove", TEXT_DISCONNECT,
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
	g_object_set_data(G_OBJECT(alert), "settings", settings);
	g_signal_connect(alert, "response", G_CALLBACK(finish_bouncer_removal), session);
	adw_dialog_present(alert, settings ? GTK_WIDGET(settings) : GTK_WIDGET(window->window));
}


static void click_bouncer_removal(GtkButton *button, BouncerForm *form) {
	ask_bouncer_removal(form->window, form->editing, form->dialog);
}


void show_bouncer_dialog(Window *window, Session *editing) {
	BouncerForm *form = g_new0(BouncerForm, 1);
	form->window = window;
	form->editing = editing;
	const AccountRecord *record = editing->record;
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *group = add_group(page, TEXT_BOUNCER, NULL);
	form->name = add_entry_row(group, TEXT_NAME, record->name);
	form->host = add_entry_row(group, TEXT_ADDRESS, record->host);
	form->port = add_port_row(group, record->port);
	form->username = add_entry_row(group, TEXT_USERNAME, record->sasl_username);
	form->password = add_password_row(group, TEXT_PASSWORD, record->sasl_password);
	GtkWidget *danger = add_group(page, NULL, NULL);
	GtkWidget *remove = gtk_button_new_with_label(TEXT_REMOVE_BOUNCER);
	gtk_widget_add_css_class(remove, "destructive-action");
	gtk_widget_add_css_class(remove, "pill");
	gtk_widget_set_halign(remove, GTK_ALIGN_CENTER);
	g_signal_connect(remove, "clicked", G_CALLBACK(click_bouncer_removal), form);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), remove);
	form->dialog = create_form_dialog(TEXT_BOUNCER_SETTINGS, TEXT_SAVE, page, &form->save_button);
	g_object_set_data_full(G_OBJECT(form->dialog), "form", form, g_free);
	g_signal_connect(form->save_button, "clicked", G_CALLBACK(save_bouncer_form), form);
	g_signal_connect(form->host, "changed", G_CALLBACK(check_bouncer_form), form);
	g_signal_connect(form->username, "changed", G_CALLBACK(check_bouncer_form), form);
	g_signal_connect(form->password, "changed", G_CALLBACK(check_bouncer_form), form);
	check_bouncer_form(NULL, form);
	adw_dialog_present(form->dialog, GTK_WIDGET(window->window));
}


static void open_bouncer_login(GtkButton *button, ServerForm *form) {
	Session *session = form->bouncer_network->session;
	Window *window = form->window;
	adw_dialog_close(form->dialog);
	show_bouncer_dialog(window, session);
}


void show_network_settings(Window *window, CrimsonNetwork *network) {
	if (!network->bouncer_id) {
		open_server_form(window, network->session, NULL, network);
		return;
	}
	open_server_form(window, NULL, network, network);
	AdwDialog *dialog = adw_application_window_get_visible_dialog(window->window);
	ServerForm *form = dialog ? g_object_get_data(G_OBJECT(dialog), "form") : NULL;
	if (!form)
		return;
	GtkWidget *page = gtk_widget_get_ancestor(form->host, ADW_TYPE_PREFERENCES_PAGE);
	GtkWidget *group = add_group(page, TEXT_BOUNCER, NULL);
	GtkWidget *row = adw_button_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_BOUNCER_LOGIN);
	adw_button_row_set_end_icon_name(ADW_BUTTON_ROW(row), "go-next-symbolic");
	g_signal_connect(row, "activated", G_CALLBACK(open_bouncer_login), form);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
}


/* The people a new private chat can go to: everyone in the user's rooms, those who spoke last first, filtered by
 * what is typed, while the typed name itself reaches anyone on the server */
typedef struct {
	CrimsonNetwork *network;
	Window *window;
	AdwDialog *alert;
	GtkWidget *entry, *people;
	GPtrArray *nicks;
} MessageForm;


static void free_message_form(MessageForm *form) {
	g_ptr_array_unref(form->nicks);
	g_free(form);
}


static void open_chat_with(MessageForm *form, const char *nick) {
	g_autofree char *trimmed = g_strstrip(g_strdup(nick));
	if (!*trimmed)
		return;
	open_room(form->window, open_private_room(form->network, trimmed));
}


static void finish_message(AdwAlertDialog *alert, const char *response, MessageForm *form) {
	if (g_strcmp0(response, "go") == 0)
		open_chat_with(form, gtk_editable_get_text(GTK_EDITABLE(form->entry)));
}


static void pick_person(GtkListBox *people, GtkListBoxRow *row, MessageForm *form) {
	const char *nick = g_object_get_data(G_OBJECT(row), "nick");
	open_chat_with(form, nick);
	adw_dialog_close(form->alert);
}


/* When each person last spoke in the user's rooms, so the list starts with whoever is active */
static GHashTable *read_last_spoken(CrimsonNetwork *network) {
	GHashTable *spoken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint rooms = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < rooms; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		guint count = g_list_model_get_n_items(G_LIST_MODEL(room->messages));
		for (guint line = count; line > 0 && count - line < PEOPLE_SCANNED_LINES; line--) {
			g_autoptr(CrimsonMessage) message = g_list_model_get_item(G_LIST_MODEL(room->messages), line - 1);
			if (!message->sender || message->is_own)
				continue;
			g_autofree char *folded = g_utf8_casefold(message->sender, -1);
			gint64 known = GPOINTER_TO_SIZE(g_hash_table_lookup(spoken, folded));
			if ((gint64)message->time > known)
				g_hash_table_replace(spoken, g_steal_pointer(&folded), GSIZE_TO_POINTER((gsize)message->time));
		}
	}
	return spoken;
}


static GHashTable *last_spoken_order;


static int order_people(gconstpointer first, gconstpointer second) {
	const char *a = *(const char **)first, *b = *(const char **)second;
	g_autofree char *folded_a = g_utf8_casefold(a, -1);
	g_autofree char *folded_b = g_utf8_casefold(b, -1);
	gint64 time_a = GPOINTER_TO_SIZE(g_hash_table_lookup(last_spoken_order, folded_a));
	gint64 time_b = GPOINTER_TO_SIZE(g_hash_table_lookup(last_spoken_order, folded_b));
	if (time_a != time_b)
		return time_a > time_b ? -1 : 1;
	return g_utf8_collate(folded_a, folded_b);
}


static GPtrArray *collect_people(CrimsonNetwork *network) {
	GPtrArray *nicks = g_ptr_array_new_with_free_func(g_free);
	IrcState *state = get_network_state(network);
	if (!state)
		return nicks;
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GHashTableIter channels;
	gpointer channel;
	g_hash_table_iter_init(&channels, state->channels);
	while (g_hash_table_iter_next(&channels, NULL, &channel)) {
		GHashTableIter members;
		gpointer folded;
		g_hash_table_iter_init(&members, ((IrcChannel *)channel)->members);
		while (g_hash_table_iter_next(&members, &folded, NULL)) {
			IrcUser *user = g_hash_table_lookup(state->users, folded);
			if (!user || g_hash_table_contains(seen, folded) || is_own_nick(state, user->nick))
				continue;
			g_hash_table_add(seen, folded);
			g_ptr_array_add(nicks, g_strdup(user->nick));
		}
	}
	last_spoken_order = read_last_spoken(network);
	g_ptr_array_sort(nicks, order_people);
	g_clear_pointer(&last_spoken_order, g_hash_table_unref);
	return nicks;
}


static GtkWidget *create_person_row(MessageForm *form, const char *nick) {
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(box, "person-row");
	GtkWidget *avatar = adw_avatar_new(PERSON_AVATAR_SIZE, nick, TRUE);
	CrimsonRoom *private = find_room(form->network, nick);
	const char *url = private ? pick_person_picture(private, nick) : find_own_picture(form->network, PICTURE_PERSON,
		nick);
	g_autofree char *sized = fill_image_size(url, PERSON_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(avatar), sized);
	gtk_box_append(GTK_BOX(box), avatar);
	GtkWidget *name = gtk_label_new(nick);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), name);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	g_object_set_data_full(G_OBJECT(row), "nick", g_strdup(nick), g_free);
	return row;
}


/* Up to a handful of people whose names hold what is typed, or the first of them with nothing typed */
static void show_people(MessageForm *form) {
	gtk_list_box_remove_all(GTK_LIST_BOX(form->people));
	g_autofree char *typed = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(form->entry)), -1);
	g_strstrip(typed);
	guint shown = 0;
	for (guint index = 0; index < form->nicks->len && shown < PEOPLE_SHOWN; index++) {
		const char *nick = g_ptr_array_index(form->nicks, index);
		g_autofree char *folded = g_utf8_casefold(nick, -1);
		if (*typed && !strstr(folded, typed))
			continue;
		gtk_list_box_append(GTK_LIST_BOX(form->people), create_person_row(form, nick));
		shown++;
	}
	gtk_widget_set_visible(form->people, shown > 0);
}


void show_message_dialog(Window *window, CrimsonNetwork *network) {
	g_autofree char *body = g_strdup_printf(TEXT_MESSAGE_BODY, network->name);
	AdwDialog *alert = adw_alert_dialog_new(TEXT_MESSAGE_SOMEONE, body);
	MessageForm *form = g_new0(MessageForm, 1);
	form->network = network;
	form->window = window;
	form->alert = alert;
	form->nicks = collect_people(network);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	form->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(form->entry), TEXT_NICK_PLACEHOLDER);
	gtk_entry_set_activates_default(GTK_ENTRY(form->entry), TRUE);
	g_signal_connect_swapped(form->entry, "changed", G_CALLBACK(show_people), form);
	gtk_box_append(GTK_BOX(box), form->entry);
	form->people = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(form->people), GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(form->people), TRUE);
	gtk_widget_add_css_class(form->people, "boxed-list");
	g_signal_connect(form->people, "row-activated", G_CALLBACK(pick_person), form);
	gtk_box_append(GTK_BOX(box), form->people);
	show_people(form);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), box);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "go", TEXT_OPEN_CHAT, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "go", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "go");
	g_object_set_data_full(G_OBJECT(alert), "form", form, (GDestroyNotify)free_message_form);
	g_signal_connect(alert, "response", G_CALLBACK(finish_message), form);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
	gtk_widget_grab_focus(form->entry);
}


static void finish_trust(AdwAlertDialog *alert, const char *response, gpointer data) {
	Session *session = g_object_get_data(G_OBJECT(alert), "session");
	const char *hash = g_object_get_data(G_OBJECT(alert), "hash");
	if (g_strcmp0(response, "trust") == 0)
		trust_certificate(session, hash);
}


static AdwDialog *trust_alert;


static void forget_trust_alert(AdwDialog *alert, gpointer data) {
	trust_alert = NULL;
}


/* The question can come up over a guide that is still open, but only once at a time */
void ask_certificate_trust(Window *window, Session *session, const char *hash) {
	if (!hash || trust_alert)
		return;
	g_autofree char *fingerprint = format_certificate_hash(hash);
	g_autofree char *host = g_markup_escape_text(session->record->host, -1);
	g_autofree char *body = g_strdup_printf(TEXT_TRUST_BODY, host, fingerprint);
	AdwDialog *alert = adw_alert_dialog_new(TEXT_TRUST_TITLE, NULL);
	adw_alert_dialog_set_body_use_markup(ADW_ALERT_DIALOG(alert), TRUE);
	adw_alert_dialog_set_body(ADW_ALERT_DIALOG(alert), body);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "trust", TEXT_TRUST, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "trust", ADW_RESPONSE_SUGGESTED);
	g_object_set_data(G_OBJECT(alert), "session", session);
	g_object_set_data_full(G_OBJECT(alert), "hash", g_strdup(hash), g_free);
	g_signal_connect(alert, "response", G_CALLBACK(finish_trust), NULL);
	g_signal_connect(alert, "closed", G_CALLBACK(forget_trust_alert), NULL);
	trust_alert = alert;
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void finish_removal(AdwAlertDialog *alert, const char *response, gpointer data) {
	CrimsonNetwork *network = data;
	Window *window = get_main_window();
	if (g_strcmp0(response, "remove") != 0)
		return;
	forget_network_in_window(window, network);
	if (network->bouncer_id)
		delete_bouncer_network(network);
	else
		remove_account(network->session);
}


/* The window lets go of a server before it goes, since the list and the open room point into it */
void forget_network_in_window(Window *window, CrimsonNetwork *network) {
	CrimsonRoom *open = get_open_room(window);
	if (open && open->network == network)
		show_conversation_room(window->conversation, NULL);
	if (window->network == network)
		choose_network(window, NULL);
}


void confirm_bouncer_disconnect(Window *window, Session *session) {
	ask_bouncer_removal(window, session, NULL);
}


void confirm_network_removal(Window *window, CrimsonNetwork *network) {
	g_autofree char *title = g_strdup_printf(TEXT_REMOVE_NETWORK_TITLE, network->name);
	AdwDialog *alert = adw_alert_dialog_new(title, network->bouncer_id ? TEXT_REMOVE_BOUNCER_NETWORK_BODY
		: TEXT_REMOVE_SERVER_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "remove", TEXT_REMOVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
	g_signal_connect_object(alert, "response", G_CALLBACK(finish_removal), network, 0);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void finish_link_question(AdwAlertDialog *alert, const char *response, gpointer data) {
	if (g_strcmp0(response, "open") != 0)
		return;
	const char *url = g_object_get_data(G_OBJECT(alert), "url");
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(alert));
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);
	gtk_uri_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, NULL, NULL);
}


void confirm_open_link(GtkWidget *source, const char *url) {
	AdwDialog *alert = adw_alert_dialog_new(TEXT_OPEN_LINK_TITLE, NULL);
	/* A long address has no spaces to break at, so it may break anywhere */
	GtkWidget *address = gtk_label_new(url);
	gtk_label_set_wrap(GTK_LABEL(address), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(address), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(address), TRUE);
	/* Taking focus would select the whole address, so focus starts on the buttons and a drag still selects it */
	gtk_widget_set_focusable(address, FALSE);
	gtk_label_set_justify(GTK_LABEL(address), GTK_JUSTIFY_CENTER);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), address);
	g_object_set_data_full(G_OBJECT(alert), "url", g_strdup(url), g_free);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "open", TEXT_OPEN_LINK, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "open", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "open");
	g_signal_connect(alert, "response", G_CALLBACK(finish_link_question), NULL);
	adw_dialog_present(alert, source);
}


static gboolean ask_about_link(GtkLabel *label, const char *uri, gpointer data) {
	confirm_open_link(GTK_WIDGET(label), uri);
	return TRUE;
}


void ask_before_label_links(GtkWidget *label) {
	g_signal_connect(label, "activate-link", G_CALLBACK(ask_about_link), NULL);
}
