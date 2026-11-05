#pragma once

#include "bouncer.h"

/* Hears the user a name and password belong to, or NULL when they do not match */
typedef void (*LoginChecked)(BouncerUser *user, gpointer data);

/* Checks a name and password on a worker thread, a few at a time, so hashing never holds up everyone else's chats.
 * A name that failed too often lately from one address, or a crowd of waiting checks, is refused without hashing.
 * Nothing is called once the cancellable is cancelled, so the caller may go away first */
void check_login(Bouncer *bouncer, const char *host, const char *name, const char *password,
	GCancellable *cancellable, LoginChecked checked, gpointer data);
