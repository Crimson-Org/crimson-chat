#pragma once

#include <glib.h>

/* Bytes in a token that signs someone in or lets them join, too many to ever guess */
#define SIGN_IN_TOKEN_BYTES 32

/* A token of this many random bytes as hex, from the system's cryptographic source, for anything that must not be
 * guessed, such as sign ins, invites and the names of shared files */
char *create_secret_token(gsize bytes);
