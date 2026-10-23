#include <libsoup/soup.h>
#include <string.h>

#include "../lang/en.h"
#include "session.h"
#include "uploads.h"

#define UPLOAD_SECONDS 60
#define AVATARS_TOKEN "crimson-chat/AVATARS"

static SoupSession *uploads;


char *find_avatar_upload_url(CrimsonNetwork *network) {
	IrcState *state = get_network_state(network);
	const char *value = state && network->bouncer_id ? get_irc_support(state, AVATARS_TOKEN) : NULL;
	if (!value || !*value)
		return NULL;
	if (g_str_has_prefix(value, "https://"))
		return g_strdup(value);
	/* A bouncer without a public address names its website's port, on the address the app reaches it by */
	return g_strdup_printf("https://%s:%s/avatar", network->session->record->host, value);
}


/* One upload on its way, with the server it is for and who waits for it */
typedef struct {
	CrimsonNetwork *network;
	char *target;
	UploadDone done;
	gpointer data;
} Upload;


static void finish_upload(Upload *upload, const char *url, const char *problem) {
	upload->done(url, problem, upload->data);
	g_object_unref(upload->network);
	g_free(upload->target);
	g_free(upload);
}


/* The bouncer's own certificate counts where the user already trusted it for the IRC connection */
static gboolean accept_bouncer_certificate(SoupMessage *message, GTlsCertificate *certificate,
	GTlsCertificateFlags errors, Upload *upload) {
	const char *trusted = upload->network->session->record->trusted_certificate;
	g_autofree char *hash = hash_certificate(certificate);
	return trusted && hash && g_str_equal(trusted, hash);
}


static void finish_sending(GObject *source, GAsyncResult *result, gpointer data) {
	Upload *upload = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) answer = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	guint status = message ? soup_message_get_status(message) : 0;
	gsize length = 0;
	const char *text = answer ? g_bytes_get_data(answer, &length) : NULL;
	g_autofree char *said = text ? g_strndup(text, length) : NULL;
	if (status == SOUP_STATUS_CREATED) {
		const char *location = soup_message_headers_get_one(soup_message_get_response_headers(message), "Location");
		finish_upload(upload, location ? location : said, NULL);
		return;
	}
	finish_upload(upload, NULL, said && *said ? said : error ? error->message : TEXT_UPLOAD_FAILED);
}


static void send_picture(Upload *upload, GBytes *bytes, const char *path) {
	if (!uploads)
		uploads = soup_session_new_with_options("timeout", UPLOAD_SECONDS, NULL);
	g_autofree char *base = find_avatar_upload_url(upload->network);
	g_autofree char *target = g_uri_escape_string(upload->target, NULL, TRUE);
	g_autofree char *address = g_strdup_printf("%s?network=%s&target=%s", base, upload->network->bouncer_id, target);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_POST, address);
	if (!message) {
		finish_upload(upload, NULL, TEXT_UPLOAD_FAILED);
		return;
	}
	AccountRecord *record = upload->network->session->record;
	g_autofree char *login = g_strdup_printf("%s:%s", record->sasl_username ? record->sasl_username : "",
		record->sasl_password ? record->sasl_password : "");
	g_autofree char *encoded = g_base64_encode((const guchar *)login, strlen(login));
	g_autofree char *authorization = g_strdup_printf("Basic %s", encoded);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", authorization);
	gsize length = 0;
	const guchar *data = g_bytes_get_data(bytes, &length);
	g_autofree char *guessed = g_content_type_guess(path, data, length, NULL);
	g_autofree char *type = g_content_type_get_mime_type(guessed);
	soup_message_set_request_body_from_bytes(message, type ? type : "application/octet-stream", bytes);
	g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_bouncer_certificate), upload);
	soup_session_send_and_read_async(uploads, message, G_PRIORITY_DEFAULT, NULL, finish_sending, upload);
}


static void finish_loading(GObject *source, GAsyncResult *result, gpointer data) {
	Upload *upload = data;
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_load_contents_finish(G_FILE(source), result, &contents, &length, NULL, NULL)) {
		finish_upload(upload, NULL, TEXT_PICTURE_UNREADABLE);
		return;
	}
	g_autoptr(GBytes) bytes = g_bytes_new_take(contents, length);
	g_autofree char *path = g_file_get_path(G_FILE(source));
	send_picture(upload, bytes, path);
}


void upload_avatar(CrimsonNetwork *network, const char *target, GFile *file, UploadDone done, gpointer data) {
	Upload *upload = g_new0(Upload, 1);
	upload->network = g_object_ref(network);
	upload->target = g_strdup(target);
	upload->done = done;
	upload->data = data;
	g_file_load_contents_async(file, NULL, finish_loading, upload);
}
