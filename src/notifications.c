#include "notifications.h"
#include "settings.h"


gboolean wants_notification(CrimsonRoom *room, CrimsonMessage *message) {
	if (room->kind == ROOM_SERVER || message->is_own)
		return FALSE;
	Settings *settings = get_settings();
	const RoomOverride *override = find_room_override(settings->notification_overrides, room->network->key,
		room->name);
	if (override)
		return override->is_enabled;
	if (room->kind == ROOM_PRIVATE)
		return settings->notifies_private;
	return message->is_highlight ? settings->notifies_mentions : settings->notifies_room_messages;
}


gboolean is_room_notified(CrimsonRoom *room) {
	Settings *settings = get_settings();
	const RoomOverride *override = find_room_override(settings->notification_overrides, room->network->key,
		room->name);
	if (override)
		return override->is_enabled;
	return room->kind == ROOM_PRIVATE ? settings->notifies_private : settings->notifies_room_messages;
}


static const RoomOverride *find_badge_override(CrimsonRoom *room) {
	return find_room_override(get_settings()->badge_overrides, room->network->key, room->name);
}


gboolean shows_room_badge(CrimsonRoom *room) {
	Settings *settings = get_settings();
	const RoomOverride *override = find_badge_override(room);
	if (override)
		return override->is_enabled;
	if (room->kind == ROOM_PRIVATE)
		return settings->shows_private_badges;
	return room->has_mention || room->new_reaction ? settings->shows_mention_badges : settings->shows_room_badges;
}


gboolean is_room_badged(CrimsonRoom *room) {
	Settings *settings = get_settings();
	const RoomOverride *override = find_badge_override(room);
	if (override)
		return override->is_enabled;
	return room->kind == ROOM_PRIVATE ? settings->shows_private_badges : settings->shows_room_badges;
}


guint count_network_badges(CrimsonNetwork *network, gboolean *has_mention) {
	guint total = 0;
	*has_mention = FALSE;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), index);
		/* What the server says stays out of the list, so it adds nothing to the badges either */
		if (room->kind == ROOM_SERVER || !shows_room_badge(room))
			continue;
		total += room->unread_count;
		*has_mention |= room->has_mention || room->new_reaction;
	}
	return total;
}
