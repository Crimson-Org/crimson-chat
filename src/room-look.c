#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "profile.h"
#include "room-look.h"

#define SECONDS_PER_MINUTE 60
#include "settings.h"
#include "pictures.h"

#define TYPING_NAMES_SHOWN 2
#define TYPING_DOTS_MILLISECONDS 750

/* Lines saying who is typing end in dots that count up, and one clock moves them all together */
static const char *const TYPING_DOTS[] = {"...", ".", ".."};
static GPtrArray *typing_labels;
static guint typing_source, typing_step;


const char *find_metadata(CrimsonRoom *room, const char *nick, const char *key) {
	IrcState *state = get_network_state(room->network);
	if (!state)
		return NULL;
	if (!nick) {
		IrcChannel *channel = room->kind == ROOM_CHANNEL ? find_irc_channel(state, room->name) : NULL;
		if (channel)
			return g_hash_table_lookup(channel->metadata, key);
		nick = room->kind == ROOM_PRIVATE ? room->name : NULL;
	}
	IrcUser *user = nick ? find_irc_user(state, nick) : NULL;
	return user ? g_hash_table_lookup(user->metadata, key) : NULL;
}


char *describe_room_title(CrimsonRoom *room) {
	if (room->kind == ROOM_SERVER)
		return g_strdup(room->network->name);
	const char *display = find_metadata(room, NULL, "display-name");
	return g_strdup(display && *display ? display : room->name);
}


void fill_room_avatar(AdwAvatar *avatar, CrimsonRoom *room, int size) {
	g_autofree char *title = describe_room_title(room);
	const char *name = room->kind == ROOM_CHANNEL && *title == '#' ? title + 1 : title;
	adw_avatar_set_size(avatar, size);
	adw_avatar_set_text(avatar, name);
	/* Rooms are rounded squares and people are round, so the two read apart at a glance */
	if (room->kind == ROOM_PRIVATE)
		gtk_widget_remove_css_class(GTK_WIDGET(avatar), "room-avatar");
	else
		gtk_widget_add_css_class(GTK_WIDGET(avatar), "room-avatar");
	const char *own = room->kind == ROOM_CHANNEL ? find_own_picture(room->network, PICTURE_ROOM, room->name)
		: room->kind == ROOM_PRIVATE ? find_own_picture(room->network, PICTURE_PERSON, room->name) : NULL;
	const char *url = room->kind == ROOM_SERVER ? pick_network_picture(room->network)
		: own ? own : find_metadata(room, NULL, "avatar");
	g_autofree char *sized = fill_image_size(url, size * 2);
	show_avatar_image(avatar, sized);
}


static char *describe_typing(CrimsonRoom *room) {
	g_autoptr(GPtrArray) nicks = g_hash_table_get_keys_as_ptr_array(room->typing);
	if (nicks->len == 1)
		return g_strdup_printf(TEXT_TYPING_ONE, (const char *)g_ptr_array_index(nicks, 0));
	if (nicks->len == TYPING_NAMES_SHOWN)
		return g_strdup_printf(TEXT_TYPING_TWO, (const char *)g_ptr_array_index(nicks, 0),
			(const char *)g_ptr_array_index(nicks, 1));
	return g_strdup_printf(TEXT_TYPING_MANY, nicks->len);
}


static void show_typing_dots(GtkWidget *label) {
	const char *text = g_object_get_data(G_OBJECT(label), "typing-text");
	g_autofree char *line = g_strconcat(text, TYPING_DOTS[typing_step], NULL);
	gtk_label_set_text(GTK_LABEL(label), line);
}


static gboolean step_typing_dots(gpointer data) {
	typing_step = (typing_step + 1) % G_N_ELEMENTS(TYPING_DOTS);
	for (guint index = 0; index < typing_labels->len; index++)
		show_typing_dots(g_ptr_array_index(typing_labels, index));
	return G_SOURCE_CONTINUE;
}


static void forget_typing_label(gpointer data, GObject *label) {
	g_ptr_array_remove(typing_labels, label);
	if (!typing_labels->len)
		g_clear_handle_id(&typing_source, g_source_remove);
}


static void stop_typing_dots(GtkWidget *label) {
	if (!typing_labels || !g_ptr_array_find(typing_labels, label, NULL))
		return;
	g_object_weak_unref(G_OBJECT(label), forget_typing_label, NULL);
	forget_typing_label(NULL, G_OBJECT(label));
}


void show_room_line(GtkWidget *label, const char *text, gboolean is_typing) {
	if (!is_typing) {
		stop_typing_dots(label);
		gtk_widget_remove_css_class(label, "typing");
		gtk_label_set_text(GTK_LABEL(label), text);
		return;
	}
	gtk_widget_add_css_class(label, "typing");
	g_object_set_data_full(G_OBJECT(label), "typing-text", g_strdup(text), g_free);
	if (!typing_labels)
		typing_labels = g_ptr_array_new();
	if (!g_ptr_array_find(typing_labels, label, NULL)) {
		g_ptr_array_add(typing_labels, label);
		g_object_weak_ref(G_OBJECT(label), forget_typing_label, NULL);
	}
	if (!typing_source) {
		typing_step = 0;
		typing_source = g_timeout_add(TYPING_DOTS_MILLISECONDS, step_typing_dots, NULL);
	}
	show_typing_dots(label);
}


char *describe_room_preview(CrimsonRoom *room) {
	if (room->kind == ROOM_SERVER)
		return describe_room_subtitle(room);
	if (is_room_typing(room) && get_settings()->shows_typing)
		return describe_typing(room);
	if (room->new_reaction_text && room->new_reaction_time >= room->last_time)
		return g_strdup(room->new_reaction_text);
	if (room->preview && *room->preview) {
		g_autofree char *plain = strip_irc_formatting(room->preview);
		if (room->is_preview_own)
			return g_strdup_printf(TEXT_PREVIEW_OWN, plain);
		if (room->kind == ROOM_CHANNEL && room->preview_sender)
			return g_strdup_printf(TEXT_PREVIEW_SENDER, room->preview_sender, plain);
		return g_steal_pointer(&plain);
	}
	if (room->kind == ROOM_CHANNEL && room->topic)
		return strip_irc_formatting(room->topic);
	return g_strdup("");
}


static char *describe_channel(CrimsonRoom *room) {
	IrcState *state = get_network_state(room->network);
	IrcChannel *channel = state ? find_irc_channel(state, room->name) : NULL;
	if (!channel)
		return g_strdup(room->is_joined ? "" : TEXT_NOT_JOINED);
	guint members = g_hash_table_size(channel->members);
	return g_strdup_printf(members == 1 ? TEXT_ONE_MEMBER : TEXT_MEMBERS, members);
}


char *describe_room_subtitle(CrimsonRoom *room) {
	if (is_room_typing(room) && get_settings()->shows_typing)
		return describe_typing(room);
	if (room->kind == ROOM_CHANNEL)
		return describe_channel(room);
	if (room->kind == ROOM_SERVER) {
		const char *state = room->network->state == NETWORK_CONNECTED ? TEXT_CONNECTED
			: room->network->state == NETWORK_CONNECTING ? TEXT_CONNECTING : TEXT_DISCONNECTED;
		return g_strdup(state);
	}
	IrcState *state = get_network_state(room->network);
	IrcUser *user = state ? find_irc_user(state, room->name) : NULL;
	if (user && user->is_away)
		return g_strdup(TEXT_AWAY);
	if (room->presence == PRESENCE_ONLINE)
		return g_strdup(TEXT_ONLINE);
	return g_strdup(room->presence == PRESENCE_OFFLINE ? TEXT_OFFLINE : "");
}


char *describe_room_activity(CrimsonRoom *room) {
	switch (room->activity) {
	case ACTIVITY_GONE:
		return g_strdup(TEXT_OFFLINE);
	case ACTIVITY_AWAY:
		return g_strdup(TEXT_AWAY);
	case ACTIVITY_SEEN: {
		gint64 seconds = (g_get_real_time() - room->last_active) / G_USEC_PER_SEC;
		if (seconds < SECONDS_PER_MINUTE)
			return g_strdup(TEXT_ACTIVE_NOW);
		g_autofree char *ago = describe_idle(seconds);
		return g_strdup_printf(TEXT_LAST_ACTIVE, ago);
	}
	default:
		return NULL;
	}
}
