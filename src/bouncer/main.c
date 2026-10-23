/* Start the Crimson Chat bouncer, which keeps IRC networks connected for its users
 *
 */

#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bouncer.h"
#include "setup.h"

#define CONFIG_FILE "bouncer.ini"
#define DATABASE_FILE "bouncer.db"
#define CERTIFICATE_FILE "certificate.pem"
#define KEY_FILE "key.pem"
#define GROUP "bouncer"
#define DEFAULT_LISTEN "6697"
#define DEFAULT_WEB "8443"
#define FAILURE_STATUS 1
/* Uploaded pictures are on unless the owner turns them off, as square PNGs of 256 pixels from at most 2.5 MB */
#define DEFAULT_AVATAR_FORMAT "png"
#define DEFAULT_AVATAR_QUALITY 90
#define DEFAULT_AVATAR_SIZE 256
#define DEFAULT_AVATAR_LARGEST_MB 2.5
#define AVATAR_FOLDER "avatars"

typedef struct {
	char *listen, *web, *new_admin;
	gboolean changes_password, shows_version;
} Options;


static GLogWriterOutput write_log(GLogLevelFlags level, const GLogField *fields, gsize count, gpointer data) {
	const char *text = NULL;
	for (gsize index = 0; index < count; index++) {
		if (g_str_equal(fields[index].key, "MESSAGE"))
			text = fields[index].value;
	}
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree char *moment = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
	const char *kind = level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING) ? " warning:" : "";
	if (level & G_LOG_LEVEL_DEBUG)
		return G_LOG_WRITER_HANDLED;
	fprintf(stderr, "%s%s %s\n", moment, kind, text ? text : "");
	return G_LOG_WRITER_HANDLED;
}


static char *build_path(const char *base, const char *name) {
	g_autofree char *folder = g_build_filename(base, BOUNCER_NAME, NULL);
	g_mkdir_with_parents(folder, 0700);
	return g_build_filename(folder, name, NULL);
}


static gboolean read_options(int *argument_count, char ***arguments, Options *options) {
	const GOptionEntry ENTRIES[] = {
		{"listen", 'l', 0, G_OPTION_ARG_STRING, &options->listen, "Listen on a port, or an address and port",
			"ADDRESS"},
		{"web", 'w', 0, G_OPTION_ARG_STRING, &options->web, "Serve the website on a port, or an address and port",
			"ADDRESS"},
		{"password", 'p', 0, G_OPTION_ARG_NONE, &options->changes_password, "Change a user's password", NULL},
		{"admin", 'a', 0, G_OPTION_ARG_STRING, &options->new_admin, "Make a user an admin, who can invite people",
			"NAME"},
		{"version", 'v', 0, G_OPTION_ARG_NONE, &options->shows_version, "Print the version", NULL},
		G_OPTION_ENTRY_NULL,
	};
	g_autoptr(GOptionContext) context = g_option_context_new(NULL);
	g_option_context_set_summary(context, "Keeps IRC networks connected for Crimson Chat and other modern clients.");
	g_option_context_add_main_entries(context, ENTRIES, NULL);
	g_autoptr(GError) error = NULL;
	if (g_option_context_parse(context, argument_count, arguments, &error))
		return TRUE;
	g_printerr("%s\n", error->message);
	return FALSE;
}


/* Answers can also come piped in for a scripted install, and input that ends early stops the setup */
static gboolean report_missing_answers(void) {
	g_printerr("\nRun %s once in a terminal, or pipe in the answers, to set up its admin account.\n", BOUNCER_NAME);
	return FALSE;
}


/* The first launch asks for the admin account that clients log in with */
static gboolean create_admin(Database *database) {
	g_print("Welcome to the Crimson Chat bouncer. Choose the admin account clients log in with.\n");
	g_autofree char *name = NULL;
	while (!name || !*name || strpbrk(name, " :/@!")) {
		g_free(name);
		name = ask_line("Username: ", FALSE);
		if (!name)
			return report_missing_answers();
	}
	g_autofree char *password = ask_new_password();
	if (!password)
		return report_missing_answers();
	g_autofree char *hash = hash_password(password);
	memset(password, 0, strlen(password));
	return hash && add_user(database, name, hash, TRUE) > 0;
}


static gboolean change_password(Database *database) {
	g_autofree char *name = ask_line("Username: ", FALSE);
	g_autofree char *password = name ? ask_new_password() : NULL;
	if (!password)
		return FALSE;
	g_autofree char *hash = hash_password(password);
	memset(password, 0, strlen(password));
	if (hash && change_user_password(database, name, hash)) {
		g_print("The password of %s changed.\n", name);
		return TRUE;
	}
	g_printerr("There is no user called %s.\n", name);
	return FALSE;
}


/* A running bouncer reads its users when it starts, so it takes the new admin once restarted */
static gboolean make_admin(Database *database, const char *name) {
	if (make_user_admin(database, name)) {
		g_print("%s is now an admin, from the next start of the bouncer.\n", name);
		return TRUE;
	}
	g_printerr("There is no user called %s.\n", name);
	return FALSE;
}


static GTlsCertificate *load_certificate(GKeyFile *config) {
	g_autofree char *certificate_path = g_key_file_get_string(config, GROUP, "certificate", NULL);
	g_autofree char *key_path = g_key_file_get_string(config, GROUP, "key", NULL);
	g_autoptr(GError) error = NULL;
	if (!certificate_path) {
		certificate_path = build_path(g_get_user_config_dir(), CERTIFICATE_FILE);
		key_path = build_path(g_get_user_config_dir(), KEY_FILE);
		if (!g_file_test(certificate_path, G_FILE_TEST_EXISTS)
			&& !create_certificate(g_get_host_name(), certificate_path, key_path, &error)) {
			g_printerr("Could not create a certificate: %s\n", error->message);
			return NULL;
		}
	}
	GTlsCertificate *certificate = g_tls_certificate_new_from_files(certificate_path, key_path, &error);
	if (!certificate)
		g_printerr("Could not read the certificate %s: %s\n", certificate_path, error->message);
	return certificate;
}


static GKeyFile *load_config(void) {
	g_autofree char *path = build_path(g_get_user_config_dir(), CONFIG_FILE);
	GKeyFile *config = g_key_file_new();
	if (g_key_file_load_from_file(config, path, G_KEY_FILE_KEEP_COMMENTS, NULL))
		return config;
	g_key_file_set_string(config, GROUP, "listen", DEFAULT_LISTEN);
	g_key_file_set_comment(config, GROUP, "listen", " A port, or an address and port such as 127.0.0.1:6697",
		NULL);
	g_key_file_set_string(config, GROUP, "web", DEFAULT_WEB);
	g_key_file_set_comment(config, GROUP, "web", " The website for signing in and invites, on a port browsers allow", NULL);
	g_key_file_set_string(config, GROUP, "name", g_get_host_name());
	g_key_file_set_comment(config, GROUP, "name", " The name invites greet people with", NULL);
	g_key_file_set_boolean(config, GROUP, "avatars", TRUE);
	g_key_file_set_comment(config, GROUP, "avatars", " Whether people can upload pictures for themselves and their "
		"rooms, which the bouncer serves; avatar-format (png or jpeg), avatar-quality (1 to 100), avatar-size "
		"(pixels), avatar-largest-mb and public-url (such as https://chat.example.com:8443) shape them", NULL);
	g_key_file_save_to_file(config, path, NULL);
	return config;
}


static void read_avatar_settings(GKeyFile *config, AvatarSettings *settings) {
	g_autoptr(GError) error = NULL;
	gboolean is_enabled = g_key_file_get_boolean(config, GROUP, "avatars", &error);
	settings->is_enabled = error ? TRUE : is_enabled;
	settings->public_url = g_key_file_get_string(config, GROUP, "public-url", NULL);
	if (settings->public_url) {
		g_strstrip(settings->public_url);
		/* A trailing slash would double up with the paths added after it */
		while (g_str_has_suffix(settings->public_url, "/"))
			settings->public_url[strlen(settings->public_url) - 1] = '\0';
		if (!*settings->public_url)
			g_clear_pointer(&settings->public_url, g_free);
	}
	g_autofree char *format = g_key_file_get_string(config, GROUP, "avatar-format", NULL);
	if (format)
		g_strstrip(format);
	settings->format = g_strdup(format && (g_ascii_strcasecmp(format, "jpeg") == 0 || g_ascii_strcasecmp(format,
		"jpg") == 0) ? "jpeg" : DEFAULT_AVATAR_FORMAT);
	int quality = g_key_file_get_integer(config, GROUP, "avatar-quality", NULL);
	settings->quality = quality > 0 ? CLAMP(quality, 1, 100) : DEFAULT_AVATAR_QUALITY;
	int size = g_key_file_get_integer(config, GROUP, "avatar-size", NULL);
	settings->size = size > 0 ? CLAMP(size, 32, 1024) : DEFAULT_AVATAR_SIZE;
	double largest = g_key_file_get_double(config, GROUP, "avatar-largest-mb", NULL);
	settings->largest_bytes = (gsize)((largest > 0 ? largest : DEFAULT_AVATAR_LARGEST_MB) * 1024 * 1024);
	settings->folder = build_path(g_get_user_data_dir(), AVATAR_FOLDER);
}


static gboolean stop_running(gpointer loop) {
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}


static int run_bouncer(Options *options) {
	g_autofree char *database_path = build_path(g_get_user_data_dir(), DATABASE_FILE);
	g_autoptr(GError) error = NULL;
	Database *database = open_database(database_path, &error);
	if (!database) {
		g_printerr("Could not open %s: %s\n", database_path, error->message);
		return FAILURE_STATUS;
	}
	g_autoptr(GPtrArray) users = load_users(database);
	if (options->changes_password)
		return change_password(database) ? EXIT_SUCCESS : FAILURE_STATUS;
	if (options->new_admin)
		return make_admin(database, options->new_admin) ? EXIT_SUCCESS : FAILURE_STATUS;
	if (!users->len && !create_admin(database))
		return FAILURE_STATUS;
	g_autoptr(GKeyFile) config = load_config();
	g_autoptr(GTlsCertificate) certificate = load_certificate(config);
	if (!certificate)
		return FAILURE_STATUS;
	g_autofree char *configured = g_key_file_get_string(config, GROUP, "listen", NULL);
	const char *listen = options->listen ? options->listen : configured ? configured : DEFAULT_LISTEN;
	g_autofree char *configured_web = g_key_file_get_string(config, GROUP, "web", NULL);
	const char *web = options->web ? options->web : configured_web ? configured_web : DEFAULT_WEB;
	g_autofree char *name = g_key_file_get_string(config, GROUP, "name", NULL);
	Bouncer *bouncer = create_bouncer(database, certificate, name ? name : g_get_host_name());
	read_avatar_settings(config, &bouncer->avatars);
	if (!listen_bouncer(bouncer, listen, web, &error)) {
		g_printerr("Could not listen on %s and %s: %s\n", listen, web, error->message);
		return FAILURE_STATUS;
	}
	g_autofree char *hash = hash_certificate(certificate);
	g_autofree char *fingerprint = format_certificate_hash(hash);
	g_message("Listening on %s with TLS", listen);
	g_message("Website on https port %s", bouncer->web_port);
	g_message("Certificate SHA-256 %s", fingerprint);
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, stop_running, loop);
	g_unix_signal_add(SIGTERM, stop_running, loop);
	g_main_loop_run(loop);
	g_message("Stopping");
	return EXIT_SUCCESS;
}


int main(int argument_count, char **arguments) {
	g_log_set_writer_func(write_log, NULL, NULL);
	Options options = {0};
	if (!read_options(&argument_count, &arguments, &options))
		return FAILURE_STATUS;
	if (options.shows_version) {
		g_print("%s %s\n", BOUNCER_NAME, APPLICATION_VERSION);
		return EXIT_SUCCESS;
	}
	int status = run_bouncer(&options);
	g_free(options.listen);
	g_free(options.web);
	g_free(options.new_admin);
	return status;
}
