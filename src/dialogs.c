#include "../lang/en.h"
#include "dialogs.h"

#define DIALOG_WIDTH 480
#define DIALOG_HEIGHT 640
#define DEFAULT_TLS_PORT 6697
#define DEFAULT_PLAIN_PORT 6667

/* The rows of the server form and what the form is for */
typedef struct {
	Window *window;
	AdwDialog *dialog;
	GtkWidget *save_button;
	GtkWidget *name, *host, *port, *tls, *nick, *username, *realname, *account, *password, *server_password;
	Session *editing;
	CrimsonNetwork *bouncer_network;
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


/* A form dialog with Cancel and a suggested button in its header, around a preferences page */
static AdwDialog *create_form_dialog(const char *title, const char *action, GtkWidget *page, GtkWidget **save) {
	AdwDialog *dialog = adw_dialog_new();
	adw_dialog_set_title(dialog, title);
	adw_dialog_set_content_width(dialog, DIALOG_WIDTH);
	adw_dialog_set_content_height(dialog, DIALOG_HEIGHT);
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL);
	g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), dialog);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
	*save = gtk_button_new_with_label(action);
	gtk_widget_add_css_class(*save, "suggested-action");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), *save);
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
	if (read_row(form->nick))
		put_attribute(attributes, "nickname", read_row(form->nick));
	if (read_row(form->username))
		put_attribute(attributes, "username", read_row(form->username));
	if (read_row(form->realname))
		put_attribute(attributes, "realname", read_row(form->realname));
	if (read_row(form->server_password))
		put_attribute(attributes, "pass", read_row(form->server_password));
	if (read_row(form->account))
		put_attribute(attributes, "sasl-username", read_row(form->account));
	if (read_row(form->password))
		put_attribute(attributes, "sasl-password", read_row(form->password));
	return attributes;
}


static AccountRecord *collect_account(ServerForm *form) {
	AccountRecord *record = g_new0(AccountRecord, 1);
	record->id = g_strdup(form->editing->record->id);
	record->name = g_strdup(read_row(form->name));
	record->host = g_strdup(read_row(form->host));
	record->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(form->port));
	record->uses_tls = adw_switch_row_get_active(ADW_SWITCH_ROW(form->tls));
	record->nick = g_strdup(read_row(form->nick));
	record->username = g_strdup(read_row(form->username));
	record->realname = g_strdup(read_row(form->realname));
	record->password = g_strdup(read_row(form->server_password));
	record->sasl_username = g_strdup(read_row(form->account));
	record->sasl_password = g_strdup(read_row(form->password));
	record->trusted_certificate = g_strdup(form->editing->record->trusted_certificate);
	return record;
}


static void save_server_form(GtkButton *button, ServerForm *form) {
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
	GtkWidget *server = add_group(page, TEXT_SERVER, NULL);
	const char *port_text = lookup_attribute(network, "port");
	int port = record ? record->port : port_text ? (int)g_ascii_strtoll(port_text, NULL, 10) : DEFAULT_TLS_PORT;
	form->name = add_entry_row(server, TEXT_NAME, record ? record->name : lookup_attribute(network, "name"));
	form->host = add_entry_row(server, TEXT_ADDRESS, record ? record->host : lookup_attribute(network, "host"));
	form->port = add_port_row(server, port);
	form->tls = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(form->tls), TEXT_USE_TLS);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(form->tls), TEXT_USE_TLS_DETAIL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(form->tls), record ? record->uses_tls
		: g_strcmp0(lookup_attribute(network, "tls"), "0") != 0);
	g_signal_connect(form->tls, "notify::active", G_CALLBACK(switch_tls), form);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(server), form->tls);
	GtkWidget *identity = add_group(page, TEXT_IDENTITY, NULL);
	form->nick = add_entry_row(identity, TEXT_NICKNAME, record ? record->nick : lookup_attribute(network, "nickname"));
	form->username = add_entry_row(identity, TEXT_USERNAME, record ? record->username
		: lookup_attribute(network, "username"));
	form->realname = add_entry_row(identity, TEXT_REAL_NAME, record ? record->realname
		: lookup_attribute(network, "realname"));
	GtkWidget *login = add_group(page, TEXT_LOGIN, TEXT_LOGIN_DETAIL);
	form->account = add_entry_row(login, TEXT_ACCOUNT, record ? record->sasl_username
		: lookup_attribute(network, "sasl-username"));
	form->password = add_password_row(login, TEXT_PASSWORD, record ? record->sasl_password : NULL);
	form->server_password = add_password_row(login, TEXT_SERVER_PASSWORD, record ? record->password : NULL);
	g_signal_connect(form->host, "changed", G_CALLBACK(check_server_form), form);
}


static void open_server_form(Window *window, Session *editing, CrimsonNetwork *bouncer_network) {
	ServerForm *form = g_new0(ServerForm, 1);
	form->window = window;
	form->editing = editing;
	form->bouncer_network = bouncer_network;
	GtkWidget *page = adw_preferences_page_new();
	build_server_form(form, page);
	form->dialog = create_form_dialog(TEXT_SERVER_SETTINGS, TEXT_SAVE, page, &form->save_button);
	g_object_set_data_full(G_OBJECT(form->dialog), "form", form, g_free);
	g_signal_connect(form->save_button, "clicked", G_CALLBACK(save_server_form), form);
	check_server_form(NULL, form);
	adw_dialog_present(form->dialog, GTK_WIDGET(window->window));
	gtk_widget_grab_focus(read_row(form->host) ? form->nick : form->host);
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


static void finish_bouncer_removal(AdwAlertDialog *alert, const char *response, BouncerForm *form) {
	if (g_strcmp0(response, "remove") != 0)
		return;
	adw_dialog_close(form->dialog);
	remove_account(form->editing);
}


static void ask_bouncer_removal(GtkButton *button, BouncerForm *form) {
	AdwDialog *alert = adw_alert_dialog_new(TEXT_REMOVE_BOUNCER_TITLE, TEXT_REMOVE_BOUNCER_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "remove", TEXT_REMOVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
	g_signal_connect(alert, "response", G_CALLBACK(finish_bouncer_removal), form);
	adw_dialog_present(alert, GTK_WIDGET(form->dialog));
}


void show_bouncer_dialog(Window *window, Session *editing) {
	BouncerForm *form = g_new0(BouncerForm, 1);
	form->window = window;
	form->editing = editing;
	const AccountRecord *record = editing->record;
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *group = add_group(page, TEXT_BOUNCER, TEXT_BOUNCER_DETAIL);
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
	g_signal_connect(remove, "clicked", G_CALLBACK(ask_bouncer_removal), form);
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
		open_server_form(window, network->session, NULL);
		return;
	}
	open_server_form(window, NULL, network);
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


typedef struct {
	CrimsonNetwork *network;
	Window *window;
	GtkWidget *entry;
	gboolean is_join;
} AskForm;


static void finish_ask(AdwAlertDialog *alert, const char *response, AskForm *form) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(form->entry));
	if (g_strcmp0(response, "go") != 0 || !*text)
		return;
	g_autofree char *trimmed = g_strstrip(g_strdup(text));
	if (form->is_join) {
		join_channel(form->network, trimmed, NULL);
		return;
	}
	open_room(form->window, open_private_room(form->network, trimmed));
}


static void ask_for_name(Window *window, CrimsonNetwork *network, gboolean is_join) {
	g_autofree char *body = g_strdup_printf(is_join ? TEXT_JOIN_BODY : TEXT_MESSAGE_BODY, network->name);
	AdwDialog *alert = adw_alert_dialog_new(is_join ? TEXT_JOIN_ROOM : TEXT_MESSAGE_SOMEONE, body);
	AskForm *form = g_new0(AskForm, 1);
	form->network = network;
	form->window = window;
	form->is_join = is_join;
	form->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(form->entry), is_join ? TEXT_CHANNEL_PLACEHOLDER
		: TEXT_NICK_PLACEHOLDER);
	gtk_entry_set_activates_default(GTK_ENTRY(form->entry), TRUE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), form->entry);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "go", is_join ? TEXT_JOIN
		: TEXT_OPEN_CHAT, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "go", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "go");
	g_object_set_data_full(G_OBJECT(alert), "form", form, g_free);
	g_signal_connect(alert, "response", G_CALLBACK(finish_ask), form);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
	gtk_widget_grab_focus(form->entry);
}


void show_join_dialog(Window *window, CrimsonNetwork *network) {
	ask_for_name(window, network, TRUE);
}


void show_message_dialog(Window *window, CrimsonNetwork *network) {
	ask_for_name(window, network, FALSE);
}


static void finish_trust(AdwAlertDialog *alert, const char *response, gpointer data) {
	Session *session = g_object_get_data(G_OBJECT(alert), "session");
	const char *hash = g_object_get_data(G_OBJECT(alert), "hash");
	if (g_strcmp0(response, "trust") == 0)
		trust_certificate(session, hash);
}


void ask_certificate_trust(Window *window, Session *session, const char *hash) {
	if (!hash || adw_application_window_get_visible_dialog(window->window))
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
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void finish_removal(AdwAlertDialog *alert, const char *response, gpointer data) {
	CrimsonNetwork *network = data;
	Window *window = get_main_window();
	if (g_strcmp0(response, "remove") != 0)
		return;
	CrimsonRoom *open = get_open_room(window);
	if (open && open->network == network)
		show_conversation_room(window->conversation, NULL);
	choose_network(window, NULL);
	if (network->bouncer_id)
		delete_bouncer_network(network);
	else
		remove_account(network->session);
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
