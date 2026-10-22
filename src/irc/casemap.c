#include "casemap.h"


IrcCasemap parse_irc_casemap(const char *name) {
	if (g_strcmp0(name, "ascii") == 0)
		return CASEMAP_ASCII;
	if (g_strcmp0(name, "strict-rfc1459") == 0)
		return CASEMAP_STRICT_RFC1459;
	return CASEMAP_RFC1459;
}


static char fold_character(IrcCasemap casemap, char character) {
	if (character >= 'A' && character <= 'Z')
		return (char)(character + ('a' - 'A'));
	if (casemap == CASEMAP_ASCII)
		return character;
	/* The Scandinavian roots of IRC made these brackets the capitals of the braces */
	switch (character) {
	case '[': return '{';
	case ']': return '}';
	case '\\': return '|';
	case '^': return casemap == CASEMAP_RFC1459 ? '~' : '^';
	default: return character;
	}
}


char *fold_irc_name(IrcCasemap casemap, const char *name) {
	char *folded = g_strdup(name ? name : "");
	for (char *character = folded; *character; character++)
		*character = fold_character(casemap, *character);
	return folded;
}


gboolean are_irc_names_equal(IrcCasemap casemap, const char *first, const char *second) {
	if (!first || !second)
		return first == second;
	for (; *first && *second; first++, second++) {
		if (fold_character(casemap, *first) != fold_character(casemap, *second))
			return FALSE;
	}
	return *first == *second;
}
