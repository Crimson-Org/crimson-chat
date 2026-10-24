#include <libsoup/soup.h>
#include <string.h>

#include "../lang/en.h"
#include "session.h"
#include "uploads.h"
#include "window.h"

#define UPLOAD_SECONDS 60
#define AVATARS_TOKEN "crimson-chat/AVATARS"
#define FILEHOST_TOKEN "soju.im/FILEHOST"
#define FILE_ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
/* Long names are cut in the middle in the line sent to the room, so the line keeps its address whole */
#define SHARED_NAME_CHARS 60
#define SHARED_NAME_END_CHARS 12
/* A server's own answer is shown when it is a short plain sentence, and not a page or a long dump */
#define LONGEST_SHOWN_ANSWER 200

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


/* Files only go over an encrypted address, as the IRC connection itself is */
char *find_file_upload_url(CrimsonNetwork *network) {
	IrcState *state = get_network_state(network);
	const char *value = state ? get_irc_support(state, FILEHOST_TOKEN) : NULL;
	return value && g_str_has_prefix(value, "https://") ? g_strdup(value) : NULL;
}


/* Hears how many of a file's bytes have gone out of how many */
typedef void (*UploadProgress)(goffset sent, goffset total, gpointer data);

/* One upload on its way, with the server it is for, where it goes and who waits for it */
typedef struct {
	CrimsonNetwork *network;
	GFile *file;
	char *address, *file_name, *content_type;
	const char *unreadable, *failed;
	goffset sent, total;
	UploadProgress progress;
	UploadDone done;
	gpointer data;
} Upload;


static void finish_upload(Upload *upload, const char *url, const char *problem) {
	upload->done(url, problem, upload->data);
	g_object_unref(upload->network);
	g_object_unref(upload->file);
	g_free(upload->address);
	g_free(upload->file_name);
	g_free(upload->content_type);
	g_free(upload);
}


/* The bouncer's own certificate counts where the user already trusted it for the IRC connection */
static gboolean accept_bouncer_certificate(SoupMessage *message, GTlsCertificate *certificate,
	GTlsCertificateFlags errors, Upload *upload) {
	const char *trusted = upload->network->session->record->trusted_certificate;
	g_autofree char *hash = hash_certificate(certificate);
	return trusted && hash && g_str_equal(trusted, hash);
}


static void count_sent(SoupMessage *message, guint chunk_bytes, Upload *upload) {
	upload->sent += chunk_bytes;
	if (upload->progress)
		upload->progress(upload->sent, upload->total, upload->data);
}


/* What to tell the user when the server refused an upload, in the server's own words where it gave some */
static const char *describe_refusal(Upload *upload, SoupMessage *message, char *said) {
	const char *type = message ? soup_message_headers_get_content_type(soup_message_get_response_headers(message),
		NULL) : NULL;
	if (said && g_strcmp0(type, "text/plain") == 0 && g_utf8_validate(g_strstrip(said), -1, NULL) && *said
		&& strlen(said) <= LONGEST_SHOWN_ANSWER && !strchr(said, '\n'))
		return said;
	guint status = message ? soup_message_get_status(message) : 0;
	if (status == SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE)
		return TEXT_FILE_TOO_LARGE;
	if (status == SOUP_STATUS_UNAUTHORIZED || status == SOUP_STATUS_FORBIDDEN)
		return TEXT_UPLOAD_LOGIN_REFUSED;
	return upload->failed;
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
		/* The address may be relative to the upload address */
		g_autoptr(GUri) base = g_uri_parse(upload->address, G_URI_FLAGS_NONE, NULL);
		g_autoptr(GUri) found = base && location ? g_uri_parse_relative(base, location, G_URI_FLAGS_NONE, NULL)
			: NULL;
		g_autofree char *url = found ? g_uri_to_string(found) : NULL;
		finish_upload(upload, url ? url : said, NULL);
		return;
	}
	finish_upload(upload, NULL, describe_refusal(upload, message, said));
}


static void set_login(SoupMessage *message, AccountRecord *record) {
	g_autofree char *login = g_strdup_printf("%s:%s", record->sasl_username ? record->sasl_username : "",
		record->sasl_password ? record->sasl_password : "");
	g_autofree char *encoded = g_base64_encode((const guchar *)login, strlen(login));
	g_autofree char *authorization = g_strdup_printf("Basic %s", encoded);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", authorization);
}


/* The name goes plain for older servers and encoded for names outside ASCII */
static void set_file_name(SoupMessage *message, const char *name) {
	g_autofree char *plain = g_str_to_ascii(name, NULL);
	g_strdelimit(plain, "\"\\", '_');
	g_autofree char *encoded = g_uri_escape_string(name, NULL, FALSE);
	g_autofree char *disposition = g_strdup_printf("attachment; filename=\"%s\"; filename*=UTF-8''%s", plain,
		encoded);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Content-Disposition", disposition);
}


static void send_stream(Upload *upload, GInputStream *stream) {
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_POST, upload->address);
	if (!message) {
		finish_upload(upload, NULL, upload->failed);
		return;
	}
	if (!uploads)
		uploads = soup_session_new_with_options("timeout", UPLOAD_SECONDS, NULL);
	set_login(message, upload->network->session->record);
	if (upload->file_name)
		set_file_name(message, upload->file_name);
	g_autofree char *type = upload->content_type ? g_content_type_get_mime_type(upload->content_type) : NULL;
	soup_message_set_request_body(message, type ? type : "application/octet-stream", stream, upload->total);
	/* The server can refuse a file by its size before any of it is sent, and its answer then arrives whole */
	soup_message_headers_set_expectations(soup_message_get_request_headers(message), SOUP_EXPECTATION_CONTINUE);
	g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_bouncer_certificate), upload);
	g_signal_connect(message, "wrote-body-data", G_CALLBACK(count_sent), upload);
	soup_session_send_and_read_async(uploads, message, G_PRIORITY_DEFAULT, NULL, finish_sending, upload);
}


static void finish_opening(GObject *source, GAsyncResult *result, gpointer data) {
	Upload *upload = data;
	g_autoptr(GFileInputStream) stream = g_file_read_finish(G_FILE(source), result, NULL);
	if (!stream) {
		finish_upload(upload, NULL, upload->unreadable);
		return;
	}
	send_stream(upload, G_INPUT_STREAM(stream));
}


static void finish_asking(GObject *source, GAsyncResult *result, gpointer data) {
	Upload *upload = data;
	g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, NULL);
	if (!info) {
		finish_upload(upload, NULL, upload->unreadable);
		return;
	}
	upload->total = g_file_info_get_size(info);
	upload->content_type = g_strdup(g_file_info_get_content_type(info));
	if (upload->progress)
		upload->progress(0, upload->total, upload->data);
	g_file_read_async(upload->file, G_PRIORITY_DEFAULT, NULL, finish_opening, upload);
}


/* The file is read as it is sent, so a large one never has to fit in memory */
static void start_upload(Upload *upload) {
	g_file_query_info_async(upload->file, FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, NULL,
		finish_asking, upload);
}


static Upload *create_upload(CrimsonNetwork *network, GFile *file, UploadDone done, gpointer data) {
	Upload *upload = g_new0(Upload, 1);
	upload->network = g_object_ref(network);
	upload->file = g_object_ref(file);
	upload->done = done;
	upload->data = data;
	return upload;
}


void upload_avatar(CrimsonNetwork *network, const char *target, GFile *file, UploadDone done, gpointer data) {
	Upload *upload = create_upload(network, file, done, data);
	g_autofree char *base = find_avatar_upload_url(network);
	g_autofree char *escaped = g_uri_escape_string(target, NULL, TRUE);
	upload->address = g_strdup_printf("%s?network=%s&target=%s", base, network->bouncer_id, escaped);
	upload->unreadable = TEXT_PICTURE_UNREADABLE;
	upload->failed = TEXT_UPLOAD_FAILED;
	start_upload(upload);
}


/* A file being shared, with the room it goes to and the card standing in for it meanwhile */
typedef struct {
	CrimsonRoom *room;
	CrimsonMessage *card;
} Sharing;


static void show_sharing_progress(goffset sent, goffset total, gpointer data) {
	Sharing *sharing = data;
	gboolean has_new_size = sharing->card->upload_size != total;
	sharing->card->upload_size = total;
	sharing->card->upload_progress = total > 0 ? CLAMP((double)sent / (double)total, 0, 1) : 0;
	/* The ring follows the progress by itself, so the card is only drawn again once its size is known */
	if (has_new_size)
		announce_message_change(sharing->card);
}


static char *shorten_shared_name(const char *name) {
	glong length = g_utf8_strlen(name, -1);
	if (length <= SHARED_NAME_CHARS)
		return g_strdup(name);
	g_autofree char *start = g_utf8_substring(name, 0, SHARED_NAME_CHARS - SHARED_NAME_END_CHARS - 1);
	g_autofree char *end = g_utf8_substring(name, length - SHARED_NAME_END_CHARS, length);
	return g_strconcat(start, "…", end, NULL);
}


static void finish_sharing(const char *url, const char *problem, gpointer data) {
	Sharing *sharing = data;
	CrimsonMessage *card = sharing->card;
	remove_room_message(sharing->room, card);
	if (url) {
		g_autofree char *name = shorten_shared_name(card->text);
		g_autofree char *size = g_format_size((guint64)card->upload_size);
		/* Other clients read the line too, so the size keeps a plain space where GLib puts a no-break one */
		g_auto(GStrv) size_parts = g_strsplit(size, "\u00a0", -1);
		g_autofree char *spaced = g_strjoinv(" ", size_parts);
		g_autofree char *line = g_strdup_printf(TEXT_SHARED_FILE, name, spaced, url);
		send_room_line(sharing->room, line);
	} else {
		show_toast(get_main_window(), problem ? problem : TEXT_FILE_UPLOAD_FAILED);
	}
	g_object_unref(sharing->card);
	g_object_unref(sharing->room);
	g_free(sharing);
}


void share_file(CrimsonRoom *room, GFile *file) {
	g_autofree char *address = find_file_upload_url(room->network);
	if (!address)
		return;
	Sharing *sharing = g_new0(Sharing, 1);
	sharing->room = g_object_ref(room);
	IrcState *state = get_network_state(room->network);
	g_autofree char *name = g_file_get_basename(file);
	sharing->card = create_message(MESSAGE_TEXT, state ? state->nick : NULL, name, g_get_real_time());
	sharing->card->is_own = TRUE;
	sharing->card->is_uploading = TRUE;
	sharing->card->arrival_time = g_get_monotonic_time();
	add_upload_message(room, sharing->card);
	Upload *upload = create_upload(room->network, file, finish_sharing, sharing);
	upload->address = g_steal_pointer(&address);
	upload->file_name = g_strdup(name);
	upload->unreadable = TEXT_FILE_UNREADABLE;
	upload->failed = TEXT_FILE_UPLOAD_FAILED;
	upload->progress = show_sharing_progress;
	start_upload(upload);
}
