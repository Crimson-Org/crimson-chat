#pragma once

#include "irc/message.h"

/* How a numeric reply from the server shows: not at all, as a quiet note, or as a problem the user has to know of */
typedef enum {
	REPLY_HIDDEN,
	REPLY_NOTE,
	REPLY_PROBLEM,
	/* A room that would not let the user in, which matters even when the app rejoined it by itself */
	REPLY_REFUSED_JOIN,
} ReplyKind;

/* A numeric reply in plain words, with how it shows, the room or person it concerns, which stays valid while the
 * message does, and the window action that helps with it, each NULL for none */
char *describe_reply(const IrcMessage *message, ReplyKind *kind, const char **subject, const char **action);
/* Someone leaving a room or the network, with the reason they gave in plain words, where the stock reasons servers
 * and clients add say nothing more than that they left or lost their connection */
char *describe_leaving(const char *nick, const char *reason);
/* Someone removing another from a room, with the reason only when it says something */
char *describe_kick(const char *kicker, const char *kicked, const char *reason);
