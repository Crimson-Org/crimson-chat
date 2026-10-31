#pragma once

#include "irc/state.h"

/* A MODE line on a room in plain words, such as "Alice made Bob a moderator and muted the room", starting at the
 * mode string's parameter, or NULL when it only changes something nobody needs to see */
char *describe_room_modes(IrcState *state, const IrcMessage *message, guint first);
/* A ban, exception or invite mask as the person or address it covers */
char *describe_mask(const char *mask);
