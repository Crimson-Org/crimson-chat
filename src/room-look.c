#include "../lang/en.h"
#include "formatting.h"
#include "images.h"
#include "profile.h"
#include "room-look.h"

#define SECONDS_PER_MINUTE 60
#include "settings.h"
#include "pictures.h"

#define TYPING_NAMES_SHOWN 2


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


char *describe_room_preview(CrimsonRoom *room) {
	if (room->kind == ROOM_SERVER)
		return describe_room_subtitle(room);
	if (is_room_typing(room) && get_settings()->shows_typing)
		return describe_typing(room);
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
