#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include "secret.h"


char *create_secret_token(gsize bytes) {
	g_autofree guint8 *random = g_malloc(bytes);
	if (gnutls_rnd(GNUTLS_RND_KEY, random, bytes) < 0)
		g_error("The system gave no random numbers for a secret token");
	GString *token = g_string_sized_new(bytes * 2);
	for (gsize index = 0; index < bytes; index++)
		g_string_append_printf(token, "%02x", random[index]);
	return g_string_free(token, FALSE);
}
