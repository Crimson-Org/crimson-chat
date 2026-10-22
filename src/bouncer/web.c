#include <string.h>

#include "bouncer.h"
#include "setup.h"

#define LARGEST_REQUEST_BYTES 65536
#define READ_CHUNK_BYTES 4096
#define REQUEST_SECONDS 15
#define SESSION_SECONDS (7 * 24 * 60 * 60)
#define FAILED_LOGIN_DELAY_MILLISECONDS 1000
#define DEFAULT_HTTPS_PORT "443"
#define SHORTEST_PASSWORD 8
#define COOKIE_NAME "crimson_session"
#define DOWNLOAD_ADDRESS "https://github.com/Crimson-Org/crimson-chat/releases/latest"
#define APPLICATION_LINK "crimson-chat://bouncer"

static const char STYLE[] =
	":root{color-scheme:light dark;--accent:#c8102e;--bg:#f4eff0;--card:#fff;--fg:#1d1d20;--dim:#6b6b73;"
	"--line:rgba(0,0,0,.1)}"
	"@media (prefers-color-scheme:dark){:root{--bg:#0d0b0c;--card:#1e1c1f;--fg:#f2f2f4;--dim:#a3a0a8;"
	"--line:rgba(255,255,255,.1)}}"
	"*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
	"background:radial-gradient(ellipse at 20% 0%,rgba(200,16,46,.18),transparent 60%),var(--bg);color:var(--fg);"
	"font:16px/1.5 system-ui,sans-serif;padding:24px}"
	"main{width:100%;max-width:520px;background:var(--card);border-radius:20px;padding:32px;"
	"box-shadow:0 10px 40px rgba(0,0,0,.25)}"
	"h1{margin:0 0 8px;font-size:28px}h2{font-size:20px;margin:28px 0 12px}p{margin:0 0 16px;color:var(--dim)}"
	".logo{width:56px;height:56px;border-radius:16px;background:linear-gradient(#ff4057,#8e0a1d);display:flex;"
	"align-items:center;justify-content:center;color:#fff;font-weight:800;font-size:30px;margin-bottom:20px}"
	"label{display:block;font-weight:600;margin:14px 0 6px}"
	"input,select{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);"
	"background:transparent;color:inherit;font:inherit}"
	"button,.button{display:inline-block;margin-top:20px;padding:12px 22px;border:0;border-radius:999px;"
	"background:var(--accent);color:#fff;font:inherit;font-weight:700;text-decoration:none;cursor:pointer}"
	".quiet{background:transparent;color:var(--dim);border:1px solid var(--line)}"
	".small{margin-top:0;padding:6px 14px}"
	"dl{display:grid;grid-template-columns:auto 1fr;gap:8px 16px;margin:0;padding:16px;border-radius:14px;"
	"background:rgba(127,127,127,.1)}dt{color:var(--dim)}dd{margin:0;font-weight:600;word-break:break-all}"
	"code{font-family:ui-monospace,monospace;font-size:16px}"
	".invite{display:flex;gap:12px;align-items:center;justify-content:space-between;padding:12px 0;"
	"border-top:1px solid var(--line)}.invite code{word-break:break-all}"
	".error{color:var(--accent);font-weight:600}.row{display:flex;gap:12px;flex-wrap:wrap}";

/* One browser request being read, which is answered and closed once its body has arrived */
typedef struct {
	Bouncer *bouncer;
	GIOStream *connection;
	GByteArray *data;
	guint8 chunk[READ_CHUNK_BYTES];
	guint timeout_source;
	char *method, *path, *host;
	GHashTable *headers, *query, *form;
} WebRequest;

/* A signed in browser and when its sign in runs out */
typedef struct {
	gint64 user_id, expires;
} WebSession;

/* What a page answers with */
typedef struct {
	int status;
	GString *body;
	char *location, *cookie;
	gboolean is_delayed;
} WebAnswer;

static void read_more(WebRequest *request);


static void free_request(WebRequest *request) {
	g_clear_handle_id(&request->timeout_source, g_source_remove);
	g_byte_array_unref(request->data);
	g_object_unref(request->connection);
	g_free(request->method);
	g_free(request->path);
	g_free(request->host);
	g_clear_pointer(&request->headers, g_hash_table_unref);
	g_clear_pointer(&request->query, g_hash_table_unref);
	g_clear_pointer(&request->form, g_hash_table_unref);
	g_free(request);
}


static void finish_closing(GObject *source, GAsyncResult *result, gpointer data) {
	g_io_stream_close_finish(G_IO_STREAM(source), result, NULL);
	free_request(data);
}


static void finish_writing(GObject *source, GAsyncResult *result, gpointer data) {
	WebRequest *request = data;
	g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);
	g_io_stream_close_async(request->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, request);
}


static const char *describe_status(int status) {
	switch (status) {
	case 200: return "OK";
	case 303: return "See Other";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	default: return "Error";
	}
}


static void send_answer(WebRequest *request, WebAnswer *answer) {
	GString *response = g_string_new(NULL);
	g_string_append_printf(response, "HTTP/1.1 %d %s\r\n", answer->status, describe_status(answer->status));
	g_string_append(response, "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n"
		"X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
		"Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'\r\n");
	if (answer->location)
		g_string_append_printf(response, "Location: %s\r\n", answer->location);
	if (answer->cookie)
		g_string_append_printf(response, "Set-Cookie: %s\r\n", answer->cookie);
	g_string_append_printf(response, "Content-Length: %zu\r\n\r\n", answer->body ? answer->body->len : 0);
	if (answer->body && !g_str_equal(request->method, "HEAD"))
		g_string_append_len(response, answer->body->str, (gssize)answer->body->len);
	gsize length = response->len;
	GBytes *bytes = g_string_free_to_bytes(response);
	g_object_set_data_full(G_OBJECT(request->connection), "response", bytes, (GDestroyNotify)g_bytes_unref);
	g_output_stream_write_all_async(g_io_stream_get_output_stream(request->connection), g_bytes_get_data(bytes, NULL),
		length, G_PRIORITY_DEFAULT, NULL, finish_writing, request);
}


static void clear_answer(WebAnswer *answer) {
	if (answer->body)
		g_string_free(answer->body, TRUE);
	g_free(answer->location);
	g_free(answer->cookie);
}


static void start_page(WebAnswer *answer, const char *title) {
	answer->status = answer->status ? answer->status : 200;
	answer->body = g_string_new(NULL);
	g_autofree char *escaped = g_markup_escape_text(title, -1);
	g_string_append_printf(answer->body, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>%s</title><style>%s</style>"
		"</head><body><main><div class=\"logo\">C</div>", escaped, STYLE);
}


static void end_page(WebAnswer *answer) {
	g_string_append(answer->body, "</main></body></html>");
}


/* Appends text with the HTML special characters escaped */
static void add_text(WebAnswer *answer, const char *format, ...) G_GNUC_PRINTF(2, 3);


static void add_text(WebAnswer *answer, const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	g_autofree char *text = g_markup_vprintf_escaped(format, arguments);
	va_end(arguments);
	g_string_append(answer->body, text);
}


static void redirect(WebAnswer *answer, const char *location) {
	answer->status = 303;
	answer->location = g_strdup(location);
}


/* The host and port the browser used, which are also what Crimson Chat connects to */
static void split_host(const char *host, char **name, char **port) {
	const char *text = host ? host : "localhost";
	const char *bracket = strrchr(text, ']');
	const char *colon = strrchr(text, ':');
	if (colon && (!bracket || colon > bracket)) {
		*name = g_strndup(text, (gsize)(colon - text));
		*port = g_strdup(colon + 1);
	} else {
		*name = g_strdup(text);
		*port = g_strdup(DEFAULT_HTTPS_PORT);
	}
}


static void add_connection_details(WebAnswer *answer, WebRequest *request, const char *user_name) {
	g_autofree char *name = NULL, *port = NULL;
	split_host(request->host, &name, &port);
	add_text(answer, "<dl><dt>Address</dt><dd><code>%s</code></dd><dt>Port</dt><dd><code>%s</code></dd>"
		"<dt>Username</dt><dd><code>%s</code></dd>"
		"<dt>Fingerprint</dt><dd><code>%s</code></dd></dl>", name, port, user_name, request->bouncer->fingerprint);
	g_autofree char *escaped_name = g_uri_escape_string(name, NULL, FALSE);
	g_autofree char *escaped_user = g_uri_escape_string(user_name, NULL, FALSE);
	g_autofree char *escaped_label = g_uri_escape_string(request->bouncer->name, NULL, FALSE);
	add_text(answer, "<div class=\"row\"><a class=\"button\" href=\"" APPLICATION_LINK
		"?host=%s&amp;port=%s&amp;username=%s&amp;name=%s\">Open in Crimson Chat</a>"
		"<a class=\"button quiet\" href=\"%s\">Download Crimson Chat</a></div>", escaped_name, port, escaped_user,
		escaped_label, DOWNLOAD_ADDRESS);
}


static BouncerUser *find_signed_in_user(WebRequest *request) {
	const char *cookies = g_hash_table_lookup(request->headers, "cookie");
	g_auto(GStrv) parts = g_strsplit(cookies ? cookies : "", ";", -1);
	for (int index = 0; parts[index]; index++) {
		char *cookie = g_strstrip(parts[index]);
		if (!g_str_has_prefix(cookie, COOKIE_NAME "="))
			continue;
		WebSession *session = g_hash_table_lookup(request->bouncer->web_sessions, cookie + strlen(COOKIE_NAME "="));
		if (session && session->expires > g_get_monotonic_time())
			return find_user(request->bouncer, session->user_id);
	}
	return NULL;
}


static const char *read_field(WebRequest *request, const char *name) {
	const char *value = request->form ? g_hash_table_lookup(request->form, name) : NULL;
	return value ? value : "";
}


static void show_sign_in(WebAnswer *answer, WebRequest *request, const char *problem) {
	start_page(answer, request->bouncer->name);
	add_text(answer, "<h1>%s</h1>", request->bouncer->name);
	if (problem)
		add_text(answer, "<p class=\"error\">%s</p>", problem);
	add_text(answer, "<form method=\"post\" action=\"/login\"><label for=\"username\">Username</label>"
		"<input id=\"username\" name=\"username\" autocomplete=\"username\" required>"
		"<label for=\"password\">Password</label><input id=\"password\" name=\"password\" type=\"password\" "
		"autocomplete=\"current-password\" required><button>Sign In</button></form>");
	end_page(answer);
}


static char *describe_uses(int uses) {
	if (uses < 0)
		return g_strdup("Unlimited");
	return uses == 1 ? g_strdup("1 use left") : g_strdup_printf("%d uses left", uses);
}


static void add_invite_list(WebAnswer *answer, WebRequest *request) {
	add_text(answer, "<h2>Invites</h2>"
		"<form method=\"post\" action=\"/invites\"><label for=\"uses\">Uses</label>"
		"<select id=\"uses\" name=\"uses\"><option value=\"1\">One person</option><option value=\"5\">Five people"
		"</option><option value=\"25\">Twenty-five people</option><option value=\"-1\">Anyone with the link</option>"
		"</select><button>Create Invite</button></form>");
	g_autoptr(GPtrArray) invites = load_invites(request->bouncer->database);
	for (guint index = 0; index < invites->len; index++) {
		InviteRecord *invite = g_ptr_array_index(invites, index);
		g_autofree char *uses = describe_uses(invite->uses_left);
		add_text(answer, "<div class=\"invite\"><div><code>https://%s/invite?token=%s</code><p>%s</p></div>"
			"<form method=\"post\" action=\"/invites/delete\"><input type=\"hidden\" name=\"token\" value=\"%s\">"
			"<button class=\"quiet small\">Delete</button></form></div>", request->host ? request->host : "",
			invite->token, uses, invite->token);
	}
}


static void show_home(WebAnswer *answer, WebRequest *request, BouncerUser *user) {
	start_page(answer, request->bouncer->name);
	add_text(answer, "<h1>%s</h1><p>%s</p>", request->bouncer->name,
		user->name);
	add_connection_details(answer, request, user->name);
	if (user->is_admin)
		add_invite_list(answer, request);
	add_text(answer, "<form method=\"post\" action=\"/logout\"><button class=\"quiet\">Sign Out</button></form>");
	end_page(answer);
}


static gboolean sign_in(WebAnswer *answer, WebRequest *request) {
	BouncerUser *user = log_in_user(request->bouncer, read_field(request, "username"), read_field(request, "password"));
	if (!user) {
		show_sign_in(answer, request, "That username and password do not match.");
		answer->is_delayed = TRUE;
		return FALSE;
	}
	g_autofree char *first = create_irc_identifier(), *second = create_irc_identifier();
	char *token = g_strconcat(first, second, NULL);
	WebSession *session = g_new0(WebSession, 1);
	session->user_id = user->id;
	session->expires = g_get_monotonic_time() + (gint64)SESSION_SECONDS * G_USEC_PER_SEC;
	g_hash_table_insert(request->bouncer->web_sessions, token, session);
	answer->cookie = g_strdup_printf(COOKIE_NAME "=%s; Path=/; Max-Age=%d; HttpOnly; Secure; SameSite=Strict", token,
		SESSION_SECONDS);
	redirect(answer, "/");
	return TRUE;
}


static void show_invite(WebAnswer *answer, WebRequest *request, const char *token, const char *problem) {
	if (count_invite_uses(request->bouncer->database, token) == 0) {
		answer->status = 404;
		start_page(answer, "Invite not found");
		add_text(answer, "<h1>This invite has expired</h1>");
		end_page(answer);
		return;
	}
	start_page(answer, request->bouncer->name);
	add_text(answer, "<h1>You've been invited to %s!</h1>", request->bouncer->name);
	if (problem)
		add_text(answer, "<p class=\"error\">%s</p>", problem);
	add_text(answer, "<form method=\"post\" action=\"/invite\"><input type=\"hidden\" name=\"token\" value=\"%s\">"
		"<label for=\"username\">Username</label><input id=\"username\" name=\"username\" autocomplete=\"username\" "
		"maxlength=\"32\" required><label for=\"password\">Password</label><input id=\"password\" name=\"password\" "
		"type=\"password\" autocomplete=\"new-password\" minlength=\"8\" required><label for=\"repeat\">"
		"Repeat password</label><input id=\"repeat\" name=\"repeat\" type=\"password\" "
		"autocomplete=\"new-password\" required><button>Create Account</button></form>", token);
	end_page(answer);
}


static void register_invited(WebAnswer *answer, WebRequest *request) {
	const char *token = read_field(request, "token");
	const char *name = read_field(request, "username");
	const char *password = read_field(request, "password");
	if (count_invite_uses(request->bouncer->database, token) == 0) {
		show_invite(answer, request, token, NULL);
		return;
	}
	const char *problem = NULL;
	if (!is_valid_user_name(name))
		problem = "Use up to 32 letters, digits, dots, dashes or underscores for the username.";
	else if (strlen(password) < SHORTEST_PASSWORD)
		problem = "Use at least 8 characters for the password.";
	else if (!g_str_equal(password, read_field(request, "repeat")))
		problem = "The two passwords differ.";
	BouncerUser *user = problem ? NULL : register_user(request->bouncer, name, password);
	if (!user) {
		show_invite(answer, request, token, problem ? problem : "That username is taken, so try another one.");
		return;
	}
	use_invite(request->bouncer->database, token);
	start_page(answer, "Welcome");
	add_text(answer, "<h1>Welcome, %s!</h1><p>Add a bouncer in Crimson Chat with these details.</p>", user->name);
	add_connection_details(answer, request, user->name);
	end_page(answer);
}


static void change_invites(WebAnswer *answer, WebRequest *request, BouncerUser *user, gboolean is_deleting) {
	if (!user || !user->is_admin) {
		redirect(answer, "/");
		return;
	}
	if (is_deleting) {
		delete_invite(request->bouncer->database, read_field(request, "token"));
	} else {
		int uses = (int)g_ascii_strtoll(read_field(request, "uses"), NULL, 10);
		g_free(create_invite(request->bouncer, user, uses == 0 ? 1 : uses));
	}
	redirect(answer, "/");
}


static void sign_out(WebAnswer *answer, WebRequest *request) {
	const char *cookies = g_hash_table_lookup(request->headers, "cookie");
	g_auto(GStrv) parts = g_strsplit(cookies ? cookies : "", ";", -1);
	for (int index = 0; parts[index]; index++) {
		char *cookie = g_strstrip(parts[index]);
		if (g_str_has_prefix(cookie, COOKIE_NAME "="))
			g_hash_table_remove(request->bouncer->web_sessions, cookie + strlen(COOKIE_NAME "="));
	}
	answer->cookie = g_strdup(COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict");
	redirect(answer, "/");
}


static void route_request(WebAnswer *answer, WebRequest *request) {
	BouncerUser *user = find_signed_in_user(request);
	gboolean is_post = g_str_equal(request->method, "POST");
	const char *path = request->path;
	if (g_str_equal(path, "/invite") && !is_post) {
		const char *token = request->query ? g_hash_table_lookup(request->query, "token") : NULL;
		show_invite(answer, request, token ? token : "", NULL);
	} else if (g_str_equal(path, "/invite") && is_post) {
		register_invited(answer, request);
	} else if (g_str_equal(path, "/login") && is_post) {
		sign_in(answer, request);
	} else if (g_str_equal(path, "/logout") && is_post) {
		sign_out(answer, request);
	} else if (g_str_equal(path, "/invites") && is_post) {
		change_invites(answer, request, user, FALSE);
	} else if (g_str_equal(path, "/invites/delete") && is_post) {
		change_invites(answer, request, user, TRUE);
	} else if (g_str_equal(path, "/")) {
		if (user)
			show_home(answer, request, user);
		else
			show_sign_in(answer, request, NULL);
	} else {
		answer->status = 404;
		start_page(answer, "Not found");
		add_text(answer, "<h1>Not found</h1><p><a href=\"/\">Go to the start page</a></p>");
		end_page(answer);
	}
}


typedef struct {
	WebRequest *request;
	WebAnswer answer;
} DelayedAnswer;


static gboolean send_delayed_answer(gpointer data) {
	DelayedAnswer *delayed = data;
	send_answer(delayed->request, &delayed->answer);
	clear_answer(&delayed->answer);
	g_free(delayed);
	return G_SOURCE_REMOVE;
}


static gboolean read_request_line(WebRequest *request, const char *line) {
	g_auto(GStrv) parts = g_strsplit(line, " ", 3);
	if (g_strv_length(parts) != 3)
		return FALSE;
	request->method = g_strdup(parts[0]);
	char *question = strchr(parts[1], '?');
	if (question) {
		request->query = g_uri_parse_params(question + 1, -1, "&", G_URI_PARAMS_WWW_FORM, NULL);
		*question = '\0';
	}
	request->path = g_uri_unescape_string(parts[1], NULL);
	return request->path != NULL;
}


/* Splits the request into its first line, headers and form body, once all of it has arrived */
static gboolean parse_request(WebRequest *request, gsize header_length) {
	g_autofree char *head = g_strndup((const char *)request->data->data, header_length);
	g_auto(GStrv) lines = g_strsplit(head, "\r\n", -1);
	if (!lines[0] || !read_request_line(request, lines[0]))
		return FALSE;
	request->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (int index = 1; lines[index]; index++) {
		char *colon = strchr(lines[index], ':');
		if (!colon)
			continue;
		*colon = '\0';
		g_hash_table_insert(request->headers, g_ascii_strdown(lines[index], -1), g_strdup(g_strstrip(colon + 1)));
	}
	request->host = g_strdup(g_hash_table_lookup(request->headers, "host"));
	const char *body = (const char *)request->data->data + header_length + 4;
	gsize body_length = request->data->len - header_length - 4;
	if (body_length)
		request->form = g_uri_parse_params(body, (gssize)body_length, "&", G_URI_PARAMS_WWW_FORM, NULL);
	return TRUE;
}


static void answer_request(WebRequest *request, gsize header_length) {
	g_clear_handle_id(&request->timeout_source, g_source_remove);
	WebAnswer answer = {0};
	if (!parse_request(request, header_length)) {
		answer.status = 400;
		if (!request->method)
			request->method = g_strdup("GET");
		start_page(&answer, "Bad request");
		end_page(&answer);
	} else {
		route_request(&answer, request);
	}
	/* A wrong password answers after a pause, which slows anyone guessing */
	if (answer.is_delayed) {
		DelayedAnswer *delayed = g_new0(DelayedAnswer, 1);
		delayed->request = request;
		delayed->answer = answer;
		g_timeout_add(FAILED_LOGIN_DELAY_MILLISECONDS, send_delayed_answer, delayed);
		return;
	}
	send_answer(request, &answer);
	clear_answer(&answer);
}


/* Returns whether the headers and the body they announce have all arrived */
static gboolean has_whole_request(WebRequest *request, gsize *header_length) {
	const char *end = g_strstr_len((const char *)request->data->data, request->data->len, "\r\n\r\n");
	if (!end)
		return FALSE;
	*header_length = (gsize)(end - (const char *)request->data->data);
	g_autofree char *head = g_ascii_strdown((const char *)request->data->data, (gssize)*header_length);
	const char *length_header = strstr(head, "\r\ncontent-length:");
	gsize body_length = length_header ? (gsize)g_ascii_strtoull(length_header + 17, NULL, 10) : 0;
	return request->data->len >= *header_length + 4 + body_length;
}


static void check_request(WebRequest *request) {
	gsize header_length = 0;
	if (has_whole_request(request, &header_length))
		answer_request(request, header_length);
	else if (request->data->len > LARGEST_REQUEST_BYTES)
		g_io_stream_close_async(request->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, request);
	else
		read_more(request);
}


static void finish_reading(GObject *source, GAsyncResult *result, gpointer data) {
	WebRequest *request = data;
	gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);
	if (count <= 0) {
		g_io_stream_close_async(request->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, request);
		return;
	}
	g_byte_array_append(request->data, request->chunk, (guint)count);
	check_request(request);
}


static void read_more(WebRequest *request) {
	g_input_stream_read_async(g_io_stream_get_input_stream(request->connection), request->chunk,
		sizeof request->chunk, G_PRIORITY_DEFAULT, NULL, finish_reading, request);
}


/* A browser that never finishes its request is cut off, so it cannot hold the connection open */
static gboolean give_up(gpointer data) {
	WebRequest *request = data;
	request->timeout_source = 0;
	g_io_stream_close(request->connection, NULL, NULL);
	return G_SOURCE_REMOVE;
}


void serve_web(Bouncer *bouncer, GIOStream *connection, GBytes *first_bytes) {
	WebRequest *request = g_new0(WebRequest, 1);
	request->bouncer = bouncer;
	request->connection = g_object_ref(connection);
	request->data = g_byte_array_new();
	gsize length = 0;
	const guint8 *bytes = g_bytes_get_data(first_bytes, &length);
	g_byte_array_append(request->data, bytes, (guint)length);
	request->timeout_source = g_timeout_add_seconds(REQUEST_SECONDS, give_up, request);
	check_request(request);
}
