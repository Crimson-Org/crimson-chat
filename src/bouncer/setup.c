#include <gio/gio.h>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "setup.h"

#define LONGEST_ANSWER 1024
#define SHORTEST_PASSWORD 8
#define HASH_ITERATIONS 210000
#define SALT_BYTES 16
#define HASH_BYTES 32
#define HASH_SCHEME "pbkdf2-sha256"
#define SERIAL_BYTES 16
#define CERTIFICATE_YEARS 10
#define SECONDS_PER_YEAR (365 * 24 * 60 * 60)


char *ask_line(const char *question, gboolean is_secret) {
	g_print("%s", question);
	struct termios saved, hidden;
	gboolean is_hidden = is_secret && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
	if (is_hidden) {
		hidden = saved;
		hidden.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
	}
	char answer[LONGEST_ANSWER];
	char *line = fgets(answer, sizeof answer, stdin);
	if (is_hidden) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		g_print("\n");
	}
	if (!line)
		return NULL;
	char *result = g_strdup(g_strchomp(answer));
	memset(answer, 0, sizeof answer);
	return result;
}


char *ask_new_password(void) {
	while (TRUE) {
		g_autofree char *password = ask_line("Password: ", TRUE);
		if (!password)
			return NULL;
		if (strlen(password) < SHORTEST_PASSWORD) {
			g_print("Use at least %d characters.\n", SHORTEST_PASSWORD);
			continue;
		}
		g_autofree char *again = ask_line("Repeat the password: ", TRUE);
		if (!again)
			return NULL;
		if (g_str_equal(password, again))
			return g_steal_pointer(&password);
		g_print("The passwords differ, try again.\n");
	}
}


static gboolean derive_key(const char *password, const guchar *salt, gsize salt_length, guint iterations,
	guchar *output) {
	gnutls_datum_t key = {(unsigned char *)password, (unsigned int)strlen(password)};
	gnutls_datum_t salt_datum = {(unsigned char *)salt, (unsigned int)salt_length};
	return gnutls_pbkdf2(GNUTLS_MAC_SHA256, &key, &salt_datum, iterations, output, HASH_BYTES) == 0;
}


char *hash_password(const char *password) {
	guchar salt[SALT_BYTES], hash[HASH_BYTES];
	if (gnutls_rnd(GNUTLS_RND_KEY, salt, sizeof salt) != 0 || !derive_key(password, salt, sizeof salt,
		HASH_ITERATIONS, hash))
		return NULL;
	g_autofree char *salt_text = g_base64_encode(salt, sizeof salt);
	g_autofree char *hash_text = g_base64_encode(hash, sizeof hash);
	return g_strdup_printf("%s$%d$%s$%s", HASH_SCHEME, HASH_ITERATIONS, salt_text, hash_text);
}


gboolean check_password(const char *password, const char *stored) {
	g_auto(GStrv) parts = g_strsplit(stored ? stored : "", "$", 4);
	if (g_strv_length(parts) != 4 || !g_str_equal(parts[0], HASH_SCHEME))
		return FALSE;
	guint iterations = (guint)g_ascii_strtoull(parts[1], NULL, 10);
	gsize salt_length = 0, expected_length = 0;
	g_autofree guchar *salt = g_base64_decode(parts[2], &salt_length);
	g_autofree guchar *expected = g_base64_decode(parts[3], &expected_length);
	guchar hash[HASH_BYTES];
	if (!iterations || expected_length != HASH_BYTES || !derive_key(password, salt, salt_length, iterations, hash))
		return FALSE;
	/* Comparing every byte takes the same time whether or not an early byte differs */
	guchar difference = 0;
	for (gsize index = 0; index < HASH_BYTES; index++)
		difference |= hash[index] ^ expected[index];
	return difference == 0;
}


static gboolean report_tls_failure(int status, GError **error) {
	if (status >= 0)
		return TRUE;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", gnutls_strerror(status));
	return FALSE;
}


static gboolean write_datum(const char *path, gnutls_datum_t *datum, int mode, GError **error) {
	gboolean is_written = g_file_set_contents_full(path, (const char *)datum->data, datum->size,
		G_FILE_SET_CONTENTS_CONSISTENT, mode, error);
	gnutls_free(datum->data);
	return is_written;
}


static int fill_certificate(gnutls_x509_crt_t certificate, gnutls_x509_privkey_t key, const char *host) {
	unsigned char serial[SERIAL_BYTES];
	int status = gnutls_rnd(GNUTLS_RND_NONCE, serial, sizeof serial);
	/* A serial number must be positive, which a clear top bit guarantees */
	serial[0] &= 0x7f;
	time_t now = time(NULL);
	if (status >= 0)
		status = gnutls_x509_crt_set_version(certificate, 3);
	if (status >= 0)
		status = gnutls_x509_crt_set_serial(certificate, serial, sizeof serial);
	if (status >= 0)
		status = gnutls_x509_crt_set_activation_time(certificate, now);
	if (status >= 0)
		status = gnutls_x509_crt_set_expiration_time(certificate, now + (time_t)CERTIFICATE_YEARS * SECONDS_PER_YEAR);
	if (status >= 0)
		status = gnutls_x509_crt_set_dn_by_oid(certificate, GNUTLS_OID_X520_COMMON_NAME, 0, host,
			(unsigned int)strlen(host));
	if (status >= 0)
		status = gnutls_x509_crt_set_subject_alt_name(certificate, GNUTLS_SAN_DNSNAME, host,
			(unsigned int)strlen(host), GNUTLS_FSAN_SET);
	if (status >= 0)
		status = gnutls_x509_crt_set_key(certificate, key);
	if (status >= 0)
		status = gnutls_x509_crt_set_key_usage(certificate, GNUTLS_KEY_DIGITAL_SIGNATURE);
	if (status >= 0)
		status = gnutls_x509_crt_set_key_purpose_oid(certificate, GNUTLS_KP_TLS_WWW_SERVER, 0);
	if (status >= 0)
		status = gnutls_x509_crt_sign2(certificate, certificate, key, GNUTLS_DIG_SHA256, 0);
	return status;
}


gboolean create_certificate(const char *host, const char *certificate_path, const char *key_path, GError **error) {
	gnutls_x509_privkey_t key = NULL;
	gnutls_x509_crt_t certificate = NULL;
	gnutls_datum_t key_text = {0}, certificate_text = {0};
	int status = gnutls_x509_privkey_init(&key);
	if (status >= 0)
		status = gnutls_x509_privkey_generate(key, GNUTLS_PK_ECDSA,
			GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1), 0);
	if (status >= 0)
		status = gnutls_x509_crt_init(&certificate);
	if (status >= 0)
		status = fill_certificate(certificate, key, host);
	if (status >= 0)
		status = gnutls_x509_privkey_export2_pkcs8(key, GNUTLS_X509_FMT_PEM, NULL, GNUTLS_PKCS_PLAIN, &key_text);
	if (status >= 0)
		status = gnutls_x509_crt_export2(certificate, GNUTLS_X509_FMT_PEM, &certificate_text);
	if (certificate)
		gnutls_x509_crt_deinit(certificate);
	if (key)
		gnutls_x509_privkey_deinit(key);
	if (!report_tls_failure(status, error))
		return FALSE;
	if (!write_datum(key_path, &key_text, 0600, error)) {
		gnutls_free(certificate_text.data);
		return FALSE;
	}
	return write_datum(certificate_path, &certificate_text, 0644, error);
}
