#include <string.h>

#include "link-previews.h"

/* The head of a page, where its title and description are, sits within this many bytes */
#define LARGEST_HEAD_BYTES (256 * 1024)
#define READ_CHUNK_BYTES 16384
/* The previews kept at most, after which the kept ones are let go and fetched again when shown */
#define KEPT_PREVIEWS 500

/* Addresses to their previews, with NULL for pages that have none, and to the owners waiting for each */
static GHashTable *previews, *waiting;

/* Who waits for a page's preview, and what to call on each of them */
typedef struct {
	ImageReady ready;
	GPtrArray *owners;
} Waiters;

/* A page being read */
typedef struct {
	char *url;
	GInputStream *stream;
	GByteArray *head;
} PageFetch;


static void free_preview(LinkPreview *preview) {
	if (!preview)
		return;
	g_free(preview->url);
	g_free(preview->site);
	g_free(preview->title);
	g_free(preview->description);
	g_free(preview->image_url);
	g_free(preview);
}


static void free_weak_reference(GWeakRef *reference) {
	g_weak_ref_clear(reference);
	g_free(reference);
}


static void free_waiters(Waiters *waiters) {
	g_ptr_array_unref(waiters->owners);
	g_free(waiters);
}


static void ensure_tables(void) {
	if (previews)
		return;
	previews = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_preview);
	waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_waiters);
}


const LinkPreview *get_link_preview(const char *url) {
	ensure_tables();
	return g_hash_table_lookup(previews, url);
}


/* Turns the character references pages use in their titles back into the characters */
static char *decode_entities(const char *text) {
	static const struct {
		const char *name, *character;
	} NAMED[] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}};
	GString *decoded = g_string_new(NULL);
	for (const char *cursor = text; *cursor;) {
		const char *end = *cursor == '&' ? strchr(cursor, ';') : NULL;
		if (!end || end - cursor > 10) {
			g_string_append_c(decoded, *cursor++);
			continue;
		}
		g_autofree char *name = g_strndup(cursor + 1, (gsize)(end - cursor - 1));
		gunichar number = *name == '#' ? (gunichar)(name[1] == 'x' ? g_ascii_strtoull(name + 2, NULL, 16)
			: g_ascii_strtoull(name + 1, NULL, 10)) : 0;
		const char *character = NULL;
		for (guint index = 0; index < G_N_ELEMENTS(NAMED); index++) {
			if (g_str_equal(name, NAMED[index].name))
				character = NAMED[index].character;
		}
		if (number && g_unichar_validate(number))
			g_string_append_unichar(decoded, number);
		else if (character)
			g_string_append(decoded, character);
		else
			g_string_append_len(decoded, cursor, end - cursor + 1);
		cursor = end + 1;
	}
	return g_strstrip(g_string_free(decoded, FALSE));
}


/* The content of the first meta tag naming one of the properties, in either order of its attributes */
static char *read_meta(const char *head, const char *const *properties) {
	static GRegex *meta, *attribute;
	if (!meta) {
		meta = g_regex_new("<meta\\s[^>]*>", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
		attribute = g_regex_new("([a-z:-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')", G_REGEX_CASELESS | G_REGEX_OPTIMIZE,
			0, NULL);
	}
	for (int wanted = 0; properties[wanted]; wanted++) {
		g_autoptr(GMatchInfo) tags = NULL;
		for (g_regex_match(meta, head, 0, &tags); g_match_info_matches(tags); g_match_info_next(tags, NULL)) {
			g_autofree char *tag = g_match_info_fetch(tags, 0);
			g_autofree char *name = NULL, *content = NULL;
			g_autoptr(GMatchInfo) pairs = NULL;
			for (g_regex_match(attribute, tag, 0, &pairs); g_match_info_matches(pairs); g_match_info_next(pairs, NULL)) {
				g_autofree char *key = g_match_info_fetch(pairs, 1);
				g_autofree char *value = g_match_info_fetch(pairs, 2);
				if (!value || !*value) {
					g_free(value);
					value = g_match_info_fetch(pairs, 3);
				}
				if (g_ascii_strcasecmp(key, "property") == 0 || g_ascii_strcasecmp(key, "name") == 0)
					g_set_str(&name, value);
				else if (g_ascii_strcasecmp(key, "content") == 0)
					g_set_str(&content, value);
			}
			if (name && content && *content && g_ascii_strcasecmp(name, properties[wanted]) == 0)
				return decode_entities(content);
		}
	}
	return NULL;
}


static char *read_title(const char *head) {
	static GRegex *title;
	if (!title)
		title = g_regex_new("<title[^>]*>([^<]*)</title>", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	if (!g_regex_match(title, head, 0, &match))
		return NULL;
	g_autofree char *text = g_match_info_fetch(match, 1);
	return decode_entities(text);
}


/* A picture address the page gives relative to itself is read against the page's own address */
static char *resolve_address(const char *page, const char *address) {
	if (!address)
		return NULL;
	g_autoptr(GUri) base = g_uri_parse(page, G_URI_FLAGS_NONE, NULL);
	g_autoptr(GUri) resolved = base ? g_uri_parse_relative(base, address, G_URI_FLAGS_NONE, NULL) : NULL;
	g_autofree char *text = resolved ? g_uri_to_string(resolved) : NULL;
	return text && g_str_has_prefix(text, "https://") ? g_steal_pointer(&text) : NULL;
}


static LinkPreview *read_preview(const char *url, const char *head) {
	static const char *const TITLES[] = {"og:title", "twitter:title", NULL};
	static const char *const DESCRIPTIONS[] = {"og:description", "twitter:description", "description", NULL};
	static const char *const IMAGES[] = {"og:image", "og:image:url", "twitter:image", NULL};
	static const char *const SITES[] = {"og:site_name", NULL};
	g_autofree char *title = read_meta(head, TITLES);
	if (!title)
		title = read_title(head);
	if (!title || !*title)
		return NULL;
	LinkPreview *preview = g_new0(LinkPreview, 1);
	preview->url = g_strdup(url);
	preview->title = g_steal_pointer(&title);
	preview->description = read_meta(head, DESCRIPTIONS);
	g_autofree char *image = read_meta(head, IMAGES);
	preview->image_url = resolve_address(url, image);
	preview->site = read_meta(head, SITES);
	if (!preview->site) {
		g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
		const char *host = uri ? g_uri_get_host(uri) : NULL;
		preview->site = g_strdup(host && g_str_has_prefix(host, "www.") ? host + 4 : host);
	}
	return preview;
}


/* The preview is kept, even when the page had none, and everyone still waiting for it hears */
static void finish_page(PageFetch *fetch, LinkPreview *preview) {
	if (g_hash_table_size(previews) >= KEPT_PREVIEWS)
		g_hash_table_remove_all(previews);
	g_hash_table_replace(previews, g_strdup(fetch->url), preview);
	Waiters *waiters = NULL;
	g_hash_table_steal_extended(waiting, fetch->url, NULL, (gpointer *)&waiters);
	for (guint index = 0; waiters && index < waiters->owners->len; index++) {
		g_autoptr(GObject) owner = g_weak_ref_get(g_ptr_array_index(waiters->owners, index));
		if (owner)
			waiters->ready(owner);
	}
	if (waiters)
		free_waiters(waiters);
	g_free(fetch->url);
	g_clear_object(&fetch->stream);
	g_byte_array_unref(fetch->head);
	g_free(fetch);
}


static void read_more(PageFetch *fetch);


static void finish_reading(GObject *source, GAsyncResult *result, gpointer data) {
	PageFetch *fetch = data;
	g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, NULL);
	gsize size = bytes ? g_bytes_get_size(bytes) : 0;
	if (size)
		g_byte_array_append(fetch->head, g_bytes_get_data(bytes, NULL), (guint)size);
	gboolean has_head_end = fetch->head->len && g_strstr_len((const char *)fetch->head->data, fetch->head->len,
		"</head>");
	if (size && !has_head_end && fetch->head->len < LARGEST_HEAD_BYTES) {
		read_more(fetch);
		return;
	}
	g_byte_array_append(fetch->head, (const guint8 *)"", 1);
	g_autofree char *head = g_utf8_make_valid((const char *)fetch->head->data, -1);
	finish_page(fetch, read_preview(fetch->url, head));
}


static void read_more(PageFetch *fetch) {
	g_input_stream_read_bytes_async(fetch->stream, READ_CHUNK_BYTES, G_PRIORITY_LOW, NULL, finish_reading, fetch);
}


static void finish_asking(GObject *source, GAsyncResult *result, gpointer data) {
	PageFetch *fetch = data;
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	fetch->stream = soup_session_send_finish(SOUP_SESSION(source), result, NULL);
	const char *type = message ? soup_message_headers_get_content_type(soup_message_get_response_headers(message),
		NULL) : NULL;
	gboolean is_page = fetch->stream && message && soup_message_get_status(message) == SOUP_STATUS_OK
		&& g_strcmp0(type, "text/html") == 0;
	if (!is_page) {
		finish_page(fetch, NULL);
		return;
	}
	read_more(fetch);
}


void preload_link_preview(const char *url, ImageReady ready, GObject *owner) {
	ensure_tables();
	if (g_hash_table_contains(previews, url))
		return;
	Waiters *waiters = g_hash_table_lookup(waiting, url);
	gboolean is_fetching = waiters != NULL;
	if (!waiters) {
		waiters = g_new0(Waiters, 1);
		waiters->ready = ready;
		waiters->owners = g_ptr_array_new_with_free_func((GDestroyNotify)free_weak_reference);
		g_hash_table_insert(waiting, g_strdup(url), waiters);
	}
	GWeakRef *reference = g_new0(GWeakRef, 1);
	g_weak_ref_init(reference, owner);
	g_ptr_array_add(waiters->owners, reference);
	if (is_fetching)
		return;
	PageFetch *fetch = g_new0(PageFetch, 1);
	fetch->url = g_strdup(url);
	fetch->head = g_byte_array_new();
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, url);
	if (!message) {
		finish_page(fetch, NULL);
		return;
	}
	soup_session_send_async(get_web_session(), message, G_PRIORITY_LOW, NULL, finish_asking, fetch);
}
