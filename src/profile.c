#include <string.h>

#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "profile.h"
#include "room-look.h"
#include "wizards.h"
#include "pictures.h"
#include "uploads.h"

#define PROFILE_WIDTH 400
/* A person's profile opens at its full height, so it does not grow when the details arrive */
#define PROFILE_HEIGHT 620
#define PROFILE_AVATAR_SIZE 96
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


/* A homepage opens in the browser from its row */
static void open_homepage(GtkButton *button, ProfileView *view) {
	const char *url = g_object_get_data(G_OBJECT(button), "url");
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(view->window->window), NULL, NULL, NULL);
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
	GtkWidget *choice = offer_picture_choice(avatar, window, network, PICTURE_PERSON, nick);
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
	GtkWidget *message = gtk_button_new_with_label(TEXT_SEND_MESSAGE);
	gtk_widget_add_css_class(message, "pill");
	gtk_widget_add_css_class(message, "suggested-action");
	gtk_widget_set_halign(message, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(message, 8);
	g_signal_connect(message, "clicked", G_CALLBACK(send_message), view);
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
	GtkWidget *avatar, *nick, *display_name, *realname, *picture, *account, *password;
	/* What the fields held when the dialog opened, so only changes are sent */
	char *old_nick, *old_display_name, *old_realname, *old_picture, *old_account, *old_password;
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


/* Operators, admins and owners of a room may change what the room shows everyone */
static gboolean is_room_operator(IrcState *state, IrcChannel *channel) {
	if (!state || !channel || !state->nick)
		return FALSE;
	g_autofree char *folded = fold_state_name(state, state->nick);
	const char *symbols = g_hash_table_lookup(channel->members, folded);
	return symbols && (strchr(symbols, '@') || strchr(symbols, '&') || strchr(symbols, '~'));
}


/* Where an uploaded picture is going, kept while the file dialog is open and the picture travels */
typedef struct {
	Window *window;
	CrimsonNetwork *network;
	char *target;
	GtkWidget *row;
} PictureUpload;


static void free_picture_upload(PictureUpload *upload) {
	g_object_unref(upload->network);
	g_free(upload->target);
	g_clear_object(&upload->row);
	g_free(upload);
}


static void finish_picture_upload(const char *url, const char *problem, gpointer data) {
	PictureUpload *upload = data;
	gtk_widget_set_sensitive(upload->row, TRUE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(upload->row), problem ? problem : TEXT_UPLOAD_PICTURE_ABOUT);
	if (url && g_str_equal(upload->target, "*")) {
		/* The address is kept like a typed one, so the app sets it again on each connection */
		save_profile_value(upload->network->key, "avatar", url);
	}
	show_toast(upload->window, url ? TEXT_PICTURE_UPLOADED : problem);
	free_picture_upload(upload);
}


static void finish_choosing_upload(GObject *source, GAsyncResult *result, gpointer data) {
	PictureUpload *upload = data;
	g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file) {
		free_picture_upload(upload);
		return;
	}
	gtk_widget_set_sensitive(upload->row, FALSE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(upload->row), TEXT_UPLOADING_PICTURE);
	upload_avatar(upload->network, upload->target, file, finish_picture_upload, upload);
}


static void choose_upload(GtkWidget *row, PictureUpload *template) {
	PictureUpload *upload = g_new0(PictureUpload, 1);
	*upload = (PictureUpload){template->window, g_object_ref(template->network), g_strdup(template->target),
		g_object_ref(row)};
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
	gtk_file_dialog_open(dialog, GTK_WINDOW(upload->window->window), NULL, finish_choosing_upload, upload);
}


/* A row that sends a picture to the bouncer, for the user with "*" or for a room */
GtkWidget *create_upload_row(Window *window, CrimsonNetwork *network, const char *target) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_PICTURE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), TEXT_UPLOAD_PICTURE_ABOUT);
	GtkWidget *button = gtk_button_new_with_label(TEXT_CHOOSE);
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pill");
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	PictureUpload *template = g_new0(PictureUpload, 1);
	*template = (PictureUpload){window, g_object_ref(network), g_strdup(target), NULL};
	g_object_set_data_full(G_OBJECT(row), "upload", template, (GDestroyNotify)free_picture_upload);
	g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_activate), row);
	g_signal_connect(row, "activated", G_CALLBACK(choose_upload), template);
	return row;
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
	/* Each profile key is kept, so the app sets it again on every connection */
	const struct {
		GtkWidget *row;
		const char *old, *key;
	} FIELDS[] = {
		{own->display_name, own->old_display_name, "display-name"}, {own->picture, own->old_picture, "avatar"},
		{own->status, own->old_status, "status"}, {own->pronouns, own->old_pronouns, "pronouns"},
		{own->homepage, own->old_homepage, "homepage"},
	};
	for (guint index = 0; index < G_N_ELEMENTS(FIELDS); index++) {
		if (!FIELDS[index].row)
			continue;
		if (network->client && index >= 2 && has_changed(FIELDS[index].row, FIELDS[index].old))
			set_metadata(network, FIELDS[index].key, read_field(FIELDS[index].row));
		save_profile_value(network->key, FIELDS[index].key, read_field(FIELDS[index].row));
	}
	/* A new login only counts on the next connection, so changing it reconnects */
	gboolean has_new_login = has_changed(own->account, own->old_account)
		|| g_strcmp0(read_password(own), own->old_password ? own->old_password : "") != 0;
	if (has_new_login && !is_direct) {
		g_autoptr(GHashTable) attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(attributes, g_strdup("sasl-username"), g_strdup(read_field(own->account)));
		if (*read_password(own))
			g_hash_table_insert(attributes, g_strdup("sasl-password"), g_strdup(read_password(own)));
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
	refresh_own_field(own->status, &own->old_status, find_metadata(network->server_room, state->nick, "status"));
	refresh_own_field(own->pronouns, &own->old_pronouns, find_metadata(network->server_room, state->nick,
		"pronouns"));
	refresh_own_field(own->homepage, &own->old_homepage, find_metadata(network->server_room, state->nick,
		"homepage"));
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
	own->status = add_field(list, TEXT_STATUS, own->old_status, has_metadata);
	own->pronouns = add_field(list, TEXT_PRONOUNS, own->old_pronouns, has_metadata);
	own->homepage = add_field(list, TEXT_HOMEPAGE, own->old_homepage, has_metadata);
	/* A bouncer that takes uploads replaces the address with a button that sends a picture from the computer */
	g_autofree char *upload_url = has_metadata ? find_avatar_upload_url(network) : NULL;
	if (upload_url) {
		gtk_list_box_append(GTK_LIST_BOX(list), create_upload_row(window, network, "*"));
	} else {
		own->picture = add_field(list, TEXT_PICTURE_ADDRESS, own->old_picture, has_metadata);
		if (own->picture)
			g_signal_connect(own->picture, "changed", G_CALLBACK(preview_picture), own);
	}
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
	GtkWidget *save = gtk_button_new_with_label(TEXT_SAVE);
	gtk_widget_add_css_class(save, "pill");
	gtk_widget_add_css_class(save, "suggested-action");
	gtk_widget_set_halign(save, GTK_ALIGN_CENTER);
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


typedef struct {
	GCallback show_members;
	gpointer data;
	AdwDialog *dialog;
} RoomDetails;


static void show_room_members(GtkButton *button, RoomDetails *details) {
	adw_dialog_close(details->dialog);
	((void (*)(gpointer))details->show_members)(details->data);
}


static void leave_from_details(GtkButton *button, RoomDetails *details) {
	adw_dialog_close(details->dialog);
}


void show_room_details(Window *window, CrimsonRoom *room, GCallback show_members, gpointer data) {
	RoomDetails *details = g_new0(RoomDetails, 1);
	details->show_members = show_members;
	details->data = data;
	details->dialog = adw_dialog_new();
	adw_dialog_set_content_width(details->dialog, PROFILE_WIDTH);
	adw_dialog_set_content_height(details->dialog, PROFILE_HEIGHT);
	g_object_set_data_full(G_OBJECT(details->dialog), "details", details, g_free);
	IrcState *state = get_network_state(room->network);
	IrcChannel *channel = state ? find_irc_channel(state, room->name) : NULL;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "profile");
	GtkWidget *avatar = adw_avatar_new(PROFILE_AVATAR_SIZE, NULL, TRUE);
	fill_room_avatar(ADW_AVATAR(avatar), room, PROFILE_AVATAR_SIZE);
	GtkWidget *choice = offer_picture_choice(avatar, window, room->network, PICTURE_ROOM, room->name);
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
		gtk_label_set_wrap(GTK_LABEL(text), TRUE);
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
	/* An operator of the room can send it a picture everyone sees, where the bouncer takes uploads */
	g_autofree char *upload_url = can_network(room->network, "draft/metadata-2")
		? find_avatar_upload_url(room->network) : NULL;
	if (upload_url && is_room_operator(state, channel))
		gtk_list_box_append(GTK_LIST_BOX(rows), create_upload_row(window, room->network, room->name));
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
	adw_dialog_set_child(details->dialog, toolbar);
	adw_dialog_present(details->dialog, GTK_WIDGET(window->window));
}
