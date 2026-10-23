#pragma once

#include "model.h"

/* Whether a message someone else sent should notify, by the room's override or else the defaults in Settings */
gboolean wants_notification(CrimsonRoom *room, CrimsonMessage *message);
/* Whether any message in the room notifies, which the room's menu offers to change */
gboolean is_room_notified(CrimsonRoom *room);
