#include "notifications.h"
#include "settings.h"


gboolean wants_notification(CrimsonRoom *room, CrimsonMessage *message) {
	if (room->kind == ROOM_SERVER || message->is_own)
		return FALSE;
	Settings *settings = get_settings();
	const NotificationOverride *override = find_notification_override(settings, room->network->key, room->name);
	if (override)
		return override->is_enabled;
	if (room->kind == ROOM_PRIVATE)
		return settings->notifies_private;
	return message->is_highlight ? settings->notifies_mentions : settings->notifies_room_messages;
}


gboolean is_room_notified(CrimsonRoom *room) {
	Settings *settings = get_settings();
	const NotificationOverride *override = find_notification_override(settings, room->network->key, room->name);
	if (override)
		return override->is_enabled;
	return room->kind == ROOM_PRIVATE ? settings->notifies_private : settings->notifies_room_messages;
}
