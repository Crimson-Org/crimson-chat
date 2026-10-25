#include <string.h>

#include "../lang/en.h"
#include "modes.h"

/* What a member status does, by the symbol it shows before a nick */
static const struct {
	char symbol;
	const char *given, *taken;
} ROLE_CHANGES[] = {
	{'~', TEXT_MADE_OWNER, TEXT_UNMADE_OWNER},
	{'&', TEXT_MADE_ADMIN, TEXT_UNMADE_ADMIN},
	{'@', TEXT_MADE_MODERATOR, TEXT_UNMADE_MODERATOR},
	{'%', TEXT_MADE_HELPER, TEXT_UNMADE_HELPER},
	{'+', TEXT_MADE_SPEAKER, TEXT_UNMADE_SPEAKER},
};

/* The settings every server shares, with what turning them on and off does, where %s is their value */
static const struct {
	char letter;
	const char *added, *removed;
} SETTING_CHANGES[] = {
	{'b', TEXT_BANNED, TEXT_UNBANNED},
	{'e', TEXT_EXEMPTED, TEXT_UNEXEMPTED},
	{'I', TEXT_INVITE_LISTED, TEXT_INVITE_UNLISTED},
	{'k', TEXT_SET_PASSWORD, TEXT_REMOVED_PASSWORD},
	{'l', TEXT_SET_LIMIT, TEXT_REMOVED_LIMIT},
	{'i', TEXT_MADE_INVITE_ONLY, TEXT_UNMADE_INVITE_ONLY},
	{'m', TEXT_MUTED_ROOM, TEXT_UNMUTED_ROOM},
	{'n', TEXT_BLOCKED_OUTSIDE, TEXT_ALLOWED_OUTSIDE},
	{'t', TEXT_LOCKED_TOPIC, TEXT_UNLOCKED_TOPIC},
	{'s', TEXT_HID_ROOM, TEXT_SHOWED_ROOM},
	{'p', TEXT_MADE_PRIVATE, TEXT_MADE_PUBLIC},
};


/* Extended bans name what they match by a letter after $ or ~, such as $a:name for an account */
static const struct {
	char letter;
	const char *text;
} EXTENDED_BANS[] = {
	{'a', TEXT_ACCOUNT_MASK},
	{'r', TEXT_REAL_NAME_MASK},
	{'j', TEXT_ROOM_MEMBERS_MASK},
};


char *describe_mask(const char *mask) {
	if (!mask)
		return g_strdup("");
	/* A ban that sends people to another room ends in $ and that room, which says nothing about who it covers */
	const char *forward = strchr(mask + 1, '$');
	g_autofree char *covered = forward ? g_strndup(mask, (gsize)(forward - mask)) : g_strdup(mask);
	gboolean is_extended = (*covered == '$' || *covered == '~') && covered[1] && covered[2] == ':';
	for (guint index = 0; is_extended && index < G_N_ELEMENTS(EXTENDED_BANS); index++) {
		if (EXTENDED_BANS[index].letter == covered[1])
			return g_strdup_printf(EXTENDED_BANS[index].text, covered + 3);
	}
	if (g_str_has_suffix(covered, "!*@*"))
		return g_strndup(covered, strlen(covered) - strlen("!*@*"));
	if (g_str_has_prefix(covered, "*!*@"))
		return g_strdup_printf(TEXT_EVERYONE_FROM, covered + strlen("*!*@"));
	return g_steal_pointer(&covered);
}


static char *describe_role_change(IrcState *state, const IrcModeChange *change) {
	const char *letter = strchr(state->prefix_modes, change->letter);
	char symbol = letter ? state->prefix_symbols[letter - state->prefix_modes] : '\0';
	for (guint index = 0; symbol && index < G_N_ELEMENTS(ROLE_CHANGES); index++) {
		if (ROLE_CHANGES[index].symbol == symbol)
			return g_strdup_printf(change->is_adding ? ROLE_CHANGES[index].given : ROLE_CHANGES[index].taken,
				change->parameter ? change->parameter : "");
	}
	return NULL;
}


static char *describe_change(IrcState *state, const IrcModeChange *change) {
	if (strchr(state->prefix_modes, change->letter))
		return describe_role_change(state, change);
	for (guint index = 0; index < G_N_ELEMENTS(SETTING_CHANGES); index++) {
		if (SETTING_CHANGES[index].letter != change->letter)
			continue;
		g_autofree char *value = strchr(state->list_modes, change->letter) ? describe_mask(change->parameter)
			: g_strdup(change->parameter ? change->parameter : "");
		return g_strdup_printf(change->is_adding ? SETTING_CHANGES[index].added : SETTING_CHANGES[index].removed,
			value);
	}
	return NULL;
}


/* Joins the changes the way a sentence lists them, as in "a, b and c" */
static char *join_changes(GPtrArray *phrases) {
	GString *text = g_string_new(NULL);
	for (guint index = 0; index < phrases->len; index++) {
		if (index)
			g_string_append(text, index + 1 == phrases->len ? TEXT_LAST_CHANGE_JOINER : TEXT_CHANGE_JOINER);
		g_string_append(text, g_ptr_array_index(phrases, index));
	}
	return g_string_free(text, FALSE);
}


char *describe_room_modes(IrcState *state, const IrcMessage *message, guint first) {
	g_autoptr(GPtrArray) changes = read_irc_mode_changes(state, message, first);
	g_autoptr(GPtrArray) phrases = g_ptr_array_new_with_free_func(g_free);
	gboolean has_unknown = FALSE;
	for (guint index = 0; index < changes->len; index++) {
		char *phrase = describe_change(state, g_ptr_array_index(changes, index));
		if (phrase)
			g_ptr_array_add(phrases, phrase);
		else
			has_unknown = TRUE;
	}
	/* Settings particular to one kind of server are summed up once rather than named letter by letter */
	if (has_unknown)
		g_ptr_array_add(phrases, g_strdup(TEXT_CHANGED_SETTINGS));
	if (!phrases->len)
		return NULL;
	g_autofree char *sender = message->source && strchr(message->source, '!') ? get_source_nick(message->source)
		: g_strdup(TEXT_SERVER_SUBJECT);
	g_autofree char *sentence = join_changes(phrases);
	return g_strdup_printf(TEXT_ROOM_CHANGED, sender, sentence);
}
