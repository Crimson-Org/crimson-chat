#include <string.h>

#include "../lang/en.h"
#include "dialogs.h"
#include "formatting.h"
#include "images.h"
#include "profile.h"
#include "room-look.h"
#include "room-settings.h"
#include "wizards.h"
#include "pictures.h"
#include "uploads.h"

#define PROFILE_WIDTH 400
/* A person's profile opens at its full height, so it does not grow when the details arrive */
#define PROFILE_HEIGHT 620
#define TOPIC_LINES 6
#define PROFILE_AVATAR_SIZE 96
/* How many letters the pronouns and status lines under the user's own name make room for */
#define PRONOUNS_WIDTH 16
#define STATUS_WIDTH 28
#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400

typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *stack, *rows, *realname;
	/* The name shown large, the nick and pronouns under it, the status the person set and whether it is a bot */
	GtkWidget *name, *identity, *status, *bot;
	/* The services account the person is logged into, small under the real name */
	GtkWidget *account;
	char *nick;
} ProfileView;


static void free_profile_view(ProfileView *view) {
	if (view->network)
		cancel_profile(view->network, view);
	g_free(view->nick);
	g_free(view);
}


char *describe_idle(gint64 seconds) {
	if (seconds < SECONDS_PER_MINUTE)
		return g_strdup(TEXT_IDLE_NOW);
	if (seconds < SECONDS_PER_HOUR)
		return g_strdup_printf(TEXT_IDLE_MINUTES, (int)(seconds / SECONDS_PER_MINUTE));
	if (seconds < SECONDS_PER_DAY)
		return g_strdup_printf(TEXT_IDLE_HOURS, (int)(seconds / SECONDS_PER_HOUR));
	return g_strdup_printf(TEXT_IDLE_DAYS, (int)(seconds / SECONDS_PER_DAY));
}


/* WHOIS marks the rooms where someone is an operator or has voice, which the room names do not need */
static char *list_rooms(const char *channels) {
	if (!channels)
		return NULL;
	g_auto(GStrv) names = g_strsplit(channels, " ", -1);
	g_autoptr(GString) list = g_string_new(NULL);
	for (int index = 0; names[index]; index++) {
		const char *name = names[index] + strspn(names[index], "~&@%+");
		if (*name)
			g_string_append_printf(list, "%s%s", list->len ? ", " : "", name);
	}
	return g_string_free(g_steal_pointer(&list), FALSE);
}


static void add_row(ProfileView *view, const char *title, const char *value) {
	if (!value || !*value)
		return;
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
	/* A value stays on one line however long it grows, such as someone in hundreds of rooms, and shows whole when
	 * pointed at */
	adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
	gtk_widget_set_tooltip_text(row, value);
	gtk_widget_add_css_class(row, "property");
	gtk_list_box_append(GTK_LIST_BOX(view->rows), row);
}


/* A homepage opens in the browser from its row */
static void open_homepage(GtkButton *button, ProfileView *view) {
	confirm_open_link(GTK_WIDGET(button), g_object_get_data(G_OBJECT(button), "url"));
}


static void add_homepage_row(ProfileView *view, const char *url) {
	if (!url || (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://")))
		return;
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_HOMEPAGE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), url);
	gtk_widget_add_css_class(row, "property");
	GtkWidget *button = gtk_button_new_from_icon_name("adw-external-link-symbolic");
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(button, TEXT_OPEN_HOMEPAGE);
	g_object_set_data_full(G_OBJECT(button), "url", g_strdup(url), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(open_homepage), view);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
	adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), button);
	gtk_list_box_append(GTK_LIST_BOX(view->rows), row);
}


/* What the person set for themselves through the metadata registry: a display name shown large with the nick
 * under it, pronouns beside the nick, a status line and whether it is a bot */
static void fill_person_metadata(ProfileView *view) {
	CrimsonRoom *server = view->network->server_room;
	const char *display_name = find_metadata(server, view->nick, "display-name");
	const char *pronouns = find_metadata(server, view->nick, "pronouns");
	const char *status = find_metadata(server, view->nick, "status");
	const char *bot = find_metadata(server, view->nick, "bot");
	gboolean has_display = display_name && *display_name && g_strcmp0(display_name, view->nick) != 0;
	gtk_label_set_text(GTK_LABEL(view->name), has_display ? display_name : view->nick);
	g_autoptr(GString) identity = g_string_new(has_display ? view->nick : "");
	if (pronouns && *pronouns)
		g_string_append_printf(identity, "%s%s", identity->len ? " · " : "", pronouns);
	gtk_label_set_text(GTK_LABEL(view->identity), identity->str);
	gtk_widget_set_visible(view->identity, identity->len > 0);
	gtk_label_set_text(GTK_LABEL(view->status), status ? status : "");
	gtk_widget_set_visible(view->status, status && *status);
	IrcState *state = get_network_state(view->network);
	IrcUser *user = state ? find_irc_user(state, view->nick) : NULL;
	gtk_widget_set_visible(view->bot, (bot && *bot) || (user && user->is_bot));
}


static GtkWidget *create_account_line(void) {
	GtkWidget *line = gtk_label_new(NULL);
	gtk_widget_add_css_class(line, "dim-label");
	gtk_widget_add_css_class(line, "caption");
	gtk_widget_set_visible(line, FALSE);
	return line;
}


static void show_account_line(GtkWidget *line, const char *account) {
	g_autofree char *text = account && *account ? g_strdup_printf(TEXT_LOGGED_IN_AS, account) : NULL;
	gtk_label_set_text(GTK_LABEL(line), text ? text : "");
	gtk_widget_set_visible(line, text != NULL);
}


static void show_details(Profile *profile, gpointer data) {
	ProfileView *view = data;
	if (!profile) {
		view->network = NULL;
		adw_dialog_close(view->dialog);
		return;
	}
	if (!profile->is_found) {
		gtk_label_set_text(GTK_LABEL(view->realname), TEXT_PROFILE_OFFLINE);
		gtk_stack_set_visible_child_name(GTK_STACK(view->stack), "details");
		return;
	}
	/* A real name that only repeats the nick says nothing */
	gboolean has_name = profile->realname && g_ascii_strcasecmp(profile->realname, profile->nick) != 0;
	gtk_label_set_text(GTK_LABEL(view->realname), has_name ? profile->realname : "");
	gtk_widget_set_visible(view->realname, has_name);
	show_account_line(view->account, profile->account);
	add_row(view, TEXT_PROFILE_AWAY, profile->away);
	g_autofree char *idle = profile->idle_seconds > 0 || profile->server ? describe_idle(profile->idle_seconds) : NULL;
	add_row(view, TEXT_PROFILE_IDLE, idle);
	g_autofree char *rooms = list_rooms(profile->channels);
	add_row(view, TEXT_PROFILE_ROOMS, rooms);
	add_homepage_row(view, find_metadata(view->network->server_room, view->nick, "homepage"));
	fill_person_metadata(view);
	gtk_widget_set_visible(view->rows, gtk_widget_get_first_child(view->rows) != NULL);
	gtk_stack_set_visible_child_name(GTK_STACK(view->stack), "details");
}


static void send_message(GtkButton *button, ProfileView *view) {
	if (!view->network)
		return;
	CrimsonRoom *room = open_private_room(view->network, view->nick);
	open_room(view->window, room);
	adw_dialog_close(view->dialog);
}


void show_profile(Window *window, CrimsonRoom *room, const char *nick) {
	CrimsonNetwork *network = room->network;
	ProfileView *view = g_new0(ProfileView, 1);
	view->window = window;
	view->network = network;
	view->nick = g_strdup(nick);
	view->dialog = adw_dialog_new();
	adw_dialog_set_content_width(view->dialog, PROFILE_WIDTH);
	adw_dialog_set_content_height(view->dialog, PROFILE_HEIGHT);
	g_object_set_data_full(G_OBJECT(view->dialog), "view", view, (GDestroyNotify)free_profile_view);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "profile");
	GtkWidget *avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, nick, TRUE);
	const char *url = pick_person_picture(room, nick);
	g_autofree char *sized = fill_image_size(url, PROFILE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(avatar), sized);
	GtkWidget *choice = offer_picture_choice(avatar, window, network, PICTURE_PERSON, nick, FALSE);
	gtk_widget_set_halign(choice, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), choice);
	view->name = gtk_label_new(nick);
	gtk_label_set_wrap(GTK_LABEL(view->name), TRUE);
	gtk_label_set_justify(GTK_LABEL(view->name), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(view->name, "title-1");
	gtk_box_append(GTK_BOX(box), view->name);
	view->bot = gtk_label_new(TEXT_BOT);
	gtk_widget_add_css_class(view->bot, "bot-badge");
	gtk_widget_set_halign(view->bot, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), view->bot);
	view->identity = gtk_label_new(NULL);
	gtk_widget_add_css_class(view->identity, "dim-label");
	gtk_box_append(GTK_BOX(box), view->identity);
	view->status = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(view->status), TRUE);
	gtk_label_set_justify(GTK_LABEL(view->status), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(view->status, "profile-status");
	gtk_box_append(GTK_BOX(box), view->status);
	view->realname = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(view->realname), TRUE);
	gtk_widget_add_css_class(view->realname, "dim-label");
	gtk_box_append(GTK_BOX(box), view->realname);
	view->account = create_account_line();
	gtk_box_append(GTK_BOX(box), view->account);
	GtkWidget *message = gtk_button_new_with_label(TEXT_SEND_MESSAGE);
	gtk_widget_add_css_class(message, "pill");
	gtk_widget_add_css_class(message, "suggested-action");
	gtk_widget_set_halign(message, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(message, 8);
	g_signal_connect(message, "clicked", G_CALLBACK(send_message), view);
	IrcState *own_state = get_network_state(room->network);
	gtk_widget_set_visible(message, !own_state || !is_own_nick(own_state, nick));
	gtk_box_append(GTK_BOX(box), message);
	view->stack = gtk_stack_new();
	gtk_stack_set_vhomogeneous(GTK_STACK(view->stack), FALSE);
	gtk_stack_add_named(GTK_STACK(view->stack), create_spinner(), "loading");
	view->rows = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(view->rows), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(view->rows, "boxed-list");
	gtk_widget_set_margin_top(view->rows, 12);
	gtk_stack_add_named(GTK_STACK(view->stack), view->rows, "details");
	gtk_box_append(GTK_BOX(box), view->stack);
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroller);
	adw_dialog_set_child(view->dialog, toolbar);
	adw_dialog_present(view->dialog, GTK_WIDGET(window->window));
	fill_person_metadata(view);
	/* The server only tells profile keys of people outside the user's rooms when asked */
	if (network->client && can_network(network, "draft/metadata-2"))
		send_irc_client_command(network->client, "METADATA", nick, "GET", FOLLOWED_PROFILE_KEYS, NULL);
	if (network->state == NETWORK_CONNECTED)
		request_profile(network, nick, show_details, view);
	else
		gtk_stack_set_visible_child_name(GTK_STACK(view->stack), "details");
}


typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *avatar, *nick, *display_name, *realname, *account, *password;
	/* What the fields held when the dialog opened, so only changes are sent */
	char *old_nick, *old_display_name, *old_realname, *old_picture, *old_account, *old_password;
	/* The picture address given through the avatar, or a picture to upload, sent on saving */
	char *picture;
	GFile *upload;
	/* The registry's status, pronouns and homepage keys, and what they held when the dialog opened */
	GtkWidget *status, *pronouns, *homepage;
	char *old_status, *old_pronouns, *old_homepage;
	/* The password field holds a stand-in for one the bouncer keeps, and its button that shows the password */
	gboolean has_stand_in;
	GtkWidget *peek;
} OwnProfile;


/* Shows as dots for a password the bouncer keeps but never sends back, and is itself never sent */
#define PASSWORD_STAND_IN "saved-password"


static const char *read_password(OwnProfile *own) {
	const char *password = gtk_editable_get_text(GTK_EDITABLE(own->password));
	return own->has_stand_in && g_str_equal(password, PASSWORD_STAND_IN) ? "" : password;
}


/* Typing into the field starts from empty, since the stand-in is no password to edit */
static void clear_stand_in(OwnProfile *own) {
	if (own->has_stand_in && !*read_password(own))
		gtk_editable_set_text(GTK_EDITABLE(own->password), "");
}


/* The password row's own button that shows the password, which libadwaita adds as its one button suffix */
static GtkWidget *find_peek_button(GtkWidget *widget) {
	const char *icon = GTK_IS_BUTTON(widget) ? gtk_button_get_icon_name(GTK_BUTTON(widget)) : NULL;
	if (icon && g_str_has_prefix(icon, "view-"))
		return widget;
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
		GtkWidget *found = find_peek_button(child);
		if (found)
			return found;
	}
	return NULL;
}


/* A password the app never learned cannot be shown, so the button only works on one typed here */
static void update_peek(OwnProfile *own) {
	gboolean is_stand_in = own->has_stand_in
		&& g_str_equal(gtk_editable_get_text(GTK_EDITABLE(own->password)), PASSWORD_STAND_IN);
	if (own->peek)
		gtk_widget_set_sensitive(own->peek, !is_stand_in);
}


/* Leaving the field empty keeps the saved password, so the dots come back */
static void restore_stand_in(OwnProfile *own) {
	if (own->has_stand_in && !*gtk_editable_get_text(GTK_EDITABLE(own->password)))
		gtk_editable_set_text(GTK_EDITABLE(own->password), PASSWORD_STAND_IN);
}


static void free_own_profile(OwnProfile *own) {
	g_free(own->old_nick);
	g_free(own->old_display_name);
	g_free(own->old_realname);
	g_free(own->old_picture);
	g_free(own->picture);
	g_clear_object(&own->upload);
	g_free(own->old_status);
	g_free(own->old_pronouns);
	g_free(own->old_homepage);
	g_free(own->old_account);
	g_free(own->old_password);
	g_free(own);
}


static const char *read_field(GtkWidget *row) {
	return row ? gtk_editable_get_text(GTK_EDITABLE(row)) : "";
}


/* A picture sent on saving finishes after the dialog closed, so it keeps only what the upload needs */
typedef struct {
	Window *window;
	CrimsonNetwork *network;
} PictureUpload;


static void finish_picture_upload(const char *url, const char *problem, gpointer data) {
	PictureUpload *upload = data;
	/* The address is kept like a typed one, so the app sets it again on each connection */
	if (url)
		save_profile_value(upload->network->key, "avatar", url);
	if (problem)
		show_toast(upload->window, problem);
	g_object_unref(upload->network);
	g_free(upload);
}


static void send_picture_upload(CrimsonNetwork *network, Window *window, GFile *file) {
	PictureUpload *upload = g_new0(PictureUpload, 1);
	*upload = (PictureUpload){window, g_object_ref(network)};
	upload_avatar(network, "*", file, finish_picture_upload, upload);
}


static gboolean has_changed(GtkWidget *row, const char *old) {
	return row && g_strcmp0(read_field(row), old ? old : "") != 0;
}


static void set_metadata(CrimsonNetwork *network, const char *key, const char *value) {
	if (*value)
		send_irc_client_command(network->client, "METADATA", "*", "SET", key, value, NULL);
	else
		send_irc_client_command(network->client, "METADATA", "*", "SET", key, NULL);
}


/* The big name shows the nickname while no display name is set, so showing a nickname there means having none */
static const char *read_display_name(OwnProfile *own) {
	const char *shown = read_field(own->display_name);
	gboolean is_nick = g_strcmp0(shown, own->old_nick) == 0 || g_strcmp0(shown, read_field(own->nick)) == 0;
	return is_nick ? "" : shown;
}


static void finish_reconnect_question(AdwAlertDialog *alert, const char *response, CrimsonNetwork *network) {
	if (g_strcmp0(response, "reconnect") == 0)
		reconnect_network(network);
}


static void ask_to_reconnect(Window *window, CrimsonNetwork *network) {
	AdwDialog *alert = adw_alert_dialog_new(TEXT_RECONNECT_NEEDED, TEXT_RECONNECT_NEEDED_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "later", TEXT_LATER, "reconnect", TEXT_RECONNECT, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "reconnect", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "reconnect");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "later");
	g_signal_connect_object(alert, "response", G_CALLBACK(finish_reconnect_question), network, G_CONNECT_DEFAULT);
	adw_dialog_present(alert, GTK_WIDGET(window->window));
}


static void save_own_profile(GtkButton *button, OwnProfile *own) {
	CrimsonNetwork *network = own->network;
	AccountRecord *record = network->session->record;
	gboolean is_direct = !network->bouncer_id;
	if (network->client && has_changed(own->nick, own->old_nick) && *read_field(own->nick)) {
		send_irc_client_command(network->client, "NICK", read_field(own->nick), NULL);
		if (is_direct) {
			g_free(record->nick);
			record->nick = g_strdup(read_field(own->nick));
		}
	}
	/* A server without SETNAME only takes a new real name when the app connects again */
	gboolean has_new_realname = has_changed(own->realname, own->old_realname);
	gboolean can_rename = can_network(network, "setname");
	if (has_new_realname && is_direct) {
		if (network->client && can_rename)
			send_irc_client_command(network->client, "SETNAME", read_field(own->realname), NULL);
		g_free(record->realname);
		record->realname = g_strdup(read_field(own->realname));
	}
	/* Each profile key is kept, so the app sets it again on every connection */
	const struct {
		GtkWidget *row;
		const char *value, *old, *key;
	} FIELDS[] = {
		{own->display_name, read_display_name(own), own->old_display_name, "display-name"},
		{own->status, read_field(own->status), own->old_status, "status"},
		{own->pronouns, read_field(own->pronouns), own->old_pronouns, "pronouns"},
		{own->homepage, read_field(own->homepage), own->old_homepage, "homepage"},
	};
	for (guint index = 0; index < G_N_ELEMENTS(FIELDS); index++) {
		if (!FIELDS[index].row)
			continue;
		const char *old = FIELDS[index].old ? FIELDS[index].old : "";
		if (network->client && g_strcmp0(FIELDS[index].value, old) != 0)
			set_metadata(network, FIELDS[index].key, FIELDS[index].value);
		save_profile_value(network->key, FIELDS[index].key, FIELDS[index].value);
	}
	/* An uploaded picture is set by the bouncer, so only a given address is sent from here */
	if (own->upload) {
		send_picture_upload(network, own->window, own->upload);
	} else if (g_strcmp0(own->picture, own->old_picture ? own->old_picture : "") != 0) {
		if (network->client)
			set_metadata(network, "avatar", own->picture);
		save_profile_value(network->key, "avatar", own->picture);
	}
	/* A new login only counts on the next connection, so changing it reconnects */
	gboolean has_new_login = has_changed(own->account, own->old_account)
		|| g_strcmp0(read_password(own), own->old_password ? own->old_password : "") != 0;
	/* The bouncer keeps the real name for its own connections and passes it on to a network with SETNAME */
	if (!is_direct) {
		g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		if (has_new_login)
			g_hash_table_insert(attributes, g_strdup("sasl-username"), g_strdup(read_field(own->account)));
		if (has_new_login && *read_password(own))
			g_hash_table_insert(attributes, g_strdup("sasl-password"), g_strdup(read_password(own)));
		if (has_new_realname)
			g_hash_table_insert(attributes, g_strdup("realname"), g_strdup(read_field(own->realname)));
		if (g_hash_table_size(attributes))
			change_bouncer_network(network, attributes);
	} else if (has_new_login) {
		AccountRecord *changed = copy_account_record(record);
		g_free(changed->sasl_username);
		changed->sasl_username = *read_field(own->account) ? g_strdup(read_field(own->account)) : NULL;
		g_free(changed->sasl_password);
		changed->sasl_password = *read_password(own) ? g_strdup(read_password(own)) : NULL;
		change_account(network->session, changed);
		free_account_record(changed);
	} else if (is_direct) {
		save_chat_accounts(network->session->chat);
	}
	Window *window = own->window;
	adw_dialog_close(own->dialog);
	/* A new login reconnects anyway, which brings the real name along */
	if (has_new_realname && !can_rename && !has_new_login)
		ask_to_reconnect(window, network);
}


/* The server answers with the user's own details after the dialog opened, and they fill fields left untouched */
static void refresh_own_field(GtkWidget *row, char **old, const char *current) {
	if (!row || !current || g_strcmp0(current, *old) == 0 || has_changed(row, *old))
		return;
	g_free(*old);
	*old = g_strdup(current);
	gtk_editable_set_text(GTK_EDITABLE(row), current);
}


static void show_own_picture(OwnProfile *own) {
	g_autofree char *chosen = own->upload ? g_file_get_uri(own->upload) : NULL;
	const char *url = chosen ? chosen : *own->picture ? own->picture : NULL;
	g_autofree char *sized = fill_image_size(url, PROFILE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(own->avatar), sized);
	show_picture_removable(own->avatar, url != NULL);
}


/* The picture the server announces shows unless the user gave another address since the dialog opened */
static void refresh_own_picture(OwnProfile *own, const char *current) {
	if (!current || own->upload || g_strcmp0(current, own->old_picture) == 0
		|| g_strcmp0(own->picture, own->old_picture ? own->old_picture : "") != 0)
		return;
	g_free(own->old_picture);
	own->old_picture = g_strdup(current);
	g_free(own->picture);
	own->picture = g_strdup(current);
	show_own_picture(own);
}


static void refresh_display_name(OwnProfile *own, const char *current) {
	const char *old = own->old_display_name ? own->old_display_name : "";
	if (!own->display_name || !current || g_strcmp0(current, old) == 0 || g_strcmp0(read_display_name(own), old) != 0)
		return;
	g_free(own->old_display_name);
	own->old_display_name = g_strdup(current);
	gtk_editable_set_text(GTK_EDITABLE(own->display_name), *current ? current : read_field(own->nick));
}


static void refresh_own_profile(CrimsonNetwork *network, AdwDialog *dialog) {
	OwnProfile *own = g_object_get_data(G_OBJECT(dialog), "own");
	IrcState *state = get_network_state(network);
	if (!own || !state || !state->nick)
		return;
	refresh_display_name(own, find_metadata(network->server_room, state->nick, "display-name"));
	refresh_own_picture(own, find_metadata(network->server_room, state->nick, "avatar"));
	refresh_own_field(own->status, &own->old_status, find_metadata(network->server_room, state->nick, "status"));
	refresh_own_field(own->pronouns, &own->old_pronouns, find_metadata(network->server_room, state->nick,
		"pronouns"));
	refresh_own_field(own->homepage, &own->old_homepage, find_metadata(network->server_room, state->nick,
		"homepage"));
}


static void finish_picture_address(AdwAlertDialog *alert, const char *response, AdwDialog *dialog) {
	OwnProfile *own = g_object_get_data(G_OBJECT(dialog), "own");
	if (g_strcmp0(response, "use") != 0 || !own)
		return;
	GtkWidget *entry = adw_alert_dialog_get_extra_child(alert);
	g_free(own->picture);
	own->picture = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	g_clear_object(&own->upload);
	show_own_picture(own);
}


/* Servers keep only an address for a picture, so without a bouncer to send one to the user gives its address */
static void ask_picture_address(OwnProfile *own) {
	AdwDialog *alert = adw_alert_dialog_new(TEXT_PICTURE_ADDRESS, TEXT_PICTURE_ADDRESS_ABOUT);
	GtkWidget *entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry), own->picture);
	gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_URL);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), entry);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "use", TEXT_USE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "use", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "use");
	g_signal_connect_object(alert, "response", G_CALLBACK(finish_picture_address), own->dialog, G_CONNECT_DEFAULT);
	adw_dialog_present(alert, GTK_WIDGET(own->window->window));
	gtk_widget_grab_focus(entry);
}


/* Like every field of the profile, the removal is sent on saving */
static void finish_removing_own_picture(AdwAlertDialog *alert, const char *response, AdwDialog *dialog) {
	OwnProfile *own = g_object_get_data(G_OBJECT(dialog), "own");
	if (g_strcmp0(response, "remove") != 0 || !own)
		return;
	g_clear_object(&own->upload);
	g_free(own->picture);
	own->picture = g_strdup("");
	show_own_picture(own);
}


static void ask_to_remove_own_picture(OwnProfile *own) {
	AdwDialog *alert = adw_alert_dialog_new(TEXT_REMOVE_YOUR_PICTURE_TITLE, TEXT_REMOVE_YOUR_PICTURE_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", TEXT_CANCEL, "remove", TEXT_REMOVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
	g_signal_connect_object(alert, "response", G_CALLBACK(finish_removing_own_picture), own->dialog,
		G_CONNECT_DEFAULT);
	adw_dialog_present(alert, GTK_WIDGET(own->window->window));
}


static void finish_choosing_upload(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GObject) dialog = data;
	OwnProfile *own = g_object_get_data(dialog, "own");
	g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file || !own)
		return;
	g_set_object(&own->upload, file);
	show_own_picture(own);
}


/* The bouncer only reads these kinds of picture */
static void choose_upload(OwnProfile *own) {
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_CHOOSE_PICTURE_TITLE);
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_PICTURES);
	gtk_file_filter_add_mime_type(filter, "image/png");
	gtk_file_filter_add_mime_type(filter, "image/jpeg");
	gtk_file_filter_add_mime_type(filter, "image/webp");
	gtk_file_filter_add_mime_type(filter, "image/gif");
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_open(dialog, GTK_WINDOW(own->window->window), NULL, finish_choosing_upload,
		g_object_ref(own->dialog));
}


/* A bouncer that takes uploads gets a picture from the computer, and any other server an address */
static void change_own_picture(OwnProfile *own) {
	g_autofree char *upload_url = find_avatar_upload_url(own->network);
	if (upload_url)
		choose_upload(own);
	else
		ask_picture_address(own);
}


static void preview_name(GtkEditable *editable, OwnProfile *own) {
	adw_avatar_set_text(ADW_AVATAR(own->avatar), gtk_editable_get_text(editable));
	if (own->display_name && !*read_display_name(own))
		gtk_editable_set_text(GTK_EDITABLE(own->display_name), gtk_editable_get_text(editable));
}


static void start_editing_name(GtkEditableLabel *name) {
	gtk_editable_label_start_editing(name);
	gtk_widget_grab_focus(GTK_WIDGET(name));
}


/* The hidden text box asks for 150 pixels without a most letters set, which leaves a gap beside a short name, so the
 * shown name sets the width and the text box only asks for room while the user types */
static void fit_name_width(GtkEditable *name) {
	gboolean is_editing = gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(name));
	int letters = is_editing ? MAX(1, (int)g_utf8_strlen(gtk_editable_get_text(name), -1)) : 1;
	gtk_editable_set_width_chars(name, letters);
	gtk_editable_set_max_width_chars(name, letters);
}


/* The name chats show the user by, large, with a pen beside it while pointed at that edits it in place */
static GtkWidget *create_name_editor(GtkWidget **name, const char *text) {
	*name = gtk_editable_label_new(text);
	gtk_widget_add_css_class(*name, "title-1");
	/* Width in average letters seldom matches the name, so the text sits in the middle of the room it gets */
	gtk_editable_set_alignment(GTK_EDITABLE(*name), 0.5);
	fit_name_width(GTK_EDITABLE(*name));
	g_signal_connect(*name, "changed", G_CALLBACK(fit_name_width), NULL);
	g_signal_connect(*name, "notify::editing", G_CALLBACK(fit_name_width), NULL);
	GtkWidget *pen = gtk_button_new_from_icon_name("document-edit-symbolic");
	gtk_widget_add_css_class(pen, "flat");
	gtk_widget_add_css_class(pen, "circular");
	gtk_widget_add_css_class(pen, "name-pen");
	gtk_widget_set_valign(pen, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(pen, TEXT_CHANGE_NAME);
	g_signal_connect_swapped(pen, "clicked", G_CALLBACK(start_editing_name), *name);
	/* An empty space as wide as the pen on the other side keeps the name itself in the middle */
	GtkWidget *balance = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkSizeGroup *widths = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget(widths, balance);
	gtk_size_group_add_widget(widths, pen);
	g_object_unref(widths);
	GtkWidget *editor = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(editor, "name-editor");
	gtk_widget_set_halign(editor, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(editor), balance);
	gtk_box_append(GTK_BOX(editor), *name);
	gtk_box_append(GTK_BOX(editor), pen);
	return editor;
}


/* A line under the name that is typed into in place, showing what to write while it is empty */
static GtkWidget *create_line_field(const char *value, const char *placeholder, const char *look, int width) {
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_alignment(GTK_ENTRY(entry), 0.5);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
	gtk_editable_set_text(GTK_EDITABLE(entry), value ? value : "");
	gtk_editable_set_width_chars(GTK_EDITABLE(entry), width);
	gtk_widget_add_css_class(entry, look);
	gtk_widget_set_halign(entry, GTK_ALIGN_CENTER);
	return entry;
}


/* The status bubble shows what the field holds, and its clear button only shows while there is something to clear */
static void show_status(GtkEditable *field) {
	const char *text = gtk_editable_get_text(field);
	gboolean is_set = *text != '\0';
	GtkWidget *label = g_object_get_data(G_OBJECT(field), "status-label");
	gtk_label_set_text(GTK_LABEL(label), is_set ? text : TEXT_SET_STATUS);
	if (is_set)
		gtk_widget_remove_css_class(label, "dim-label");
	else
		gtk_widget_add_css_class(label, "dim-label");
	gtk_widget_set_visible(g_object_get_data(G_OBJECT(field), "status-clear"), is_set);
}


static void clear_status(GtkEditable *field) {
	gtk_editable_set_text(field, "");
}


static void pick_status(GtkButton *button, GtkEditable *field) {
	gtk_editable_set_text(field, gtk_button_get_label(button));
	gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(field), "status-menu")));
}


static void close_status_menu(GtkEditable *field) {
	gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(field), "status-menu")));
}


/* A bubble under the name that opens a menu to write a status with emoji or pick a ready one, and clears with one
 * click; the field it returns holds the status for saving */
static GtkWidget *create_status_field(GtkWidget **field, const char *value) {
	static const char *const PRESETS[] = {TEXT_STATUS_MEETING, TEXT_STATUS_WORKING, TEXT_STATUS_LUNCH, TEXT_STATUS_GAMING,
		TEXT_STATUS_SICK, TEXT_STATUS_HOLIDAY};
	*field = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(*field), TEXT_STATUS_PROMPT);
	g_object_set(*field, "show-emoji-icon", TRUE, NULL);
	gtk_editable_set_text(GTK_EDITABLE(*field), value ? value : "");
	gtk_editable_set_width_chars(GTK_EDITABLE(*field), STATUS_WIDTH);
	g_signal_connect(*field, "activate", G_CALLBACK(close_status_menu), NULL);
	GtkWidget *menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(menu, "status-menu");
	gtk_box_append(GTK_BOX(menu), *field);
	for (guint index = 0; index < G_N_ELEMENTS(PRESETS); index++) {
		GtkWidget *preset = gtk_button_new_with_label(PRESETS[index]);
		gtk_widget_add_css_class(preset, "flat");
		gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(preset))), 0);
		g_signal_connect(preset, "clicked", G_CALLBACK(pick_status), *field);
		gtk_box_append(GTK_BOX(menu), preset);
	}
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), menu);
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(label), STATUS_WIDTH);
	GtkWidget *opener = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(opener), label);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(opener), popover);
	gtk_widget_set_tooltip_text(opener, TEXT_STATUS);
	GtkWidget *clear = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_add_css_class(clear, "flat");
	gtk_widget_add_css_class(clear, "circular");
	gtk_widget_set_valign(clear, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(clear, TEXT_CLEAR_STATUS);
	g_signal_connect_swapped(clear, "clicked", G_CALLBACK(clear_status), *field);
	GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bubble, "status-bubble");
	gtk_widget_set_halign(bubble, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(bubble), opener);
	gtk_box_append(GTK_BOX(bubble), clear);
	g_object_set_data(G_OBJECT(*field), "status-label", label);
	g_object_set_data(G_OBJECT(*field), "status-clear", clear);
	g_object_set_data(G_OBJECT(*field), "status-menu", popover);
	g_signal_connect(*field, "changed", G_CALLBACK(show_status), NULL);
	show_status(GTK_EDITABLE(*field));
	return bubble;
}


static GtkWidget *add_field(GtkWidget *list, const char *title, const char *value) {
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
}


/* What the server does not support is left out, and a dim line under the list says so with a button naming it */
static GtkWidget *create_unsupported_note(GPtrArray *missing) {
	g_autoptr(GString) names = g_string_new(NULL);
	for (guint index = 0; index < missing->len; index++) {
		const char *joiner = index == 0 ? "" : index + 1 == missing->len ? TEXT_LIST_LAST : ", ";
		g_string_append_printf(names, "%s%s", joiner, (const char *)missing->pdata[index]);
	}
	g_autofree char *help = g_strdup_printf(TEXT_UNSUPPORTED_FIELDS, names->str);
	GtkWidget *note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
	GtkWidget *label = gtk_label_new(TEXT_SOME_UNSUPPORTED);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(note), label);
	GtkWidget *button = create_help_button(help);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_add_css_class(button, "dim-label");
	gtk_box_append(GTK_BOX(note), button);
	return note;
}


/* What the server says the user set for a key, or what the app kept from last time before the server answers */
static char *read_own_value(CrimsonNetwork *network, const char *nick, const char *key) {
	const char *current = find_metadata(network->server_room, nick, key);
	return current ? g_strdup(current) : load_profile_value(network->key, key);
}


void show_own_profile(Window *window, CrimsonNetwork *network) {
	OwnProfile *own = g_new0(OwnProfile, 1);
	own->window = window;
	own->network = network;
	IrcState *state = get_network_state(network);
	const char *nick = state && state->nick ? state->nick : network->session->record->nick;
	IrcUser *user = state && nick ? find_irc_user(state, nick) : NULL;
	gboolean has_metadata = can_network(network, "draft/metadata-2");
	own->old_nick = g_strdup(nick);
	own->old_display_name = read_own_value(network, nick, "display-name");
	own->old_picture = read_own_value(network, nick, "avatar");
	own->picture = g_strdup(own->old_picture ? own->old_picture : "");
	own->old_status = read_own_value(network, nick, "status");
	own->old_pronouns = read_own_value(network, nick, "pronouns");
	own->old_homepage = read_own_value(network, nick, "homepage");
	/* Without a real name given, the app logs in with the nickname as the real name */
	const char *realname = user && user->realname ? user->realname : network->session->record->realname;
	own->old_realname = g_strdup(realname ? realname : network->session->record->nick);
	/* The server only tells the user's own details when asked */
	if (has_metadata && network->client)
		send_irc_client_command(network->client, "METADATA", "*", "GET", OWN_PROFILE_KEYS, NULL);
	own->dialog = adw_dialog_new();
	adw_dialog_set_content_width(own->dialog, PROFILE_WIDTH);
	g_object_set_data_full(G_OBJECT(own->dialog), "own", own, (GDestroyNotify)free_own_profile);
	g_signal_connect_object(network, "changed", G_CALLBACK(refresh_own_profile), own->dialog, G_CONNECT_DEFAULT);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "profile");
	own->avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, nick, TRUE);
	GtkWidget *picture = has_metadata ? offer_picture_change(own->avatar, G_CALLBACK(change_own_picture),
		G_CALLBACK(ask_to_remove_own_picture), own) : own->avatar;
	show_own_picture(own);
	gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), picture);
	/* Chats show the display name where the server has them, and the nickname otherwise, as the big name does */
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	if (has_metadata) {
		const char *shown = own->old_display_name && *own->old_display_name ? own->old_display_name : nick;
		gtk_box_append(GTK_BOX(box), create_name_editor(&own->display_name, shown));
		own->nick = add_field(list, TEXT_NICKNAME, nick);
	} else {
		gtk_box_append(GTK_BOX(box), create_name_editor(&own->nick, nick));
	}
	capitalize_nick_field(own->nick);
	g_signal_connect(own->nick, "changed", G_CALLBACK(preview_name), own);
	/* Pronouns and status sit with the name the way others see them, which keeps the whole page in view */
	if (has_metadata) {
		own->pronouns = create_line_field(own->old_pronouns, TEXT_ADD_PRONOUNS, "pronouns-field", PRONOUNS_WIDTH);
		gtk_widget_set_tooltip_text(own->pronouns, TEXT_PRONOUNS);
		gtk_box_append(GTK_BOX(box), own->pronouns);
	}
	/* The real name and the account logged into sit small under the name, as on anyone's profile */
	if (own->old_realname && nick && g_ascii_strcasecmp(own->old_realname, nick) != 0) {
		GtkWidget *realname_line = gtk_label_new(own->old_realname);
		gtk_label_set_wrap(GTK_LABEL(realname_line), TRUE);
		gtk_widget_add_css_class(realname_line, "dim-label");
		gtk_box_append(GTK_BOX(box), realname_line);
	}
	/* The server is named here rather than in the title, where a long name would not fit */
	GtkWidget *account_line = create_account_line();
	const char *account = state ? state->account : NULL;
	g_autofree char *where = account && *account ? g_strdup_printf(TEXT_LOGGED_IN_ON, account, network->name)
		: g_strdup_printf(TEXT_ON_SERVER, network->name);
	gtk_label_set_text(GTK_LABEL(account_line), where);
	gtk_box_append(GTK_BOX(box), account_line);
	if (has_metadata)
		gtk_box_append(GTK_BOX(box), create_status_field(&own->status, own->old_status));
	own->realname = add_field(list, TEXT_REAL_NAME, own->old_realname);
	const struct {
		GtkWidget **row;
		const char *title, *value;
		gboolean is_supported;
	} OPTIONAL_FIELDS[] = {
		{&own->homepage, TEXT_HOMEPAGE, own->old_homepage, has_metadata},
	};
	g_autoptr(GPtrArray) missing = g_ptr_array_new();
	if (!has_metadata) {
		g_ptr_array_add(missing, TEXT_STATUS);
		g_ptr_array_add(missing, TEXT_PRONOUNS);
	}
	for (guint index = 0; index < G_N_ELEMENTS(OPTIONAL_FIELDS); index++) {
		if (OPTIONAL_FIELDS[index].is_supported)
			*OPTIONAL_FIELDS[index].row = add_field(list, OPTIONAL_FIELDS[index].title, OPTIONAL_FIELDS[index].value);
		else
			g_ptr_array_add(missing, (gpointer)OPTIONAL_FIELDS[index].title);
	}
	/* Pictures travel as a profile key too, so a server without the registry has no pictures either */
	if (!has_metadata)
		g_ptr_array_add(missing, TEXT_PICTURE);
	gtk_box_append(GTK_BOX(box), list);
	if (missing->len)
		gtk_box_append(GTK_BOX(box), create_unsupported_note(missing));
	/* The login to the server's account services, which a bouncer keeps without sending the password back */
	GtkWidget *login_title = gtk_label_new(TEXT_SASL_LOGIN);
	gtk_label_set_xalign(GTK_LABEL(login_title), 0);
	gtk_widget_add_css_class(login_title, "heading");
	gtk_widget_set_margin_top(login_title, 6);
	gtk_box_append(GTK_BOX(box), login_title);
	GtkWidget *login = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(login), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(login, "boxed-list");
	AccountRecord *record = network->session->record;
	own->old_account = g_strdup(network->bouncer_id ? g_hash_table_lookup(network->attributes, "sasl-username")
		: record->sasl_username);
	own->old_password = g_strdup(network->bouncer_id ? NULL : record->sasl_password);
	own->account = add_field(login, TEXT_ACCOUNT_NAME, own->old_account);
	/* Asked to log in for a registered nickname, the account is most likely named after it */
	const char *wanted = network->bouncer_id ? g_hash_table_lookup(network->attributes, "nickname") : record->nick;
	if (network->wants_login && (!own->old_account || !*own->old_account) && wanted)
		gtk_editable_set_text(GTK_EDITABLE(own->account), wanted);
	own->password = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(own->password), TEXT_PASSWORD);
	/* A bouncer keeps the password without ever sending it back, so a stand-in shows that one is set */
	own->has_stand_in = network->bouncer_id && own->old_account && *own->old_account;
	gtk_editable_set_text(GTK_EDITABLE(own->password), own->has_stand_in ? PASSWORD_STAND_IN
		: own->old_password ? own->old_password : "");
	if (own->has_stand_in) {
		GtkEventController *focus = gtk_event_controller_focus_new();
		g_signal_connect_swapped(focus, "enter", G_CALLBACK(clear_stand_in), own);
		g_signal_connect_swapped(focus, "leave", G_CALLBACK(restore_stand_in), own);
		gtk_widget_add_controller(own->password, focus);
		own->peek = find_peek_button(own->password);
		g_signal_connect_swapped(own->password, "changed", G_CALLBACK(update_peek), own);
		update_peek(own);
	}
	gtk_list_box_append(GTK_LIST_BOX(login), own->password);
	gtk_box_append(GTK_BOX(box), login);
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *save = NULL;
	GtkWidget *header = create_form_header(own->dialog, TEXT_SAVE, &save);
	g_signal_connect(save, "clicked", G_CALLBACK(save_own_profile), own);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_help_button(TEXT_HELP_PROFILE));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
	adw_dialog_set_title(own->dialog, TEXT_ACCOUNT_TITLE);
	adw_dialog_set_child(own->dialog, toolbar);
	adw_dialog_present(own->dialog, GTK_WIDGET(window->window));
}


typedef struct {
	CrimsonRoom *room;
	Window *window;
	GCallback show_members;
	gpointer data;
	AdwDialog *dialog;
	AdwNavigationView *pages;
} RoomDetails;


static void free_room_details(RoomDetails *details) {
	g_object_unref(details->room);
	g_free(details);
}


static void open_room_settings(AdwActionRow *row, RoomDetails *details) {
	adw_navigation_view_push(details->pages, create_room_settings_page(details->room));
}


static void invite_from_details(AdwActionRow *row, RoomDetails *details) {
	Window *window = details->window;
	CrimsonRoom *room = g_object_ref(details->room);
	adw_dialog_close(details->dialog);
	show_room_invite_dialog(window, room);
	g_object_unref(room);
}


static GtkWidget *create_page_row(const char *title, const char *icon, GCallback activated, RoomDetails *details) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icon));
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	g_signal_connect(row, "activated", activated, details);
	return row;
}


static void show_room_members(GtkButton *button, RoomDetails *details) {
	adw_dialog_close(details->dialog);
	((void (*)(gpointer))details->show_members)(details->data);
}


static void leave_from_details(GtkButton *button, RoomDetails *details) {
	adw_dialog_close(details->dialog);
}


void show_room_details(Window *window, CrimsonRoom *room, GCallback show_members, gpointer data) {
	RoomDetails *details = g_new0(RoomDetails, 1);
	details->room = g_object_ref(room);
	details->window = window;
	details->show_members = show_members;
	details->data = data;
	details->dialog = adw_dialog_new();
	adw_dialog_set_content_width(details->dialog, PROFILE_WIDTH);
	adw_dialog_set_content_height(details->dialog, PROFILE_HEIGHT);
	g_object_set_data_full(G_OBJECT(details->dialog), "details", details, (GDestroyNotify)free_room_details);
	IrcState *state = get_network_state(room->network);
	IrcChannel *channel = state ? find_irc_channel(state, room->name) : NULL;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "profile");
	GtkWidget *avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, NULL, TRUE);
	fill_room_avatar(ADW_AVATAR(avatar), room, PROFILE_AVATAR_SIZE);
	GtkWidget *choice = offer_picture_choice(avatar, window, room->network, PICTURE_ROOM, room->name, FALSE);
	gtk_widget_set_halign(choice, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), choice);
	g_autofree char *title = describe_room_title(room);
	GtkWidget *name = gtk_label_new(title);
	gtk_widget_add_css_class(name, "title-1");
	gtk_box_append(GTK_BOX(box), name);
	g_autofree char *size = describe_room_subtitle(room);
	GtkWidget *count = gtk_label_new(size);
	gtk_widget_add_css_class(count, "dim-label");
	gtk_box_append(GTK_BOX(box), count);
	/* Each sentence of the topic starts its own line, and its links open in the browser */
	if (room->topic && *room->topic) {
		g_autoptr(GRegex) endings = g_regex_new("([.!?]) +", 0, 0, NULL);
		g_autofree char *lines = g_regex_replace(endings, room->topic, -1, 0, "\\1\n", 0, NULL);
		g_autofree char *markup = format_irc_markup(lines);
		GtkWidget *text = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(text), markup);
		ask_before_label_links(text);
		gtk_label_set_wrap(GTK_LABEL(text), TRUE);
		/* A topic of any length keeps the room's buttons in view, and shows whole when pointed at */
		gtk_label_set_lines(GTK_LABEL(text), TOPIC_LINES);
		gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
		gtk_widget_set_tooltip_text(text, room->topic);
		gtk_label_set_justify(GTK_LABEL(text), GTK_JUSTIFY_CENTER);
		gtk_widget_set_margin_top(text, 6);
		gtk_box_append(GTK_BOX(box), text);
	}
	GtkWidget *members = gtk_button_new_with_label(TEXT_SHOW_MEMBERS);
	gtk_widget_add_css_class(members, "pill");
	gtk_widget_add_css_class(members, "suggested-action");
	gtk_widget_set_halign(members, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(members, 8);
	g_signal_connect(members, "clicked", G_CALLBACK(show_room_members), details);
	gtk_box_append(GTK_BOX(box), members);
	GtkWidget *rows = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(rows, "boxed-list");
	gtk_widget_set_margin_top(rows, 12);
	ProfileView rows_view = {.rows = rows};
	if (channel && channel->topic_setter) {
		g_autofree char *setter = get_source_nick(channel->topic_setter);
		g_autofree char *day = channel->topic_time ? format_day_heading(channel->topic_time * G_USEC_PER_SEC) : NULL;
		g_autofree char *by = day ? g_strdup_printf(TEXT_TOPIC_SET_ON, setter, day) : g_strdup(setter);
		add_row(&rows_view, TEXT_TOPIC_SET_BY, by);
	}
	add_row(&rows_view, TEXT_PROFILE_SERVER, room->network->name);
	/* Everyone sees how a joined room is set up, and its moderators change it there */
	if (channel) {
		gtk_list_box_append(GTK_LIST_BOX(rows), create_page_row(TEXT_INVITE_SOMEONE, "contact-new-symbolic",
			G_CALLBACK(invite_from_details), details));
		gtk_list_box_append(GTK_LIST_BOX(rows), create_page_row(TEXT_ROOM_SETTINGS, "go-next-symbolic",
			G_CALLBACK(open_room_settings), details));
	}
	gtk_box_append(GTK_BOX(box), rows);
	GtkWidget *leave = gtk_button_new_with_label(TEXT_LEAVE_ROOM);
	gtk_widget_add_css_class(leave, "pill");
	gtk_widget_add_css_class(leave, "room-card-leave");
	gtk_widget_set_halign(leave, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(leave, 12);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(leave), "win.leave-room");
	g_signal_connect(leave, "clicked", G_CALLBACK(leave_from_details), details);
	gtk_box_append(GTK_BOX(box), leave);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroller);
	details->pages = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
	adw_navigation_view_add(details->pages, adw_navigation_page_new(toolbar, title));
	adw_dialog_set_child(details->dialog, GTK_WIDGET(details->pages));
	adw_dialog_present(details->dialog, GTK_WIDGET(window->window));
}
