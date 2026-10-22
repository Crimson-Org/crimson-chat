#include <string.h>

#include "../lang/en.h"
#include "images.h"
#include "profile.h"
#include "room-look.h"
#include "wizards.h"

#define PROFILE_WIDTH 400
#define PROFILE_AVATAR_SIZE 96
#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400

typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *stack, *rows, *realname;
	char *nick;
} ProfileView;


static void free_profile_view(ProfileView *view) {
	if (view->network)
		cancel_profile(view->network, view);
	g_free(view->nick);
	g_free(view);
}


static char *describe_idle(gint64 seconds) {
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
			g_string_append_printf(list, "%s%s", list->len ? " " : "", name);
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
	gtk_widget_add_css_class(row, "property");
	gtk_list_box_append(GTK_LIST_BOX(view->rows), row);
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
	add_row(view, TEXT_PROFILE_AWAY, profile->away);
	add_row(view, TEXT_PROFILE_ACCOUNT, profile->account);
	g_autofree char *idle = profile->idle_seconds > 0 || profile->server ? describe_idle(profile->idle_seconds) : NULL;
	add_row(view, TEXT_PROFILE_IDLE, idle);
	g_autofree char *rooms = list_rooms(profile->channels);
	add_row(view, TEXT_PROFILE_ROOMS, rooms);
	g_autofree char *address = profile->user && profile->host ? g_strdup_printf("%s@%s", profile->user, profile->host)
		: NULL;
	add_row(view, TEXT_PROFILE_ADDRESS, address);
	add_row(view, TEXT_PROFILE_SERVER, profile->server);
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
	g_object_set_data_full(G_OBJECT(view->dialog), "view", view, (GDestroyNotify)free_profile_view);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "profile");
	GtkWidget *avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, nick, TRUE);
	const char *url = find_metadata(room, nick, "avatar");
	g_autofree char *sized = fill_image_size(url, PROFILE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(avatar), sized);
	gtk_widget_set_halign(avatar, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), avatar);
	GtkWidget *name = gtk_label_new(nick);
	gtk_widget_add_css_class(name, "title-1");
	gtk_box_append(GTK_BOX(box), name);
	view->realname = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(view->realname), TRUE);
	gtk_widget_add_css_class(view->realname, "dim-label");
	gtk_box_append(GTK_BOX(box), view->realname);
	GtkWidget *message = gtk_button_new_with_label(TEXT_SEND_MESSAGE);
	gtk_widget_add_css_class(message, "pill");
	gtk_widget_add_css_class(message, "suggested-action");
	gtk_widget_set_halign(message, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(message, 8);
	g_signal_connect(message, "clicked", G_CALLBACK(send_message), view);
	gtk_box_append(GTK_BOX(box), message);
	view->stack = gtk_stack_new();
	gtk_stack_set_vhomogeneous(GTK_STACK(view->stack), FALSE);
	gtk_stack_add_named(GTK_STACK(view->stack), adw_spinner_new(), "loading");
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
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
	adw_dialog_set_child(view->dialog, toolbar);
	adw_dialog_present(view->dialog, GTK_WIDGET(window->window));
	if (network->state == NETWORK_CONNECTED)
		request_profile(network, nick, show_details, view);
	else
		gtk_stack_set_visible_child_name(GTK_STACK(view->stack), "details");
}


typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *avatar, *nick, *display_name, *realname, *picture, *account, *password;
	/* What the fields held when the dialog opened, so only changes are sent */
	char *old_nick, *old_display_name, *old_realname, *old_picture, *old_account, *old_password;
} OwnProfile;


static void free_own_profile(OwnProfile *own) {
	g_free(own->old_nick);
	g_free(own->old_display_name);
	g_free(own->old_realname);
	g_free(own->old_picture);
	g_free(own->old_account);
	g_free(own->old_password);
	g_free(own);
}


static const char *read_field(GtkWidget *row) {
	return row ? gtk_editable_get_text(GTK_EDITABLE(row)) : "";
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
	if (network->client && has_changed(own->realname, own->old_realname)) {
		send_irc_client_command(network->client, "SETNAME", read_field(own->realname), NULL);
		if (is_direct) {
			g_free(record->realname);
			record->realname = g_strdup(read_field(own->realname));
		}
	}
	if (network->client && has_changed(own->display_name, own->old_display_name))
		set_metadata(network, "display-name", read_field(own->display_name));
	if (network->client && has_changed(own->picture, own->old_picture))
		set_metadata(network, "avatar", read_field(own->picture));
	if (own->display_name || own->picture)
		save_network_profile(network->key, read_field(own->display_name), read_field(own->picture));
	/* A new login only counts on the next connection, so changing it reconnects */
	gboolean has_new_login = has_changed(own->account, own->old_account) || has_changed(own->password, own->old_password);
	if (has_new_login && !is_direct) {
		g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(attributes, g_strdup("sasl-username"), g_strdup(read_field(own->account)));
		if (*read_field(own->password))
			g_hash_table_insert(attributes, g_strdup("sasl-password"), g_strdup(read_field(own->password)));
		change_bouncer_network(network, attributes);
	} else if (has_new_login) {
		AccountRecord *changed = copy_account_record(record);
		g_free(changed->sasl_username);
		changed->sasl_username = *read_field(own->account) ? g_strdup(read_field(own->account)) : NULL;
		g_free(changed->sasl_password);
		changed->sasl_password = *read_field(own->password) ? g_strdup(read_field(own->password)) : NULL;
		change_account(network->session, changed);
		free_account_record(changed);
	} else if (is_direct) {
		save_chat_accounts(network->session->chat);
	}
	adw_dialog_close(own->dialog);
}


/* The server answers with the user's own details after the dialog opened, and they fill fields left untouched */
static void refresh_own_field(GtkWidget *row, char **old, const char *current) {
	if (!row || !current || g_strcmp0(current, *old) == 0 || has_changed(row, *old))
		return;
	g_free(*old);
	*old = g_strdup(current);
	gtk_editable_set_text(GTK_EDITABLE(row), current);
}


static void refresh_own_profile(CrimsonNetwork *network, AdwDialog *dialog) {
	OwnProfile *own = g_object_get_data(G_OBJECT(dialog), "own");
	IrcState *state = get_network_state(network);
	if (!own || !state || !state->nick)
		return;
	refresh_own_field(own->display_name, &own->old_display_name, find_metadata(network->server_room, state->nick,
		"display-name"));
	refresh_own_field(own->picture, &own->old_picture, find_metadata(network->server_room, state->nick, "avatar"));
}


static void preview_picture(GtkEditable *editable, OwnProfile *own) {
	const char *url = gtk_editable_get_text(editable);
	g_autofree char *sized = fill_image_size(*url ? url : NULL, PROFILE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(own->avatar), sized);
}


static void preview_name(GtkEditable *editable, OwnProfile *own) {
	adw_avatar_set_text(ADW_AVATAR(own->avatar), gtk_editable_get_text(editable));
}


/* A field the server supports can be edited, and one it does not support says so instead */
static GtkWidget *add_field(GtkWidget *list, const char *title, const char *value, gboolean is_supported) {
	if (!is_supported) {
		GtkWidget *row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), TEXT_NOT_SUPPORTED);
		gtk_widget_set_sensitive(row, FALSE);
		gtk_list_box_append(GTK_LIST_BOX(list), row);
		return NULL;
	}
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
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
	g_autofree char *saved_name = NULL, *saved_picture = NULL;
	load_network_profile(network->key, &saved_name, &saved_picture);
	const char *display_name = find_metadata(network->server_room, nick, "display-name");
	const char *picture = find_metadata(network->server_room, nick, "avatar");
	own->old_display_name = g_strdup(display_name ? display_name : saved_name);
	own->old_picture = g_strdup(picture ? picture : saved_picture);
	/* Without a real name given, the app logs in with the nickname as the real name */
	const char *realname = user && user->realname ? user->realname : network->session->record->realname;
	own->old_realname = g_strdup(realname ? realname : network->session->record->nick);
	/* The server only tells the user's own details when asked */
	if (has_metadata && network->client)
		send_irc_client_command(network->client, "METADATA", "*", "GET", "avatar", "display-name", NULL);
	own->dialog = adw_dialog_new();
	adw_dialog_set_content_width(own->dialog, PROFILE_WIDTH);
	g_object_set_data_full(G_OBJECT(own->dialog), "own", own, (GDestroyNotify)free_own_profile);
	g_signal_connect_object(network, "changed", G_CALLBACK(refresh_own_profile), own->dialog, G_CONNECT_DEFAULT);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "profile");
	own->avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, nick, TRUE);
	gtk_widget_set_halign(own->avatar, GTK_ALIGN_CENTER);
	g_autofree char *sized = fill_image_size(own->old_picture, PROFILE_AVATAR_SIZE * 2);
	show_avatar_image(ADW_AVATAR(own->avatar), sized);
	gtk_box_append(GTK_BOX(box), own->avatar);
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	own->nick = add_field(list, TEXT_NICKNAME, nick, TRUE);
	capitalize_nick_field(own->nick);
	g_signal_connect(own->nick, "changed", G_CALLBACK(preview_name), own);
	own->display_name = add_field(list, TEXT_DISPLAY_NAME, own->old_display_name, has_metadata);
	own->realname = add_field(list, TEXT_REAL_NAME, own->old_realname, can_network(network, "setname"));
	own->picture = add_field(list, TEXT_PICTURE_ADDRESS, own->old_picture, has_metadata);
	if (own->picture)
		g_signal_connect(own->picture, "changed", G_CALLBACK(preview_picture), own);
	gtk_box_append(GTK_BOX(box), list);
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
	own->account = add_field(login, TEXT_ACCOUNT_NAME, own->old_account, TRUE);
	own->password = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(own->password), TEXT_PASSWORD);
	gtk_editable_set_text(GTK_EDITABLE(own->password), own->old_password ? own->old_password : "");
	gtk_list_box_append(GTK_LIST_BOX(login), own->password);
	gtk_box_append(GTK_BOX(box), login);
	GtkWidget *save = gtk_button_new_with_label(TEXT_SAVE);
	gtk_widget_add_css_class(save, "pill");
	gtk_widget_add_css_class(save, "suggested-action");
	gtk_widget_set_halign(save, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive(save, network->state == NETWORK_CONNECTED);
	g_signal_connect(save, "clicked", G_CALLBACK(save_own_profile), own);
	gtk_box_append(GTK_BOX(box), save);
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_help_button(TEXT_HELP_PROFILE));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
	g_autofree char *title = g_strdup_printf(TEXT_ACCOUNT_ON, network->name);
	adw_dialog_set_title(own->dialog, title);
	adw_dialog_set_child(own->dialog, toolbar);
	adw_dialog_present(own->dialog, GTK_WIDGET(window->window));
}
