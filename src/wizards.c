#include <string.h>

#include "../lang/en.h"
#include "wizards.h"

#define GUIDE_WIDTH 460
#define GUIDE_HEIGHT 620
#define CONTENT_WIDTH 400
#define ICON_SIZE 72
#define NETWORK_AVATAR_SIZE 40
#define TLS_PORT 6697
#define HTTPS_PORT 443
#define LINK_SCHEME "crimson-chat"

/* Well known networks with broad IRCv3 support, from the ircv3.net network table, all reached over TLS */
static const struct {
	const char *name, *host, *about;
} KNOWN_NETWORKS[] = {
	{"Libera.Chat", "irc.libera.chat", TEXT_LIBERA_ABOUT},
	{"Ergo", "irc.ergo.chat", TEXT_ERGO_ABOUT},
	{"hackint", "irc.hackint.org", TEXT_HACKINT_ABOUT},
	{"tilde.chat", "irc.tilde.chat", TEXT_TILDE_ABOUT},
	{"Snoonet", "irc.snoonet.org", TEXT_SNOONET_ABOUT},
};

/* The answers a guide has collected so far */
typedef struct {
	Window *window;
	AdwDialog *dialog;
	AdwNavigationView *pages;
	char *network_name;
	/* The server chosen from the suggestions or typed in */
	char *server_host;
	int server_port;
	gboolean uses_tls;
	GtkWidget *host, *port, *tls, *nick, *account_row, *account, *password, *username;
	/* The bouncer choices of the last server step, after the choice of this computer alone */
	GPtrArray *bouncers, *choices;
	GtkWidget *finish_button;
} Guide;


static void free_guide(Guide *guide) {
	g_free(guide->network_name);
	g_free(guide->server_host);
	g_ptr_array_unref(guide->bouncers);
	g_ptr_array_unref(guide->choices);
	g_free(guide);
}


static Guide *create_guide(Window *window, const char *title) {
	Guide *guide = g_new0(Guide, 1);
	guide->window = window;
	guide->bouncers = g_ptr_array_new();
	guide->choices = g_ptr_array_new();
	guide->dialog = adw_dialog_new();
	adw_dialog_set_title(guide->dialog, title);
	adw_dialog_set_content_width(guide->dialog, GUIDE_WIDTH);
	adw_dialog_set_content_height(guide->dialog, GUIDE_HEIGHT);
	guide->pages = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
	adw_dialog_set_child(guide->dialog, GTK_WIDGET(guide->pages));
	g_object_set_data_full(G_OBJECT(guide->dialog), "guide", guide, (GDestroyNotify)free_guide);
	return guide;
}


static const char *read_entry(GtkWidget *row) {
	const char *text = row ? gtk_editable_get_text(GTK_EDITABLE(row)) : NULL;
	return text && *text ? text : NULL;
}


static GtkWidget *create_label(const char *text, const char *css_class) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(label, css_class);
	return label;
}


/* One step of a guide: an icon, a heading, a short explanation, the questions and the button onwards */
static AdwNavigationPage *create_step(const char *title, const char *description, const char *icon,
	GtkWidget *content, GtkWidget *button) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "guide-step");
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
	gtk_widget_add_css_class(image, "guide-icon");
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), create_label(title, "title-1"));
	if (description)
		gtk_box_append(GTK_BOX(box), create_label(description, "guide-description"));
	if (content) {
		gtk_widget_set_margin_top(content, 12);
		gtk_box_append(GTK_BOX(box), content);
	}
	if (button) {
		gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
		gtk_widget_set_margin_top(button, 12);
		gtk_box_append(GTK_BOX(box), button);
	}
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), CONTENT_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
	GtkWidget *toolbar = adw_toolbar_view_new();
	/* The heading in the page already names the step, so the header bar only carries Back and Close */
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroller);
	return adw_navigation_page_new(toolbar, title);
}


static GtkWidget *create_button(const char *label, GCallback clicked, Guide *guide) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	g_signal_connect_swapped(button, "clicked", clicked, guide);
	return button;
}


static GtkWidget *create_boxed_list(void) {
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	return list;
}


static GtkWidget *add_entry(GtkWidget *list, const char *title, const char *value, gboolean is_secret) {
	GtkWidget *row = is_secret ? adw_password_entry_row_new() : adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
}


static GtkWidget *add_port(GtkWidget *list, int port) {
	GtkWidget *row = adw_spin_row_new_with_range(1, G_MAXUINT16, 1);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_PORT);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), port);
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
}


/* A button onwards only works once the rows it needs hold something */
static void require_rows(GtkWidget *button, GtkWidget *first, GtkWidget *second) {
	gtk_widget_set_sensitive(button, read_entry(first) && (!second || read_entry(second)));
}


static void check_required(GtkEditable *editable, GtkWidget *button) {
	GtkWidget *first = g_object_get_data(G_OBJECT(button), "first");
	GtkWidget *second = g_object_get_data(G_OBJECT(button), "second");
	require_rows(button, first, second);
}


static void watch_required(GtkWidget *button, GtkWidget *first, GtkWidget *second) {
	g_object_set_data(G_OBJECT(button), "first", first);
	g_object_set_data(G_OBJECT(button), "second", second);
	g_signal_connect(first, "changed", G_CALLBACK(check_required), button);
	if (second)
		g_signal_connect(second, "changed", G_CALLBACK(check_required), button);
	require_rows(button, first, second);
}


static void collect_bouncers(Guide *guide) {
	Chat *chat = guide->window->chat;
	for (guint index = 0; index < chat->sessions->len; index++) {
		Session *session = g_ptr_array_index(chat->sessions, index);
		if (session->record->is_bouncer)
			g_ptr_array_add(guide->bouncers, session);
	}
}


static Session *get_chosen_bouncer(Guide *guide) {
	for (guint index = 0; index < guide->choices->len; index++) {
		if (gtk_check_button_get_active(g_ptr_array_index(guide->choices, index)))
			return index < guide->bouncers->len ? g_ptr_array_index(guide->bouncers, index) : NULL;
	}
	return NULL;
}


static void put_attribute(GHashTable *attributes, const char *name, const char *value) {
	if (value)
		g_hash_table_insert(attributes, g_strdup(name), g_strdup(value));
}


/* The last answer either adds the network to a bouncer or saves a direct account */
static void finish_server(Guide *guide) {
	Session *bouncer = get_chosen_bouncer(guide);
	const char *host = guide->server_host;
	int port = guide->server_port;
	gboolean uses_tls = guide->uses_tls;
	gboolean has_account = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(guide->account_row));
	const char *account = has_account ? read_entry(guide->account) : NULL;
	const char *password = has_account ? read_entry(guide->password) : NULL;
	if (bouncer) {
		g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_autofree char *port_text = g_strdup_printf("%d", port);
		put_attribute(attributes, "name", guide->network_name);
		put_attribute(attributes, "host", host);
		put_attribute(attributes, "port", port_text);
		put_attribute(attributes, "tls", uses_tls ? "1" : "0");
		put_attribute(attributes, "nickname", read_entry(guide->nick));
		put_attribute(attributes, "sasl-username", account);
		put_attribute(attributes, "sasl-password", password);
		add_bouncer_network(bouncer, attributes);
	} else {
		AccountRecord *record = g_new0(AccountRecord, 1);
		record->id = create_account_id();
		record->name = g_strdup(guide->network_name);
		record->host = g_strdup(host);
		record->port = port;
		record->uses_tls = uses_tls;
		record->nick = g_strdup(read_entry(guide->nick));
		record->sasl_username = g_strdup(account);
		record->sasl_password = g_strdup(password);
		add_account(guide->window->chat, record);
		save_chat_accounts(guide->window->chat);
	}
	adw_dialog_close(guide->dialog);
}


static void push_connection_step(Guide *guide) {
	GtkWidget *list = create_boxed_list();
	GtkWidget *first = NULL;
	for (guint index = 0; index <= guide->bouncers->len; index++) {
		Session *session = index < guide->bouncers->len ? g_ptr_array_index(guide->bouncers, index) : NULL;
		GtkWidget *row = adw_action_row_new();
		g_autofree char *title = session ? g_strdup_printf(TEXT_THROUGH_BOUNCER, session->record->name
			? session->record->name : session->record->host) : g_strdup(TEXT_THIS_COMPUTER);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), session ? TEXT_THROUGH_BOUNCER_DETAIL
			: NULL);
		GtkWidget *check = gtk_check_button_new();
		if (first)
			gtk_check_button_set_group(GTK_CHECK_BUTTON(check), GTK_CHECK_BUTTON(first));
		else
			first = check;
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), index == 0);
		adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
		adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
		g_ptr_array_add(guide->choices, check);
		gtk_list_box_append(GTK_LIST_BOX(list), row);
	}
	GtkWidget *button = create_button(TEXT_CONNECT, G_CALLBACK(finish_server), guide);
	adw_navigation_view_push(guide->pages, create_step(TEXT_STAY_CONNECTED_TITLE, NULL,
		"crimson-chats-symbolic", list, button));
}


static void continue_from_nick(Guide *guide) {
	if (guide->bouncers->len)
		push_connection_step(guide);
	else
		finish_server(guide);
}


/* Nicks start with a capital, so a lowercase login name from the computer does not become one */
static char *capitalize_nick(const char *nick) {
	if (!nick || !*nick)
		return g_strdup("");
	gunichar first = g_utf8_get_char(nick);
	char upper[8] = {0};
	g_unichar_to_utf8(g_unichar_toupper(first), upper);
	return g_strconcat(upper, g_utf8_next_char(nick), NULL);
}


static void capitalize_typed_nick(GtkEditable *editable, gpointer data) {
	const char *text = gtk_editable_get_text(editable);
	if (!*text || !g_unichar_islower(g_utf8_get_char(text)))
		return;
	g_autofree char *capitalized = capitalize_nick(text);
	int position = gtk_editable_get_position(editable);
	gtk_editable_set_text(editable, capitalized);
	gtk_editable_set_position(editable, position);
}


void capitalize_nick_field(GtkWidget *row) {
	g_signal_connect(row, "changed", G_CALLBACK(capitalize_typed_nick), NULL);
}


static void push_nick_step(Guide *guide) {
	GtkWidget *list = create_boxed_list();
	g_autofree char *nick = capitalize_nick(g_get_user_name());
	guide->nick = add_entry(list, TEXT_NICKNAME, nick, FALSE);
	capitalize_nick_field(guide->nick);
	guide->account_row = adw_expander_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(guide->account_row), TEXT_HAVE_ACCOUNT);
	adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(guide->account_row), NULL);
	adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(guide->account_row), TRUE);
	adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(guide->account_row), FALSE);
	guide->account = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(guide->account), TEXT_ACCOUNT);
	guide->password = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(guide->password), TEXT_PASSWORD);
	adw_expander_row_add_row(ADW_EXPANDER_ROW(guide->account_row), guide->account);
	adw_expander_row_add_row(ADW_EXPANDER_ROW(guide->account_row), guide->password);
	gtk_list_box_append(GTK_LIST_BOX(list), guide->account_row);
	GtkWidget *button = create_button(guide->bouncers->len ? TEXT_CONTINUE : TEXT_CONNECT,
		G_CALLBACK(continue_from_nick), guide);
	watch_required(button, guide->nick, NULL);
	adw_navigation_view_push(guide->pages, create_step(TEXT_NICK_TITLE, NULL, "avatar-default-symbolic", list,
		button));
}


static void choose_server(Guide *guide, const char *name, const char *host, int port, gboolean uses_tls) {
	g_free(guide->network_name);
	guide->network_name = g_strdup(name);
	g_free(guide->server_host);
	guide->server_host = g_strdup(host);
	guide->server_port = port;
	guide->uses_tls = uses_tls;
	push_nick_step(guide);
}


static void continue_from_address(Guide *guide) {
	const char *host = read_entry(guide->host);
	choose_server(guide, host, host, (int)adw_spin_row_get_value(ADW_SPIN_ROW(guide->port)),
		adw_switch_row_get_active(ADW_SWITCH_ROW(guide->tls)));
}


static void push_address_step(Guide *guide) {
	GtkWidget *list = create_boxed_list();
	guide->host = add_entry(list, TEXT_ADDRESS, NULL, FALSE);
	guide->port = add_port(list, TLS_PORT);
	guide->tls = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(guide->tls), TEXT_USE_TLS);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(guide->tls), NULL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(guide->tls), TRUE);
	gtk_list_box_append(GTK_LIST_BOX(list), guide->tls);
	GtkWidget *button = create_button(TEXT_CONTINUE, G_CALLBACK(continue_from_address), guide);
	watch_required(button, guide->host, NULL);
	adw_navigation_view_push(guide->pages, create_step(TEXT_ADDRESS_TITLE, NULL,
		"network-server-symbolic", list, button));
}


static void pick_network(GtkListBox *list, GtkListBoxRow *row, Guide *guide) {
	int index = gtk_list_box_row_get_index(row);
	if (index < 0 || index >= (int)G_N_ELEMENTS(KNOWN_NETWORKS)) {
		push_address_step(guide);
		return;
	}
	choose_server(guide, KNOWN_NETWORKS[index].name, KNOWN_NETWORKS[index].host, TLS_PORT, TRUE);
}


static GtkWidget *create_network_row(const char *name, const char *about, gboolean is_custom) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), about);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	GtkWidget *avatar = is_custom ? gtk_image_new_from_icon_name("list-add-symbolic")
		: adw_avatar_new(NETWORK_AVATAR_SIZE, name, TRUE);
	if (is_custom)
		gtk_widget_set_size_request(avatar, NETWORK_AVATAR_SIZE, NETWORK_AVATAR_SIZE);
	adw_action_row_add_prefix(ADW_ACTION_ROW(row), avatar);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
	return row;
}


void show_server_guide(Window *window) {
	Guide *guide = create_guide(window, TEXT_ADD_SERVER_TITLE);
	collect_bouncers(guide);
	GtkWidget *list = create_boxed_list();
	for (guint index = 0; index < G_N_ELEMENTS(KNOWN_NETWORKS); index++)
		gtk_list_box_append(GTK_LIST_BOX(list), create_network_row(KNOWN_NETWORKS[index].name,
			KNOWN_NETWORKS[index].about, FALSE));
	gtk_list_box_append(GTK_LIST_BOX(list), create_network_row(TEXT_OTHER_SERVER, NULL, TRUE));
	g_signal_connect(list, "row-activated", G_CALLBACK(pick_network), guide);
	adw_navigation_view_push(guide->pages, create_step(TEXT_PICK_NETWORK_TITLE, NULL,
		"crimson-chats-symbolic", list, NULL));
	adw_dialog_present(guide->dialog, GTK_WIDGET(window->window));
}


static void finish_bouncer(Guide *guide) {
	AccountRecord *record = g_new0(AccountRecord, 1);
	record->id = create_account_id();
	record->is_bouncer = TRUE;
	record->uses_tls = TRUE;
	record->name = g_strdup(guide->network_name ? guide->network_name : read_entry(guide->host));
	record->host = g_strdup(read_entry(guide->host));
	record->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(guide->port));
	record->nick = g_strdup(read_entry(guide->username));
	record->sasl_username = g_strdup(read_entry(guide->username));
	record->sasl_password = g_strdup(read_entry(guide->password));
	add_account(guide->window->chat, record);
	save_chat_accounts(guide->window->chat);
	adw_dialog_close(guide->dialog);
}


static AdwNavigationPage *create_login_step(Guide *guide, const char *username) {
	GtkWidget *list = create_boxed_list();
	guide->username = add_entry(list, TEXT_USERNAME, username, FALSE);
	guide->password = add_entry(list, TEXT_PASSWORD, NULL, TRUE);
	GtkWidget *button = create_button(TEXT_CONNECT, G_CALLBACK(finish_bouncer), guide);
	watch_required(button, guide->username, guide->password);
	return create_step(TEXT_LOGIN_TITLE, NULL, "dialog-password-symbolic", list, button);
}


static void continue_to_login(Guide *guide) {
	adw_navigation_view_push(guide->pages, create_login_step(guide, NULL));
}


static AdwNavigationPage *create_bouncer_address_step(Guide *guide, const BouncerHint *hint) {
	GtkWidget *list = create_boxed_list();
	guide->host = add_entry(list, TEXT_ADDRESS, hint ? hint->host : NULL, FALSE);
	guide->port = add_port(list, hint && hint->port ? hint->port : TLS_PORT);
	GtkWidget *button = create_button(TEXT_CONTINUE, G_CALLBACK(continue_to_login), guide);
	watch_required(button, guide->host, NULL);
	return create_step(TEXT_BOUNCER_ADDRESS_TITLE, NULL, "network-server-symbolic", list,
		button);
}


static void continue_to_address(Guide *guide) {
	adw_navigation_view_push(guide->pages, create_bouncer_address_step(guide, NULL));
}


void show_bouncer_guide(Window *window, const BouncerHint *hint) {
	Guide *guide = create_guide(window, TEXT_ADD_BOUNCER_TITLE);
	guide->network_name = g_strdup(hint ? hint->name : NULL);
	GtkWidget *button = create_button(TEXT_CONTINUE, G_CALLBACK(continue_to_address), guide);
	AdwNavigationPage *intro = create_step(TEXT_BOUNCER_INTRO_TITLE, TEXT_BOUNCER_INTRO_BODY, "crimson-chats-symbolic",
		NULL, button);
	if (!hint) {
		adw_navigation_view_push(guide->pages, intro);
	} else {
		/* An invite link already gave the address, so only the password is left to enter */
		AdwNavigationPage *steps[] = {intro, create_bouncer_address_step(guide, hint),
			create_login_step(guide, hint->username)};
		adw_navigation_view_replace(guide->pages, steps, G_N_ELEMENTS(steps));
	}
	adw_dialog_present(guide->dialog, GTK_WIDGET(window->window));
	if (hint)
		gtk_widget_grab_focus(guide->password);
}


void open_application_link(Window *window, const char *link) {
	g_autoptr(GUri) uri = g_uri_parse(link, G_URI_FLAGS_NONE, NULL);
	if (!uri || g_strcmp0(g_uri_get_scheme(uri), LINK_SCHEME) != 0 || g_strcmp0(g_uri_get_host(uri), "bouncer") != 0)
		return;
	g_autoptr(GHashTable) query = g_uri_get_query(uri) ? g_uri_parse_params(g_uri_get_query(uri), -1, "&",
		G_URI_PARAMS_WWW_FORM, NULL) : NULL;
	if (!query)
		return;
	const char *port = g_hash_table_lookup(query, "port");
	BouncerHint hint = {
		.name = g_hash_table_lookup(query, "name"),
		.host = g_hash_table_lookup(query, "host"),
		.username = g_hash_table_lookup(query, "username"),
		.port = port ? (int)g_ascii_strtoll(port, NULL, 10) : TLS_PORT,
	};
	if (hint.host)
		show_bouncer_guide(window, &hint);
}


static void create_invite(AdwAlertDialog *alert, const char *response, gpointer data) {
	Session *session = data;
	if (g_strcmp0(response, "create") != 0 || !session->control)
		return;
	static const char *const USES[] = {"1", "5", "25", "*"};
	guint chosen = gtk_drop_down_get_selected(g_object_get_data(G_OBJECT(alert), "uses"));
	send_irc_client_command(session->control, "CRIMSON", "INVITE", "CREATE", USES[MIN(chosen, 3)], NULL);
}


void show_invite_dialog(Window *window, CrimsonNetwork *network) {
	Session *session = network->session;
	if (!session->record->is_bouncer) {
		show_toast(window, TEXT_INVITES_NEED_BOUNCER);
		return;
	}
	AdwDialog *alert = adw_alert_dialog_new(TEXT_INVITE_PEOPLE, NULL);
	static const char *const CHOICES[] = {TEXT_INVITE_ONE, TEXT_INVITE_FIVE, TEXT_INVITE_TWENTY_FIVE,
		TEXT_INVITE_ANYONE, NULL};
	GtkWidget *uses = gtk_drop_down_new_from_strings(CHOICES);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), uses);
	g_object_set_data(G_OBJECT(alert), "uses", uses);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "create", TEXT_CREATE_LINK, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "create", ADW_RESPONSE_SUGGESTED);
	g_signal_connect(alert, "response", G_CALLBACK(create_invite), session);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void copy_link(AdwAlertDialog *alert, const char *response, gpointer data) {
	const char *link = g_object_get_data(G_OBJECT(alert), "link");
	Window *window = get_main_window();
	if (g_strcmp0(response, "copy") != 0 || !window)
		return;
	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(window->window)), link);
	show_toast(window, TEXT_LINK_COPIED);
}


void show_invite_link(Window *window, Session *session, const char *token, const char *uses) {
	const AccountRecord *record = session->record;
	g_autofree char *port = record->port == HTTPS_PORT ? g_strdup("") : g_strdup_printf(":%d", record->port);
	g_autofree char *link = g_strdup_printf("https://%s%s/invite?token=%s", record->host, port, token);
	AdwDialog *alert = adw_alert_dialog_new(TEXT_INVITE_READY,
		g_strcmp0(uses, "*") == 0 ? TEXT_INVITE_READY_ANYONE : TEXT_INVITE_READY_BODY);
	GtkWidget *entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry), link);
	gtk_editable_set_editable(GTK_EDITABLE(entry), FALSE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), entry);
	g_object_set_data_full(G_OBJECT(alert), "link", g_steal_pointer(&link), g_free);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "close", TEXT_CLOSE, "copy", TEXT_COPY_LINK, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "copy", ADW_RESPONSE_SUGGESTED);
	g_signal_connect(alert, "response", G_CALLBACK(copy_link), NULL);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}
