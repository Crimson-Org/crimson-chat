#pragma once

#include <glib.h>

/* Asks on the terminal, hiding what is typed when it is a password, and returns NULL when input ends */
char *ask_line(const char *question, gboolean is_secret);
/* Asks for a password twice until both match and it is long enough */
char *ask_new_password(void);

/* A salted PBKDF2 hash in the form pbkdf2-sha256$iterations$salt$hash */
char *hash_password(const char *password);
gboolean check_password(const char *password, const char *stored);

/* Writes a self-signed certificate and its key for the host name, both as PEM */
gboolean create_certificate(const char *host, const char *certificate_path, const char *key_path, GError **error);
