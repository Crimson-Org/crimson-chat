#include <string.h>

#include "../lang/en.h"
#include "dialogs.h"
#include "images.h"
#include "room-picker.h"
#include "wizards.h"

#define GUIDE_WIDTH 460
#define GUIDE_HEIGHT 680
#define CONTENT_WIDTH 400
#define ICON_SIZE 72
#define HELP_WIDTH_CHARS 34
#define NETWORK_AVATAR_SIZE 40
#define TLS_PORT 6697
#define HTTPS_PORT 443
#define LINK_SCHEME "crimson-chat"

/* Well known networks with broad IRCv3 support, from the ircv3.net network table, all reached over TLS */
static const struct {
	const char *name, *host, *about, *picture;
} KNOWN_NETWORKS[] = {
	{CRIMSON_SERVER_NAME, CRIMSON_SERVER_HOST, TEXT_CRIMSON_ABOUT, CRIMSON_SERVER_PICTURE},
	{"Libera.Chat", "irc.libera.chat", TEXT_LIBERA_ABOUT, NULL},
	{"Ergo", "irc.ergo.chat", TEXT_ERGO_ABOUT, NULL},
	{"hackint", "irc.hackint.org", TEXT_HACKINT_ABOUT, NULL},
	{"tilde.chat", "irc.tilde.chat", TEXT_TILDE_ABOUT, NULL},
	{"Snoonet", "irc.snoonet.org", TEXT_SNOONET_ABOUT, NULL},
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
	/* The bouncer that keeps the new server, when the user is connected to one */
	Session *bouncer;
	/* What the last step connects, watched until it is connected: a bouncer login, or a server and,
	 * on a bouncer, the networks it had before so the new one can be told apart */
	Session *connecting;
	CrimsonNetwork *network;
	GPtrArray *known_networks;
	GtkWidget *connecting_label;
	gulong watch_handler;
	GObject *watched;
} Guide;


/* The guide holds what it watches, so a server removed meanwhile is still there to let go of */
static void stop_watching(Guide *guide) {
	if (guide->watched && guide->watch_handler)
		g_signal_handler_disconnect(guide->watched, guide->watch_handler);
	g_clear_object(&guide->watched);
	guide->watch_handler = 0;
}


static void free_guide(Guide *guide) {
	stop_watching(guide);
	g_free(guide->network_name);
	g_free(guide->server_host);
	g_clear_pointer(&guide->known_networks, g_ptr_array_unref);
	g_free(guide);
}


static Guide *create_guide(Window *window, const char *title) {
	Guide *guide = g_new0(Guide, 1);
	guide->window = window;
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


GtkWidget *create_help_button(const char *text) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), HELP_WIDTH_CHARS);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "help-text");
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), label);
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "dialog-question-symbolic");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	gtk_widget_set_tooltip_text(button, TEXT_HELP);
	return button;
}


/* One step of a guide: an icon, a heading, a short explanation, the questions, the button onwards and the
 * help its corner button pops up */
static AdwNavigationPage *create_step(const char *title, const char *description, const char *icon,
	GtkWidget *content, GtkWidget *button, const char *help) {
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
	if (help)
		adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_help_button(help));
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


static void leave_connecting(Guide *guide);


/* Connected: a bouncer login is done, and a server goes on to its rooms */
static void finish_connecting(Guide *guide) {
	stop_watching(guide);
	if (guide->connecting) {
		adw_dialog_close(guide->dialog);
		return;
	}
	choose_network(guide->window, guide->network);
	AdwNavigationPage *picker = create_room_picker_page(guide->window, guide->network, guide->dialog);
	adw_navigation_view_replace(guide->pages, &picker, 1);
}


static void show_connecting_error(Guide *guide, const char *error) {
	gtk_label_set_text(GTK_LABEL(guide->connecting_label), error ? error : "");
	gtk_widget_set_visible(guide->connecting_label, error && *error);
}


static void follow_network(CrimsonNetwork *network, Guide *guide) {
	if (network->state == NETWORK_CONNECTED)
		finish_connecting(guide);
	else
		show_connecting_error(guide, network->error);
}


static void watch_network(Guide *guide, CrimsonNetwork *network) {
	stop_watching(guide);
	guide->network = network;
	choose_network(guide->window, network);
	guide->watched = g_object_ref(G_OBJECT(network));
	guide->watch_handler = g_signal_connect(network, "changed", G_CALLBACK(follow_network), guide);
	follow_network(network, guide);
}


/* A bouncer announces the new network a moment later, as the one it did not have before */
static void find_new_network(GListModel *networks, guint position, guint removed, guint added, Guide *guide) {
	for (guint index = 0; index < guide->bouncer->networks->len; index++) {
		CrimsonNetwork *network = g_ptr_array_index(guide->bouncer->networks, index);
		if (!g_ptr_array_find(guide->known_networks, network, NULL)) {
			watch_network(guide, network);
			return;
		}
	}
}


static void follow_login(CrimsonAccount *account, Guide *guide) {
	if (read_account_state(account->session) == NETWORK_CONNECTED)
		finish_connecting(guide);
	else
		show_connecting_error(guide, read_account_error(account->session));
}


/* A spinner while the connection is made, with Back undoing what the guide just added */
static void push_connecting_step(Guide *guide, const char *name) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(box), create_spinner());
	guide->connecting_label = create_label(NULL, "guide-error");
	gtk_widget_set_visible(guide->connecting_label, FALSE);
	gtk_box_append(GTK_BOX(box), guide->connecting_label);
	g_autofree char *title = g_strdup_printf(TEXT_CONNECTING_TO, name);
	AdwNavigationPage *page = create_step(title, NULL, "network-server-symbolic", box, NULL, TEXT_HELP_CONNECTING);
	adw_navigation_page_set_can_pop(page, FALSE);
	GtkWidget *back = gtk_button_new_with_label(TEXT_BACK);
	gtk_widget_add_css_class(back, "pill");
	gtk_widget_set_halign(back, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(back, 12);
	g_signal_connect_swapped(back, "clicked", G_CALLBACK(leave_connecting), guide);
	gtk_box_append(GTK_BOX(box), back);
	adw_navigation_view_push(guide->pages, page);
}


static void leave_connecting(Guide *guide) {
	stop_watching(guide);
	if (guide->connecting) {
		remove_account(guide->connecting);
		guide->connecting = NULL;
	} else if (guide->network && guide->network->bouncer_id) {
		delete_bouncer_network(guide->network);
	} else if (guide->network) {
		forget_network_in_window(guide->window, guide->network);
		remove_account(guide->network->session);
	}
	guide->network = NULL;
	AdwNavigationPage *page = adw_navigation_view_get_visible_page(guide->pages);
	adw_navigation_page_set_can_pop(page, TRUE);
	adw_navigation_view_pop(guide->pages);
}


static void put_attribute(GHashTable *attributes, const char *name, const char *value) {
	if (value)
		g_hash_table_insert(attributes, g_strdup(name), g_strdup(value));
}


/* The last answer adds the server to the bouncer when there is one, or connects to it from here */
static void finish_server(Guide *guide) {
	const char *host = guide->server_host;
	int port = guide->server_port;
	gboolean has_account = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(guide->account_row));
	const char *account = has_account ? read_entry(guide->account) : NULL;
	const char *password = has_account ? read_entry(guide->password) : NULL;
	push_connecting_step(guide, guide->network_name);
	if (guide->bouncer) {
		g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_autofree char *port_text = g_strdup_printf("%d", port);
		put_attribute(attributes, "name", guide->network_name);
		put_attribute(attributes, "host", host);
		put_attribute(attributes, "port", port_text);
		put_attribute(attributes, "tls", guide->uses_tls ? "1" : "0");
		put_attribute(attributes, "nickname", read_entry(guide->nick));
		put_attribute(attributes, "sasl-username", account);
		put_attribute(attributes, "sasl-password", password);
		g_clear_pointer(&guide->known_networks, g_ptr_array_unref);
		guide->known_networks = g_ptr_array_copy(guide->bouncer->networks, NULL, NULL);
		guide->watched = g_object_ref(G_OBJECT(guide->window->chat->networks));
		guide->watch_handler = g_signal_connect(guide->window->chat->networks, "items-changed",
			G_CALLBACK(find_new_network), guide);
		add_bouncer_network(guide->bouncer, attributes);
		return;
	}
	AccountRecord *record = g_new0(AccountRecord, 1);
	record->id = create_account_id();
	record->name = g_strdup(guide->network_name);
	record->host = g_strdup(host);
	record->port = port;
	record->uses_tls = guide->uses_tls;
	record->nick = g_strdup(read_entry(guide->nick));
	record->sasl_username = g_strdup(account);
	record->sasl_password = g_strdup(password);
	Session *session = add_account(guide->window->chat, record);
	save_chat_accounts(guide->window->chat);
	watch_network(guide, g_ptr_array_index(session->networks, 0));
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
	GtkWidget *button = create_button(TEXT_CONNECT, G_CALLBACK(finish_server), guide);
	watch_required(button, guide->nick, NULL);
	adw_navigation_view_push(guide->pages, create_step(TEXT_NICK_TITLE, NULL, "avatar-default-symbolic", list,
		button, TEXT_HELP_NICK));
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
		"network-server-symbolic", list, button, TEXT_HELP_ADDRESS));
}


static void pick_network(GtkListBox *list, GtkListBoxRow *row, Guide *guide) {
	int index = gtk_list_box_row_get_index(row);
	if (index < 0 || index >= (int)G_N_ELEMENTS(KNOWN_NETWORKS)) {
		push_address_step(guide);
		return;
	}
	choose_server(guide, KNOWN_NETWORKS[index].name, KNOWN_NETWORKS[index].host, TLS_PORT, TRUE);
}


static GtkWidget *create_network_row(const char *name, const char *about, const char *picture, gboolean is_custom) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), about);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	GtkWidget *avatar = is_custom ? gtk_image_new_from_icon_name("list-add-symbolic")
		: adw_avatar_new(NETWORK_AVATAR_SIZE, name, TRUE);
	if (is_custom)
		gtk_widget_set_size_request(avatar, NETWORK_AVATAR_SIZE, NETWORK_AVATAR_SIZE);
	else if (picture)
		show_avatar_image(ADW_AVATAR(avatar), picture);
	adw_action_row_add_prefix(ADW_ACTION_ROW(row), avatar);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
	return row;
}


void show_server_guide(Window *window) {
	Guide *guide = create_guide(window, TEXT_ADD_SERVER_TITLE);
	guide->bouncer = find_bouncer(window->chat);
	GtkWidget *list = create_boxed_list();
	for (guint index = 0; index < G_N_ELEMENTS(KNOWN_NETWORKS); index++)
		gtk_list_box_append(GTK_LIST_BOX(list), create_network_row(KNOWN_NETWORKS[index].name,
			KNOWN_NETWORKS[index].about, KNOWN_NETWORKS[index].picture, FALSE));
	gtk_list_box_append(GTK_LIST_BOX(list), create_network_row(TEXT_OTHER_SERVER, NULL, NULL, TRUE));
	g_signal_connect(list, "row-activated", G_CALLBACK(pick_network), guide);
	adw_navigation_view_push(guide->pages, create_step(TEXT_PICK_NETWORK_TITLE, NULL,
		"crimson-chats-symbolic", list, NULL, guide->bouncer ? TEXT_HELP_PICK_ON_BOUNCER : TEXT_HELP_PICK));
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
	push_connecting_step(guide, record->name);
	guide->connecting = add_account(guide->window->chat, record);
	save_chat_accounts(guide->window->chat);
	guide->watched = g_object_ref(G_OBJECT(guide->connecting->item));
	guide->watch_handler = g_signal_connect(guide->connecting->item, "changed", G_CALLBACK(follow_login), guide);
}


static AdwNavigationPage *create_login_step(Guide *guide, const char *username) {
	GtkWidget *list = create_boxed_list();
	guide->username = add_entry(list, TEXT_USERNAME, username, FALSE);
	guide->password = add_entry(list, TEXT_PASSWORD, NULL, TRUE);
	GtkWidget *button = create_button(TEXT_CONNECT, G_CALLBACK(finish_bouncer), guide);
	watch_required(button, guide->username, guide->password);
	return create_step(TEXT_LOGIN_TITLE, NULL, "dialog-password-symbolic", list, button, TEXT_HELP_LOGIN);
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
		button, TEXT_HELP_BOUNCER_ADDRESS);
}


static void continue_to_address(Guide *guide) {
	adw_navigation_view_push(guide->pages, create_bouncer_address_step(guide, NULL));
}


void show_bouncer_guide(Window *window, const BouncerHint *hint) {
	Session *bouncer = find_bouncer(window->chat);
	if (bouncer) {
		g_autofree char *text = g_strdup_printf(TEXT_ONE_BOUNCER, describe_account_name(bouncer));
		show_toast(window, text);
		return;
	}
	Guide *guide = create_guide(window, TEXT_ADD_BOUNCER_TITLE);
	guide->network_name = g_strdup(hint ? hint->name : NULL);
	GtkWidget *button = create_button(TEXT_CONTINUE, G_CALLBACK(continue_to_address), guide);
	AdwNavigationPage *intro = create_step(TEXT_BOUNCER_INTRO_TITLE, TEXT_BOUNCER_INTRO_BODY, "crimson-chats-symbolic",
		NULL, button, TEXT_HELP_BOUNCER);
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


void show_invite_dialog(Window *window, Session *session) {
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


void show_invite_link(Window *window, Session *session, const char *token, const char *uses, const char *port) {
	gboolean is_default_port = g_ascii_strtoll(port, NULL, 10) == HTTPS_PORT;
	g_autofree char *link = g_strdup_printf("https://%s%s%s/invite?token=%s", session->record->host,
		is_default_port ? "" : ":", is_default_port ? "" : port, token);
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
