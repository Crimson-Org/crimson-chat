#include <string.h>

#include "../lang/en.h"
#include "replies.h"

/* Replies the app words itself, with the positions of the parameters its text names and of the one naming the room
 * or person it concerns, where the user's own nick comes first at zero and zero means none, and the window action
 * that helps, which takes the server's key and that room or person */
static const struct {
	int number;
	ReplyKind kind;
	const char *text;
	guint first, second, subject;
	const char *action;
} REPLIES[] = {
	{301, REPLY_NOTE, TEXT_REPLY_AWAY, 1, 2, 1, NULL},
	{305, REPLY_NOTE, TEXT_REPLY_BACK, 0, 0, 0, NULL},
	{306, REPLY_NOTE, TEXT_REPLY_MARKED_AWAY, 0, 0, 0, NULL},
	{341, REPLY_NOTE, TEXT_REPLY_INVITING, 1, 2, 2, NULL},
	{381, REPLY_NOTE, TEXT_REPLY_SERVER_OPERATOR, 0, 0, 0, NULL},
	{396, REPLY_NOTE, TEXT_REPLY_HOST_SHOWN, 1, 0, 0, NULL},
	{401, REPLY_PROBLEM, TEXT_REPLY_NOT_ONLINE, 1, 0, 1, NULL},
	{403, REPLY_PROBLEM, TEXT_REPLY_NO_ROOM, 1, 0, 0, NULL},
	{404, REPLY_PROBLEM, TEXT_REPLY_NOT_SENT, 1, 0, 1, NULL},
	{405, REPLY_PROBLEM, TEXT_REPLY_TOO_MANY_ROOMS, 0, 0, 0, NULL},
	{406, REPLY_PROBLEM, TEXT_REPLY_UNKNOWN_NICK, 1, 0, 0, NULL},
	{421, REPLY_PROBLEM, TEXT_REPLY_UNKNOWN_COMMAND, 1, 0, 0, NULL},
	{432, REPLY_PROBLEM, TEXT_REPLY_BAD_NICK, 1, 0, 0, NULL},
	{433, REPLY_PROBLEM, TEXT_REPLY_NICK_TAKEN, 1, 0, 0, NULL},
	{436, REPLY_PROBLEM, TEXT_REPLY_NICK_TAKEN, 1, 0, 0, NULL},
	{437, REPLY_PROBLEM, TEXT_REPLY_UNAVAILABLE, 1, 0, 0, NULL},
	{441, REPLY_PROBLEM, TEXT_REPLY_NOT_IN_ROOM, 1, 2, 2, NULL},
	{442, REPLY_PROBLEM, TEXT_REPLY_NOT_JOINED, 1, 0, 1, NULL},
	{443, REPLY_PROBLEM, TEXT_REPLY_ALREADY_IN_ROOM, 1, 2, 2, NULL},
	{461, REPLY_PROBLEM, TEXT_REPLY_NEEDS_MORE, 0, 0, 0, NULL},
	{465, REPLY_PROBLEM, TEXT_REPLY_SERVER_BAN, 0, 0, 0, NULL},
	{471, REPLY_REFUSED_JOIN, TEXT_REPLY_ROOM_FULL, 1, 0, 1, NULL},
	{472, REPLY_PROBLEM, TEXT_REPLY_UNKNOWN_SETTING, 0, 0, 0, NULL},
	{473, REPLY_REFUSED_JOIN, TEXT_REPLY_INVITE_ONLY, 1, 0, 1, NULL},
	{474, REPLY_REFUSED_JOIN, TEXT_REPLY_BANNED, 1, 0, 1, NULL},
	{475, REPLY_REFUSED_JOIN, TEXT_REPLY_NEEDS_PASSWORD, 1, 0, 1, "enter-room-password"},
	{476, REPLY_PROBLEM, TEXT_REPLY_BAD_ROOM_NAME, 1, 0, 0, NULL},
	{477, REPLY_REFUSED_JOIN, TEXT_REPLY_ACCOUNTS_ONLY, 1, 0, 1, "log-in"},
	{478, REPLY_PROBLEM, TEXT_REPLY_LIST_FULL, 1, 0, 1, NULL},
	{481, REPLY_PROBLEM, TEXT_REPLY_SERVER_OPERATORS_ONLY, 0, 0, 0, NULL},
	{482, REPLY_PROBLEM, TEXT_REPLY_MODERATORS_ONLY, 1, 0, 1, NULL},
	{484, REPLY_PROBLEM, TEXT_REPLY_RESTRICTED, 0, 0, 0, NULL},
	{489, REPLY_REFUSED_JOIN, TEXT_REPLY_SECURE_ONLY, 1, 0, 1, NULL},
	{502, REPLY_PROBLEM, TEXT_REPLY_OWN_SETTINGS_ONLY, 0, 0, 0, NULL},
	{691, REPLY_PROBLEM, TEXT_REPLY_NOT_SECURE, 0, 0, 0, NULL},
	{696, REPLY_PROBLEM, TEXT_REPLY_BAD_SETTING, 0, 0, 0, NULL},
	{723, REPLY_PROBLEM, TEXT_REPLY_SERVER_OPERATORS_ONLY, 0, 0, 0, NULL},
	{734, REPLY_PROBLEM, TEXT_REPLY_WATCH_LIST_FULL, 0, 0, 0, NULL},
};

/* Replies that only carry details the app keeps or asks for itself, such as server statistics, settings, name
 * lists, topics, metadata, logins and the message of the day */
static const int HIDDEN_REPLIES[] = {
	1, 2, 3, 4, 5, 42, 221, 250, 251, 252, 253, 254, 255, 265, 266, 315, 324, 329, 331, 333, 346, 347, 348, 349, 352,
	353, 354, 367, 368, 372, 375, 376, 422, 761, 766, 770, 771, 772, 774, 900, 901, 903,
};


static const char *read_parameter(const IrcMessage *message, guint index) {
	const char *value = index ? get_irc_parameter(message, index) : NULL;
	return value ? value : "";
}


char *describe_reply(const IrcMessage *message, ReplyKind *kind, const char **subject, const char **action) {
	int number = (int)g_ascii_strtoll(message->command, NULL, 10);
	*subject = NULL;
	*action = NULL;
	for (guint index = 0; index < G_N_ELEMENTS(HIDDEN_REPLIES); index++) {
		if (HIDDEN_REPLIES[index] == number) {
			*kind = REPLY_HIDDEN;
			return NULL;
		}
	}
	for (guint index = 0; index < G_N_ELEMENTS(REPLIES); index++) {
		if (REPLIES[index].number != number)
			continue;
		*kind = REPLIES[index].kind;
		*subject = REPLIES[index].subject ? get_irc_parameter(message, REPLIES[index].subject) : NULL;
		*action = REPLIES[index].action;
		return g_strdup_printf(REPLIES[index].text, read_parameter(message, REPLIES[index].first),
			read_parameter(message, REPLIES[index].second));
	}
	/* Other errors and notes keep the server's own sentence, which follows the names it is about */
	*kind = number >= 400 && number < 600 ? REPLY_PROBLEM : REPLY_NOTE;
	return g_strdup(get_last_irc_parameter(message));
}


/* The starts of the reasons a server gives when a connection dies, rather than the person choosing to go */
static const char *const LOST_CONNECTION[] = {
	"Ping timeout", "Read error", "Connection reset", "Remote host closed", "Connection closed", "Broken pipe",
	"Excess Flood", "Max SendQ", "EOF from client", "*.net *.split", NULL,
};

/* Reasons clients fill in by themselves, which say no more than that the person left */
static const char *const PLAIN_LEAVING[] = {"Client Quit", "Leaving", "Quit:", "Quit", NULL};


char *describe_leaving(const char *nick, const char *reason) {
	const char *said = reason ? reason : "";
	if (g_str_has_prefix(said, "Quit: "))
		said += strlen("Quit: ");
	for (int index = 0; LOST_CONNECTION[index]; index++) {
		if (g_ascii_strncasecmp(said, LOST_CONNECTION[index], strlen(LOST_CONNECTION[index])) == 0)
			return g_strdup_printf(TEXT_LOST_CONNECTION, nick);
	}
	if (g_ascii_strncasecmp(said, "Changing host", strlen("Changing host")) == 0)
		return g_strdup_printf(TEXT_RECONNECTED, nick);
	gboolean is_plain = !*said || g_ascii_strcasecmp(said, nick) == 0;
	for (int index = 0; !is_plain && PLAIN_LEAVING[index]; index++)
		is_plain = g_ascii_strcasecmp(said, PLAIN_LEAVING[index]) == 0;
	return is_plain ? g_strdup_printf(TEXT_LEFT, nick) : g_strdup_printf(TEXT_LEFT_SAYING, nick, said);
}


char *describe_kick(const char *kicker, const char *kicked, const char *reason) {
	gboolean says_more = reason && *reason && g_ascii_strcasecmp(reason, kicker) != 0
		&& g_ascii_strcasecmp(reason, kicked) != 0;
	return says_more ? g_strdup_printf(TEXT_KICKED, kicker, kicked, reason)
		: g_strdup_printf(TEXT_KICKED_PLAIN, kicker, kicked);
}
