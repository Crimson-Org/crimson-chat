#pragma once

#include "model.h"

/* Whether a message someone else sent should notify, by the room's override or else the defaults in Settings */
gboolean wants_notification(CrimsonRoom *room, CrimsonMessage *message);
/* Whether any message in the room notifies, which the room's menu offers to change */
gboolean is_room_notified(CrimsonRoom *room);
/* Whether the room's unread messages show as a badge, by the room's override or else the defaults in Settings */
gboolean shows_room_badge(CrimsonRoom *room);
/* Whether the room shows badges at all, which the room's menu offers to change */
gboolean is_room_badged(CrimsonRoom *room);
/* The unread messages a server's badge counts, and whether any of them is a mention or a reaction */
guint count_network_badges(CrimsonNetwork *network, gboolean *has_mention);
