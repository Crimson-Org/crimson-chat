#pragma once

#include <glib.h>

/* How a network compares nicks and channel names, from its CASEMAPPING token */
typedef enum {
	CASEMAP_RFC1459,
	CASEMAP_STRICT_RFC1459,
	CASEMAP_ASCII,
} IrcCasemap;

IrcCasemap parse_irc_casemap(const char *name);
/* The lowercase form two names share when the network treats them as the same */
char *fold_irc_name(IrcCasemap casemap, const char *name);
gboolean are_irc_names_equal(IrcCasemap casemap, const char *first, const char *second);
