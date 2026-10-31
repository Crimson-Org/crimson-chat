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
/* Lists show this many accounts, invites or pictures a page, however many there are */
#define ITEMS_PER_PAGE 50
#define COOKIE_NAME "crimson_session"
#define AVATAR_LINE_START "POST /avatar "
#define AVATAR_LINE_START_QUERY "POST /avatar?"
#define FILE_LINE_START "POST /upload "
#define CONTINUE_ANSWER "HTTP/1.1 100 Continue\r\n\r\n"
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
	".error{color:var(--accent);font-weight:600}.row{display:flex;gap:12px;flex-wrap:wrap}"
	"main.wide{max-width:780px}header{display:flex;gap:16px;align-items:flex-start;justify-content:space-between;"
	"flex-wrap:wrap}header p{margin:0}section{margin-top:32px}"
	".notice{padding:12px 16px;border-radius:12px;background:rgba(127,127,127,.12);color:var(--fg);font-weight:600;"
	"margin-top:20px}"
	".list{border:1px solid var(--line);border-radius:14px}"
	".item{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap;padding:14px 16px;"
	"border-top:1px solid var(--line)}.item:first-child{border-top:0}.item p{margin:0}"
	".tag{display:inline-block;margin-left:8px;padding:0 10px;border-radius:999px;background:rgba(200,16,46,.15);"
	"color:var(--accent);font-weight:700}"
	".actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.actions form{display:flex;gap:8px;margin:0}"
	".actions input{width:11em;padding:6px 12px}.danger{background:#8e0a1d}"
	".tabs{display:flex;gap:6px;flex-wrap:wrap;margin-top:16px}.tabs a{padding:6px 14px;border-radius:999px;"
	"color:var(--fg);text-decoration:none;border:1px solid var(--line)}.tabs a[aria-current]{background:var(--accent);"
	"color:#fff;border-color:transparent}"
	".search{margin:20px 0 4px}.search input{flex:1;min-width:12em}.search button{margin-top:0}"
	".pager{display:flex;gap:12px;align-items:center;justify-content:center;margin-top:20px}"
	".counts{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:20px}.counts a{display:block;"
	"padding:16px;border-radius:14px;background:rgba(127,127,127,.1);color:var(--fg);text-decoration:none}"
	".counts b{display:block;font-size:28px}"
	".pictures{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:16px;margin-top:16px}"
	".picture{text-align:center}.picture img{width:96px;height:96px;border-radius:50%;object-fit:cover;"
	"display:block;margin:0 auto 6px;background:rgba(127,127,127,.15)}.picture p{margin:0 0 8px}";

/* One browser request being read, which is answered and closed once its body has arrived */
typedef struct {
	Bouncer *bouncer;
	GIOStream *connection;
	GByteArray *data;
	guint8 chunk[READ_CHUNK_BYTES];
	guint timeout_source;
	char *method, *path, *host;
	GHashTable *headers, *query, *form;
	/* The raw body, which an uploaded picture arrives as */
	GBytes *body;
	/* Whether the client was told to go on sending a body it asked about first */
	gboolean has_continued;
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
	/* A picture answers with its own type and bytes in place of a page, and may be kept by browsers */
	const char *content_type;
	GBytes *bytes;
	gboolean is_lasting;
	/* A shared file browsers should save under its name rather than show */
	char *download_name;
	/* The methods an address takes, which an OPTIONS request asks for */
	const char *allow;
} WebAnswer;

static void read_more(WebRequest *request);
static gboolean give_up(gpointer data);


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
	g_clear_pointer(&request->body, g_bytes_unref);
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
	case 201: return "Created";
	case 204: return "No Content";
	case 303: return "See Other";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 404: return "Not Found";
	case 409: return "Conflict";
	case 413: return "Content Too Large";
	case 415: return "Unsupported Media Type";
	default: return "Error";
	}
}


static void send_answer(WebRequest *request, WebAnswer *answer) {
	GString *response = g_string_new(NULL);
	g_string_append_printf(response, "HTTP/1.1 %d %s\r\n", answer->status, describe_status(answer->status));
	g_string_append_printf(response, "Content-Type: %s\r\nConnection: close\r\nCache-Control: %s\r\n",
		answer->content_type ? answer->content_type : "text/html; charset=utf-8",
		answer->is_lasting ? "public, max-age=31536000, immutable" : "no-store");
	g_string_append(response, "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
		"Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; form-action 'self'\r\n");
	if (answer->location)
		g_string_append_printf(response, "Location: %s\r\n", answer->location);
	if (answer->cookie)
		g_string_append_printf(response, "Set-Cookie: %s\r\n", answer->cookie);
	if (answer->download_name)
		g_string_append_printf(response, "Content-Disposition: attachment; filename=\"%s\"\r\n",
			answer->download_name);
	if (answer->allow)
		g_string_append_printf(response, "Allow: %s\r\n", answer->allow);
	gsize body_length = answer->bytes ? g_bytes_get_size(answer->bytes) : answer->body ? answer->body->len : 0;
	g_string_append_printf(response, "Content-Length: %zu\r\n\r\n", body_length);
	if (answer->bytes && !g_str_equal(request->method, "HEAD"))
		g_string_append_len(response, g_bytes_get_data(answer->bytes, NULL), (gssize)body_length);
	else if (answer->body && !g_str_equal(request->method, "HEAD"))
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
	g_free(answer->download_name);
	g_free(answer->cookie);
	g_clear_pointer(&answer->bytes, g_bytes_unref);
}


/* Pages with lists, such as an admin's home, get a wider card than the forms */
static void begin_page(WebAnswer *answer, const char *title, gboolean is_wide) {
	answer->status = answer->status ? answer->status : 200;
	answer->body = g_string_new(NULL);
	g_autofree char *escaped = g_markup_escape_text(title, -1);
	g_string_append_printf(answer->body, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>%s</title><style>%s</style>"
		"</head><body><main%s><div class=\"logo\">C</div>", escaped, STYLE, is_wide ? " class=\"wide\"" : "");
}


static void start_page(WebAnswer *answer, const char *title) {
	begin_page(answer, title, FALSE);
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
	g_autofree char *name = NULL, *web_port = NULL;
	split_host(request->host, &name, &web_port);
	const char *port = request->bouncer->irc_port;
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


/* Notices a change leaves on the page it returns to, named in its address so a reload repeats nothing */
static const struct {
	const char *key, *text;
} NOTICES[] = {
	{"password", "Your password changed."},
	{"account-password", "The password changed."},
	{"admin", "The admin rights changed."},
	{"removed", "The account was deleted."},
	{"pictures", "The pictures were deleted."},
	{"picture", "The picture was deleted."},
	{"short", "Use at least 8 characters for the password."},
};


static const char *find_notice(WebRequest *request) {
	const char *key = request->query ? g_hash_table_lookup(request->query, "notice") : NULL;
	for (guint index = 0; key && index < G_N_ELEMENTS(NOTICES); index++)
		if (g_str_equal(key, NOTICES[index].key))
			return NOTICES[index].text;
	return NULL;
}


static const char *read_query(WebRequest *request, const char *name) {
	const char *value = request->query ? g_hash_table_lookup(request->query, name) : NULL;
	return value ? value : "";
}


/* Pages count from 1 in addresses and from 0 here */
static guint read_page(WebRequest *request) {
	guint64 page = g_ascii_strtoull(read_query(request, "page"), NULL, 10);
	return page > 1 ? (guint)(page - 1) : 0;
}


/* The address of one page of a list, which forms send back so a change returns to the page it was made on */
static char *describe_list_address(const char *path, const char *search, guint page) {
	g_autofree char *escaped = g_uri_escape_string(search, NULL, FALSE);
	return g_strdup_printf("%s?q=%s&page=%u", path, escaped, page + 1);
}


static gboolean matches_search(const char *name, const char *search) {
	if (!*search)
		return TRUE;
	g_autofree char *folded_name = g_utf8_casefold(name, -1);
	g_autofree char *folded_search = g_utf8_casefold(search, -1);
	return strstr(folded_name, folded_search) != NULL;
}


static void add_search(WebAnswer *answer, const char *path, const char *search, const char *label) {
	add_text(answer, "<form method=\"get\" action=\"%s\" class=\"row search\"><input name=\"q\" value=\"%s\" "
		"type=\"search\" placeholder=\"%s\" aria-label=\"%s\"><button class=\"small\">Search</button></form>", path,
		search, label, label);
}


static void add_pager(WebAnswer *answer, const char *path, const char *search, guint page, guint total) {
	guint pages = MAX(1, (total + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
	if (pages == 1)
		return;
	add_text(answer, "<nav class=\"pager\">");
	if (page > 0) {
		g_autofree char *previous = describe_list_address(path, search, page - 1);
		add_text(answer, "<a class=\"button quiet small\" href=\"%s\">Previous</a>", previous);
	}
	add_text(answer, "<span>Page %u of %u</span>", page + 1, pages);
	if (page + 1 < pages) {
		g_autofree char *next = describe_list_address(path, search, page + 1);
		add_text(answer, "<a class=\"button quiet small\" href=\"%s\">Next</a>", next);
	}
	add_text(answer, "</nav>");
}


/* Every admin page has the same header, which names where the admin is and links the other pages */
static void start_admin_page(WebAnswer *answer, WebRequest *request, BouncerUser *user, const char *title) {
	begin_page(answer, title, TRUE);
	add_text(answer, "<header><div><h1>%s</h1><p>%s, signed in as <b>%s</b>%s</p></div>"
		"<form method=\"post\" action=\"/logout\"><button class=\"quiet small\">Sign Out</button></form></header>",
		title, request->bouncer->name, user->name, user->is_admin ? ", an admin" : "");
	if (user->is_admin) {
		static const char *const LINKS[][2] = {
			{"/", "Home"}, {"/accounts", "Accounts"}, {"/invites", "Invites"}, {"/pictures", "Pictures"},
		};
		add_text(answer, "<nav class=\"tabs\">");
		for (guint index = 0; index < G_N_ELEMENTS(LINKS); index++)
			add_text(answer, "<a href=\"%s\"%s>%s</a>", LINKS[index][0],
				g_str_equal(request->path, LINKS[index][0]) ? " aria-current=\"page\"" : "", LINKS[index][1]);
		add_text(answer, "</nav>");
	}
	const char *notice = find_notice(request);
	if (notice)
		add_text(answer, "<p class=\"notice\">%s</p>", notice);
}


/* A hidden field naming the list page a form was sent from */
static void add_back_field(WebAnswer *answer, const char *back) {
	add_text(answer, "<input type=\"hidden\" name=\"back\" value=\"%s\">", back);
}


static void show_invites(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	start_admin_page(answer, request, admin, "Invites");
	g_autoptr(GPtrArray) invites = load_invites(request->bouncer->database);
	guint page = read_page(request);
	g_autofree char *back = describe_list_address("/invites", "", page);
	add_text(answer, "<p>Anyone with an invite link can make an account. There are %u now.</p>"
		"<form method=\"post\" action=\"/invites/create\" class=\"row\">", invites->len);
	add_back_field(answer, back);
	add_text(answer, "<select name=\"uses\" aria-label=\"Uses\"><option value=\"1\">One person</option>"
		"<option value=\"5\">Five people</option><option value=\"25\">Twenty-five people</option>"
		"<option value=\"-1\">Anyone with the link</option></select><button>Create Invite</button></form>");
	guint first = page * ITEMS_PER_PAGE;
	if (first < invites->len)
		add_text(answer, "<div class=\"list\">");
	for (guint index = first; index < invites->len && index < first + ITEMS_PER_PAGE; index++) {
		InviteRecord *invite = g_ptr_array_index(invites, index);
		g_autofree char *uses = describe_uses(invite->uses_left);
		add_text(answer, "<div class=\"item\"><div><code>https://%s/invite?token=%s</code><p>%s</p></div>"
			"<form method=\"post\" action=\"/invites/delete\"><input type=\"hidden\" name=\"token\" value=\"%s\">",
			request->host ? request->host : "", invite->token, uses, invite->token);
		add_back_field(answer, back);
		add_text(answer, "<button class=\"quiet small\">Delete</button></form></div>");
	}
	if (first < invites->len)
		add_text(answer, "</div>");
	add_pager(answer, "/invites", "", page, invites->len);
	end_page(answer);
}


static char *describe_networks(BouncerUser *user) {
	if (!user->upstreams->len)
		return g_strdup("No servers");
	GString *text = g_string_new(NULL);
	for (guint index = 0; index < user->upstreams->len; index++) {
		Upstream *upstream = g_ptr_array_index(user->upstreams, index);
		const char *name = upstream->record.name && *upstream->record.name ? upstream->record.name
			: upstream->record.host;
		g_string_append_printf(text, "%s%s, %s", index ? "; " : "", name, describe_upstream_state(upstream));
	}
	return g_string_free(text, FALSE);
}


/* How many pictures each user has, counted once for the whole page */
static GHashTable *count_pictures_by_user(GPtrArray *pictures) {
	GHashTable *counts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (guint index = 0; index < pictures->len; index++) {
		gint64 user_id = ((AvatarFile *)g_ptr_array_index(pictures, index))->user_id;
		guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, &user_id));
		g_hash_table_replace(counts, g_memdup2(&user_id, sizeof user_id), GUINT_TO_POINTER(count + 1));
	}
	return counts;
}


/* What an admin can do to an account, where nobody changes their own admin rights or deletes themselves, so the
 * bouncer always keeps an admin */
static void add_account_actions(WebAnswer *answer, BouncerUser *account, gboolean is_self, guint pictures,
	const char *back) {
	add_text(answer, "<div class=\"actions\">");
	if (!is_self) {
		add_text(answer, "<form method=\"post\" action=\"/accounts/admin\"><input type=\"hidden\" name=\"id\" "
			"value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"admin\" value=\"%s\">", account->id,
			account->is_admin ? "0" : "1");
		add_back_field(answer, back);
		add_text(answer, "<button class=\"quiet small\">%s</button></form>",
			account->is_admin ? "Remove Admin" : "Make Admin");
	}
	add_text(answer, "<form method=\"post\" action=\"/accounts/password\"><input type=\"hidden\" name=\"id\" "
		"value=\"%" G_GINT64_FORMAT "\"><input name=\"password\" type=\"password\" placeholder=\"New password\" "
		"aria-label=\"New password for %s\" autocomplete=\"new-password\" minlength=\"8\" required>", account->id,
		account->name);
	add_back_field(answer, back);
	add_text(answer, "<button class=\"quiet small\">Set Password</button></form>");
	if (pictures) {
		add_text(answer, "<form method=\"post\" action=\"/accounts/pictures\"><input type=\"hidden\" name=\"id\" "
			"value=\"%" G_GINT64_FORMAT "\">", account->id);
		add_back_field(answer, back);
		add_text(answer, "<button class=\"quiet small\">Delete Pictures</button></form>");
	}
	if (!is_self) {
		add_text(answer, "<form method=\"post\" action=\"/accounts/delete\"><input type=\"hidden\" name=\"id\" "
			"value=\"%" G_GINT64_FORMAT "\">", account->id);
		add_back_field(answer, back);
		add_text(answer, "<button class=\"small danger\">Delete Account</button></form>");
	}
	add_text(answer, "</div>");
}


static void show_accounts(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	start_admin_page(answer, request, admin, "Accounts");
	const char *search = read_query(request, "q");
	guint page = read_page(request);
	g_autofree char *back = describe_list_address("/accounts", search, page);
	GPtrArray *users = request->bouncer->users;
	g_autoptr(GPtrArray) found = g_ptr_array_new();
	for (guint index = 0; index < users->len; index++) {
		BouncerUser *account = g_ptr_array_index(users, index);
		if (matches_search(account->name, search))
			g_ptr_array_add(found, account);
	}
	add_search(answer, "/accounts", search, "Search accounts by name");
	add_text(answer, "<p>%u of %u %s.</p>", found->len, users->len, users->len == 1 ? "account" : "accounts");
	g_autoptr(GPtrArray) pictures = list_avatars(request->bouncer);
	g_autoptr(GHashTable) counts = count_pictures_by_user(pictures);
	guint first = page * ITEMS_PER_PAGE;
	if (first < found->len)
		add_text(answer, "<div class=\"list\">");
	for (guint index = first; index < found->len && index < first + ITEMS_PER_PAGE; index++) {
		BouncerUser *account = g_ptr_array_index(found, index);
		g_autofree char *networks = describe_networks(account);
		guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, &account->id));
		add_text(answer, "<div class=\"item\"><div><p><b>%s</b>", account->name);
		if (account->is_admin)
			add_text(answer, "<span class=\"tag\">Admin</span>");
		if (account == admin)
			add_text(answer, "<span class=\"tag\">You</span>");
		add_text(answer, "</p><p>%s</p><p>%u %s</p></div>", networks, count, count == 1 ? "picture" : "pictures");
		add_account_actions(answer, account, account == admin, count, back);
		add_text(answer, "</div>");
	}
	if (first < found->len)
		add_text(answer, "</div>");
	add_pager(answer, "/accounts", search, page, found->len);
	end_page(answer);
}


static void show_pictures(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	start_admin_page(answer, request, admin, "Pictures");
	const char *search = read_query(request, "q");
	guint page = read_page(request);
	g_autofree char *back = describe_list_address("/pictures", search, page);
	g_autoptr(GPtrArray) pictures = list_avatars(request->bouncer);
	g_autoptr(GPtrArray) found = g_ptr_array_new();
	goffset total = 0;
	for (guint index = 0; index < pictures->len; index++) {
		AvatarFile *file = g_ptr_array_index(pictures, index);
		BouncerUser *owner = find_user(request->bouncer, file->user_id);
		total += file->bytes;
		if (matches_search(owner ? owner->name : "", search))
			g_ptr_array_add(found, file);
	}
	g_autofree char *size = g_format_size((guint64)total);
	add_search(answer, "/pictures", search, "Search pictures by owner");
	add_text(answer, "<div class=\"row\" style=\"align-items:center;justify-content:space-between\">"
		"<p style=\"margin:0\">%u of %u %s, %s in all.</p>", found->len, pictures->len,
		pictures->len == 1 ? "picture" : "pictures", size);
	if (pictures->len) {
		add_text(answer, "<form method=\"post\" action=\"/pictures/clear\">");
		add_back_field(answer, back);
		add_text(answer, "<button class=\"small danger\">Delete All Pictures</button></form>");
	}
	add_text(answer, "</div><div class=\"pictures\">");
	guint first = page * ITEMS_PER_PAGE;
	for (guint index = first; index < found->len && index < first + ITEMS_PER_PAGE; index++) {
		AvatarFile *file = g_ptr_array_index(found, index);
		BouncerUser *owner = find_user(request->bouncer, file->user_id);
		add_text(answer, "<div class=\"picture\"><img src=\"/avatars/%s\" alt=\"\" loading=\"lazy\"><p>%s</p>"
			"<form method=\"post\" action=\"/pictures/delete\"><input type=\"hidden\" name=\"name\" value=\"%s\">",
			file->name, owner ? owner->name : "A removed account", file->name);
		add_back_field(answer, back);
		add_text(answer, "<button class=\"quiet small\">Delete</button></form></div>");
	}
	add_text(answer, "</div>");
	add_pager(answer, "/pictures", search, page, found->len);
	end_page(answer);
}


/* The home page shows the user's own login, and an admin the size of each list with a link to it */
static void show_home(WebAnswer *answer, WebRequest *request, BouncerUser *user, const char *problem) {
	start_admin_page(answer, request, user, "Home");
	if (problem)
		add_text(answer, "<p class=\"notice error\">%s</p>", problem);
	if (user->is_admin) {
		g_autoptr(GPtrArray) invites = load_invites(request->bouncer->database);
		g_autoptr(GPtrArray) pictures = list_avatars(request->bouncer);
		add_text(answer, "<div class=\"counts\"><a href=\"/accounts\"><b>%u</b>Accounts</a>"
			"<a href=\"/invites\"><b>%u</b>Invites</a><a href=\"/pictures\"><b>%u</b>Pictures</a></div>",
			request->bouncer->users->len, invites->len, pictures->len);
	}
	add_text(answer, "<section><h2>Connect</h2><p>Add this bouncer in Crimson Chat with these details.</p>");
	add_connection_details(answer, request, user->name);
	add_text(answer, "</section><section id=\"password\"><h2>Your Password</h2>"
		"<form method=\"post\" action=\"/password\"><label for=\"current\">Current password</label>"
		"<input id=\"current\" name=\"current\" type=\"password\" autocomplete=\"current-password\" required>"
		"<label for=\"new\">New password</label><input id=\"new\" name=\"password\" type=\"password\" "
		"autocomplete=\"new-password\" minlength=\"8\" required><label for=\"repeat\">Repeat new password</label>"
		"<input id=\"repeat\" name=\"repeat\" type=\"password\" autocomplete=\"new-password\" minlength=\"8\" "
		"required><button>Change Password</button></form></section>");
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


static const char *find_password_problem(const char *password, const char *repeat) {
	if (strlen(password) < SHORTEST_PASSWORD)
		return "Use at least 8 characters for the password.";
	return g_str_equal(password, repeat) ? NULL : "The two passwords differ.";
}


static void register_invited(WebAnswer *answer, WebRequest *request) {
	const char *token = read_field(request, "token");
	const char *name = read_field(request, "username");
	const char *password = read_field(request, "password");
	if (count_invite_uses(request->bouncer->database, token) == 0) {
		show_invite(answer, request, token, NULL);
		return;
	}
	const char *problem = is_valid_user_name(name) ? find_password_problem(password, read_field(request, "repeat"))
		: "Use up to 32 letters, digits, dots, dashes or underscores for the username.";
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


/* The list page a form came from, or the fallback when it names anything else, so no form can send a browser
 * elsewhere */
static const char *read_back(WebRequest *request, const char *fallback) {
	static const char *const LISTS[] = {"/accounts?", "/invites?", "/pictures?"};
	const char *back = read_field(request, "back");
	for (const char *letter = back; *letter; letter++)
		if ((guchar)*letter < ' ')
			return fallback;
	for (guint index = 0; index < G_N_ELEMENTS(LISTS); index++)
		if (g_str_has_prefix(back, LISTS[index]))
			return back;
	return fallback;
}


static void return_to_list(WebAnswer *answer, WebRequest *request, const char *fallback, const char *notice) {
	const char *place = read_back(request, fallback);
	g_autofree char *address = notice ? g_strdup_printf("%s%cnotice=%s", place, strchr(place, '?') ? '&' : '?',
		notice) : g_strdup(place);
	redirect(answer, address);
}


/* Deleting asks first on a page of its own, whose button sends the same request again marked as confirmed */
static gboolean is_confirmed(WebAnswer *answer, WebRequest *request, const char *question, const char *detail) {
	if (g_str_equal(read_field(request, "confirmed"), "1"))
		return TRUE;
	const char *back = read_back(request, "/");
	start_page(answer, question);
	add_text(answer, "<h1>%s</h1><p>%s</p><form method=\"post\" action=\"%s\">"
		"<input type=\"hidden\" name=\"id\" value=\"%s\"><input type=\"hidden\" name=\"confirmed\" value=\"1\">",
		question, detail, request->path, read_field(request, "id"));
	add_back_field(answer, back);
	add_text(answer, "<div class=\"row\"><button class=\"danger\">Delete</button>"
		"<a class=\"button quiet\" href=\"%s\">Cancel</a></div></form>", back);
	end_page(answer);
	return FALSE;
}


static void create_web_invite(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	int uses = (int)g_ascii_strtoll(read_field(request, "uses"), NULL, 10);
	g_free(create_invite(request->bouncer, admin, uses == 0 ? 1 : uses));
	return_to_list(answer, request, "/invites?", NULL);
}


static void delete_web_invite(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	delete_invite(request->bouncer->database, read_field(request, "token"));
	return_to_list(answer, request, "/invites?", NULL);
}


static void change_own_password(WebAnswer *answer, WebRequest *request, BouncerUser *user) {
	const char *password = read_field(request, "password");
	if (!check_password(read_field(request, "current"), user->password_hash)) {
		answer->is_delayed = TRUE;
		show_home(answer, request, user, "The current password is not right.");
		return;
	}
	const char *problem = find_password_problem(password, read_field(request, "repeat"));
	if (problem) {
		show_home(answer, request, user, problem);
		return;
	}
	set_user_password(user, password);
	redirect(answer, "/?notice=password");
}


static BouncerUser *find_chosen_user(WebRequest *request) {
	return find_user(request->bouncer, g_ascii_strtoll(read_field(request, "id"), NULL, 10));
}


static void change_admin(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	BouncerUser *account = find_chosen_user(request);
	if (account && account != admin)
		set_user_admin(account, g_str_equal(read_field(request, "admin"), "1"));
	return_to_list(answer, request, "/accounts?", account && account != admin ? "admin" : NULL);
}


static void change_account_password(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	BouncerUser *account = find_chosen_user(request);
	const char *password = read_field(request, "password");
	if (!account || find_password_problem(password, password)) {
		return_to_list(answer, request, "/accounts?", account ? "short" : NULL);
		return;
	}
	set_user_password(account, password);
	return_to_list(answer, request, "/accounts?", "account-password");
}


static void delete_account_pictures(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	BouncerUser *account = find_chosen_user(request);
	if (!account) {
		return_to_list(answer, request, "/accounts?", NULL);
		return;
	}
	g_autofree char *question = g_strdup_printf("Delete the pictures of %s?", account->name);
	if (!is_confirmed(answer, request, question, "People see no picture for them or their rooms until they "
		"upload another."))
		return;
	remove_user_avatars(request->bouncer, account->id);
	return_to_list(answer, request, "/accounts?", "pictures");
}


static void delete_account(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	BouncerUser *account = find_chosen_user(request);
	if (!account || account == admin) {
		return_to_list(answer, request, "/accounts?", NULL);
		return;
	}
	g_autofree char *question = g_strdup_printf("Delete %s?", account->name);
	if (!is_confirmed(answer, request, question, "Their servers disconnect, and their history and pictures are "
		"deleted for good."))
		return;
	remove_user(account);
	return_to_list(answer, request, "/accounts?", "removed");
}


static void delete_picture(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	remove_avatar(request->bouncer, read_field(request, "name"));
	return_to_list(answer, request, "/pictures?", "picture");
}


static void delete_all_pictures(WebAnswer *answer, WebRequest *request, BouncerUser *admin) {
	if (!is_confirmed(answer, request, "Delete every picture?", "People see no pictures for anyone or any room "
		"until they upload new ones."))
		return;
	g_autoptr(GPtrArray) pictures = list_avatars(request->bouncer);
	for (guint index = 0; index < pictures->len; index++)
		remove_avatar(request->bouncer, ((AvatarFile *)g_ptr_array_index(pictures, index))->name);
	return_to_list(answer, request, "/pictures?", "pictures");
}


/* What only admins may send from the website, by address */
static const struct {
	const char *path;
	void (*act)(WebAnswer *answer, WebRequest *request, BouncerUser *admin);
} ADMIN_ACTIONS[] = {
	{"/invites/create", create_web_invite},
	{"/invites/delete", delete_web_invite},
	{"/accounts/admin", change_admin},
	{"/accounts/password", change_account_password},
	{"/accounts/pictures", delete_account_pictures},
	{"/accounts/delete", delete_account},
	{"/pictures/delete", delete_picture},
	{"/pictures/clear", delete_all_pictures},
};


/* The lists only admins may see, by address */
static const struct {
	const char *path;
	void (*act)(WebAnswer *answer, WebRequest *request, BouncerUser *admin);
} ADMIN_PAGES[] = {
	{"/accounts", show_accounts},
	{"/invites", show_invites},
	{"/pictures", show_pictures},
};


/* Finds what the address names, for admins only, and sends anyone else home */
static gboolean run_admin_action(WebAnswer *answer, WebRequest *request, BouncerUser *user, gboolean is_post) {
	void (*act)(WebAnswer *answer, WebRequest *request, BouncerUser *admin) = NULL;
	for (guint index = 0; is_post && index < G_N_ELEMENTS(ADMIN_ACTIONS); index++)
		if (g_str_equal(request->path, ADMIN_ACTIONS[index].path))
			act = ADMIN_ACTIONS[index].act;
	for (guint index = 0; !is_post && index < G_N_ELEMENTS(ADMIN_PAGES); index++)
		if (g_str_equal(request->path, ADMIN_PAGES[index].path))
			act = ADMIN_PAGES[index].act;
	if (!act)
		return FALSE;
	if (user && user->is_admin)
		act(answer, request, user);
	else
		redirect(answer, "/");
	return TRUE;
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


static void answer_plainly(WebAnswer *answer, int status, const char *text) {
	answer->status = status;
	answer->content_type = "text/plain; charset=utf-8";
	answer->body = g_string_new(text);
}


/* An app logs in with the user's bouncer name and password, as it does over IRC, where other clients may add the
 * network or their own name after a slash or an at sign */
static BouncerUser *read_basic_login(WebRequest *request) {
	const char *header = g_hash_table_lookup(request->headers, "authorization");
	if (!header || g_ascii_strncasecmp(header, "Basic ", 6) != 0)
		return NULL;
	gsize length = 0;
	g_autofree guchar *decoded = g_base64_decode(header + 6, &length);
	g_autofree char *text = g_strndup((const char *)decoded, length);
	char *colon = strchr(text, ':');
	if (!colon)
		return NULL;
	*colon = '\0';
	text[strcspn(text, "/@")] = '\0';
	return log_in_user(request->bouncer, text, colon + 1);
}


/* The address pictures are served from: the configured public one, or the address the app reached the bouncer by */
static char *describe_base_url(WebRequest *request) {
	if (request->bouncer->public_url)
		return g_strdup(request->bouncer->public_url);
	return g_strdup_printf("https://%s", request->host ? request->host : "localhost");
}


/* A picture for the user, or for a room they run, on one of their networks */
static void upload_avatar(WebAnswer *answer, WebRequest *request) {
	if (!request->bouncer->avatars.is_enabled) {
		answer_plainly(answer, 404, "This bouncer does not take pictures");
		return;
	}
	BouncerUser *user = read_basic_login(request);
	if (!user) {
		answer->is_delayed = TRUE;
		answer_plainly(answer, 401, "The bouncer login was not accepted");
		return;
	}
	const char *network = request->query ? g_hash_table_lookup(request->query, "network") : NULL;
	const char *target = request->query ? g_hash_table_lookup(request->query, "target") : NULL;
	Upstream *upstream = find_upstream(user, network);
	if (!upstream || get_irc_client_status(upstream->client) != IRC_CLIENT_REGISTERED) {
		answer_plainly(answer, 409, "That server is not connected right now");
		return;
	}
	if (!has_irc_cap(get_irc_client_state(upstream->client), "draft/metadata-2")) {
		answer_plainly(answer, 409, "That server does not support pictures");
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autofree char *base = describe_base_url(request);
	g_autofree char *url = store_avatar(request->bouncer, user, upstream, target && *target ? target : "*",
		request->body, g_hash_table_lookup(request->headers, "content-type"), base, &error);
	if (!url) {
		gboolean is_large = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
		answer_plainly(answer, is_large ? 413 : 415, error->message);
		return;
	}
	answer_plainly(answer, 201, url);
	answer->location = g_strdup(url);
}


static void serve_avatar(WebAnswer *answer, WebRequest *request, const char *name) {
	const char *content_type = NULL;
	GBytes *bytes = load_avatar(request->bouncer, name, &content_type);
	if (!bytes) {
		answer_plainly(answer, 404, "No such picture");
		return;
	}
	answer->status = 200;
	answer->content_type = content_type;
	answer->bytes = bytes;
	answer->is_lasting = TRUE;
}


/* The name in a Content-Disposition header, plain or quoted, or in the encoded form for names outside ASCII */
static char *read_upload_name(WebRequest *request) {
	const char *header = g_hash_table_lookup(request->headers, "content-disposition");
	const char *encoded = header ? strstr(header, "filename*=") : NULL;
	if (encoded) {
		const char *quote = strstr(encoded, "''");
		g_autofree char *value = g_strndup(quote ? quote + 2 : encoded + 10, strcspn(quote ? quote + 2 : encoded
			+ 10, ";"));
		char *name = g_uri_unescape_string(g_strstrip(value), NULL);
		if (name)
			return name;
	}
	const char *plain = header ? strstr(header, "filename=") : NULL;
	if (!plain)
		return NULL;
	plain += strlen("filename=");
	if (*plain == '"')
		return g_strndup(plain + 1, strcspn(plain + 1, "\""));
	g_autofree char *value = g_strndup(plain, strcspn(plain, ";"));
	return g_strdup(g_strstrip(value));
}


/* A file shared in a chat, sent as the body with its name in Content-Disposition and answered with its address */
static void upload_file(WebAnswer *answer, WebRequest *request) {
	if (!request->bouncer->files.is_enabled) {
		answer_plainly(answer, 404, "This bouncer does not take files");
		return;
	}
	BouncerUser *user = read_basic_login(request);
	if (!user) {
		answer->is_delayed = TRUE;
		answer_plainly(answer, 401, "The bouncer login was not accepted");
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autofree char *name = read_upload_name(request);
	g_autofree char *base = describe_base_url(request);
	g_autofree char *url = store_file(request->bouncer, user, request->body, name, base, &error);
	if (!url) {
		gboolean is_large = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
		answer_plainly(answer, is_large ? 413 : 500, error->message);
		return;
	}
	answer_plainly(answer, 201, url);
	answer->location = g_strdup(url);
}


static void serve_file(WebAnswer *answer, WebRequest *request, const char *name) {
	const char *content_type = NULL;
	gboolean is_download = FALSE;
	GBytes *bytes = load_file(request->bouncer, name, &content_type, &is_download);
	if (!bytes) {
		answer_plainly(answer, 404, "No such file");
		return;
	}
	answer->status = 200;
	answer->content_type = content_type;
	answer->bytes = bytes;
	answer->is_lasting = TRUE;
	/* The name the file was kept under starts with the user's id and a token, which the saved copy leaves out */
	const char *shown = strchr(name, '-');
	shown = shown ? strchr(shown + 1, '-') : NULL;
	if (is_download)
		answer->download_name = g_strdup(shown ? shown + 1 : name);
}


static void route_request(WebAnswer *answer, WebRequest *request) {
	BouncerUser *user = find_signed_in_user(request);
	gboolean is_post = g_str_equal(request->method, "POST");
	const char *path = request->path;
	if (g_str_equal(path, "/avatar") && is_post) {
		upload_avatar(answer, request);
	} else if (g_str_has_prefix(path, "/avatars/") && !is_post) {
		serve_avatar(answer, request, path + strlen("/avatars/"));
	} else if (g_str_equal(path, "/upload") && is_post) {
		upload_file(answer, request);
	} else if (g_str_equal(path, "/upload") && g_str_equal(request->method, "OPTIONS")) {
		answer->status = request->bouncer->files.is_enabled ? 204 : 404;
		answer->allow = "OPTIONS, POST";
	} else if (g_str_has_prefix(path, "/files/") && !is_post) {
		serve_file(answer, request, path + strlen("/files/"));
	} else if (g_str_equal(path, "/invite") && !is_post) {
		const char *token = request->query ? g_hash_table_lookup(request->query, "token") : NULL;
		show_invite(answer, request, token ? token : "", NULL);
	} else if (g_str_equal(path, "/invite") && is_post) {
		register_invited(answer, request);
	} else if (g_str_equal(path, "/login") && is_post) {
		sign_in(answer, request);
	} else if (g_str_equal(path, "/logout") && is_post) {
		sign_out(answer, request);
	} else if (g_str_equal(path, "/password") && is_post && user) {
		change_own_password(answer, request, user);
	} else if (run_admin_action(answer, request, user, is_post)) {
		return;
	} else if (g_str_equal(path, "/")) {
		if (user)
			show_home(answer, request, user, NULL);
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
	request->body = g_bytes_new(body, body_length);
	const char *type = g_hash_table_lookup(request->headers, "content-type");
	gboolean is_form = !type || g_str_has_prefix(type, "application/x-www-form-urlencoded");
	if (body_length && is_form)
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


/* Returns whether the headers have arrived, with their length and the length of the body they announce */
static gboolean read_head(WebRequest *request, gsize *header_length, gsize *body_length) {
	const char *end = g_strstr_len((const char *)request->data->data, request->data->len, "\r\n\r\n");
	if (!end)
		return FALSE;
	*header_length = (gsize)(end - (const char *)request->data->data);
	g_autofree char *head = g_ascii_strdown((const char *)request->data->data, (gssize)*header_length);
	const char *length_header = strstr(head, "\r\ncontent-length:");
	*body_length = length_header ? (gsize)g_ascii_strtoull(length_header + 17, NULL, 10) : 0;
	return TRUE;
}


static gboolean starts_with(WebRequest *request, const char *start) {
	gsize length = strlen(start);
	return request->data->len >= length && memcmp(request->data->data, start, length) == 0;
}


/* Uploads of pictures and files may be larger than pages and forms, up to what the settings allow */
static gsize find_largest_body(WebRequest *request) {
	Bouncer *bouncer = request->bouncer;
	if (bouncer->avatars.is_enabled && (starts_with(request, AVATAR_LINE_START) || starts_with(request,
		AVATAR_LINE_START_QUERY)))
		return bouncer->avatars.largest_bytes;
	if (bouncer->files.is_enabled && starts_with(request, FILE_LINE_START))
		return bouncer->files.largest_bytes;
	return LARGEST_REQUEST_BYTES;
}


/* A body too large is refused as soon as its length is known, so the sender learns why instead of being cut off */
static void refuse_large_request(WebRequest *request) {
	g_clear_handle_id(&request->timeout_source, g_source_remove);
	WebAnswer answer = {0};
	request->method = g_strdup("POST");
	gsize largest = find_largest_body(request);
	g_autofree char *text = largest > LARGEST_REQUEST_BYTES ? g_strdup_printf("Files can be at most %.0f MB",
		largest / (1024.0 * 1024.0)) : g_strdup("That request is too large");
	answer_plainly(&answer, 413, text);
	send_answer(request, &answer);
	clear_answer(&answer);
}


/* An upload may take longer than a page, so it only gives up once it stops arriving */
static void wait_for_more(WebRequest *request) {
	if (find_largest_body(request) > LARGEST_REQUEST_BYTES) {
		g_clear_handle_id(&request->timeout_source, g_source_remove);
		request->timeout_source = g_timeout_add_seconds(REQUEST_SECONDS, give_up, request);
	}
	read_more(request);
}


static void finish_continuing(GObject *source, GAsyncResult *result, gpointer data) {
	WebRequest *request = data;
	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL)) {
		g_io_stream_close_async(request->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, request);
		return;
	}
	wait_for_more(request);
}


/* A client that asks before sending a body hears to go on once its size is allowed */
static gboolean continue_request(WebRequest *request, gsize header_length) {
	if (request->has_continued)
		return FALSE;
	g_autofree char *head = g_ascii_strdown((const char *)request->data->data, (gssize)header_length);
	if (!strstr(head, "\r\nexpect: 100-continue"))
		return FALSE;
	request->has_continued = TRUE;
	g_output_stream_write_all_async(g_io_stream_get_output_stream(request->connection), CONTINUE_ANSWER,
		strlen(CONTINUE_ANSWER), G_PRIORITY_DEFAULT, NULL, finish_continuing, request);
	return TRUE;
}


static void check_request(WebRequest *request) {
	gsize header_length = 0, body_length = 0;
	gboolean has_head = read_head(request, &header_length, &body_length);
	gsize largest = find_largest_body(request) + LARGEST_REQUEST_BYTES;
	if (has_head && body_length > find_largest_body(request))
		refuse_large_request(request);
	else if (has_head && request->data->len >= header_length + 4 + body_length)
		answer_request(request, header_length);
	else if (request->data->len > largest)
		g_io_stream_close_async(request->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, request);
	else if (!has_head || !continue_request(request, header_length))
		wait_for_more(request);
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


void serve_web(Bouncer *bouncer, GIOStream *connection) {
	WebRequest *request = g_new0(WebRequest, 1);
	request->bouncer = bouncer;
	request->connection = g_object_ref(connection);
	request->data = g_byte_array_new();
	request->timeout_source = g_timeout_add_seconds(REQUEST_SECONDS, give_up, request);
	read_more(request);
}
