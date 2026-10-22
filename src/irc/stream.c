#include <stdarg.h>
#include <string.h>

#include "stream.h"

#define READ_CHUNK_BYTES 4096
/* A peer that sends this much without ending a line is broken or hostile */
#define LONGEST_LINE_BYTES 65536
#define KEEPALIVE_CHECK_SECONDS 30
#define PING_AFTER_SECONDS 60
#define GIVE_UP_AFTER_SECONDS 150
#define KEEPALIVE_TOKEN "crimson-keepalive"
#define BURST_LINES 5.0
#define LINES_PER_SECOND 1.0
#define CLOSE_DEADLINE_SECONDS 5

struct IrcStream {
	gint references;
	GIOStream *connection;
	GCancellable *reading, *writing;
	GByteArray *unread;
	guint8 chunk[READ_CHUNK_BYTES];
	GQueue outgoing;
	/* The line being written, which the output stream reads until the write finishes */
	GBytes *current;
	gboolean is_writing, is_closing, has_failed, has_finished, is_ping_sent, is_rate_limited;
	gint64 last_received;
	double send_credit;
	gint64 credit_time;
	guint keepalive_source, rate_source, deadline_source;
	IrcStreamReceived received;
	IrcStreamClosed closed;
	gpointer data;
};

static void read_more(IrcStream *stream);
static void write_next(IrcStream *stream);


static IrcStream *hold_stream(IrcStream *stream) {
	g_atomic_int_inc(&stream->references);
	return stream;
}


static void release_stream(IrcStream *stream) {
	if (!g_atomic_int_dec_and_test(&stream->references))
		return;
	g_clear_handle_id(&stream->keepalive_source, g_source_remove);
	g_clear_handle_id(&stream->rate_source, g_source_remove);
	g_clear_handle_id(&stream->deadline_source, g_source_remove);
	g_queue_clear_full(&stream->outgoing, (GDestroyNotify)g_bytes_unref);
	g_clear_pointer(&stream->current, g_bytes_unref);
	g_byte_array_unref(stream->unread);
	g_object_unref(stream->reading);
	g_object_unref(stream->writing);
	g_object_unref(stream->connection);
	g_free(stream);
}


static void fail_stream(IrcStream *stream, const char *reason) {
	if (stream->has_failed || stream->is_closing)
		return;
	stream->has_failed = TRUE;
	g_clear_handle_id(&stream->keepalive_source, g_source_remove);
	stream->closed(stream, reason, stream->data);
}


/* Old clients still send Latin-1, which is what a line that is not UTF-8 almost always is */
static char *decode_line(const char *line, gsize length) {
	if (g_utf8_validate_len(line, length, NULL))
		return g_strndup(line, length);
	char *converted = g_convert(line, (gssize)length, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
	return converted ? converted : g_utf8_make_valid(line, (gssize)length);
}


static void handle_line(IrcStream *stream, const char *line, gsize length) {
	g_autofree char *text = decode_line(line, length);
	g_autoptr(IrcMessage) message = parse_irc_line(text);
	if (!message)
		return;
	stream->last_received = g_get_monotonic_time();
	stream->is_ping_sent = FALSE;
	if (g_str_equal(message->command, "PING")) {
		g_autoptr(IrcMessage) answer = copy_irc_message(message);
		g_free(answer->command);
		answer->command = g_strdup("PONG");
		g_clear_pointer(&answer->source, g_free);
		g_hash_table_remove_all(answer->tags);
		send_irc_message(stream, answer);
		return;
	}
	if (g_str_equal(message->command, "PONG"))
		return;
	stream->received(stream, message, stream->data);
}


static void take_lines(IrcStream *stream) {
	while (!stream->is_closing && !stream->has_failed) {
		guint8 *end = memchr(stream->unread->data, '\n', stream->unread->len);
		if (!end)
			break;
		gsize length = (gsize)(end - stream->unread->data);
		gsize line_length = length > 0 && stream->unread->data[length - 1] == '\r' ? length - 1 : length;
		handle_line(stream, (const char *)stream->unread->data, line_length);
		g_byte_array_remove_range(stream->unread, 0, (guint)length + 1);
	}
	if (stream->unread->len > LONGEST_LINE_BYTES)
		fail_stream(stream, "Line too long");
}


static void finish_reading(GObject *source, GAsyncResult *result, gpointer data) {
	IrcStream *stream = data;
	g_autoptr(GError) error = NULL;
	gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
	if (!stream->is_closing) {
		if (count < 0) {
			fail_stream(stream, error->message);
		} else if (count == 0) {
			fail_stream(stream, "Connection closed");
		} else {
			g_byte_array_append(stream->unread, stream->chunk, (guint)count);
			take_lines(stream);
			read_more(stream);
		}
	}
	release_stream(stream);
}


static void read_more(IrcStream *stream) {
	if (stream->is_closing || stream->has_failed)
		return;
	g_input_stream_read_async(g_io_stream_get_input_stream(stream->connection), stream->chunk, sizeof stream->chunk,
		G_PRIORITY_DEFAULT, stream->reading, finish_reading, hold_stream(stream));
}


static void finish_closing(GObject *source, GAsyncResult *result, gpointer data) {
	g_io_stream_close_finish(G_IO_STREAM(source), result, NULL);
	release_stream(data);
}


static void close_connection(IrcStream *stream) {
	if (stream->has_finished)
		return;
	stream->has_finished = TRUE;
	g_clear_handle_id(&stream->deadline_source, g_source_remove);
	g_cancellable_cancel(stream->writing);
	g_io_stream_close_async(stream->connection, G_PRIORITY_DEFAULT, NULL, finish_closing, stream);
}


static void finish_writing(GObject *source, GAsyncResult *result, gpointer data) {
	IrcStream *stream = data;
	g_autoptr(GError) error = NULL;
	stream->is_writing = FALSE;
	g_clear_pointer(&stream->current, g_bytes_unref);
	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, &error))
		fail_stream(stream, error->message);
	if (stream->is_closing && (error || g_queue_is_empty(&stream->outgoing)))
		close_connection(stream);
	else if (!error)
		write_next(stream);
	release_stream(stream);
}


static gboolean resume_writing(gpointer data) {
	IrcStream *stream = data;
	stream->rate_source = 0;
	write_next(stream);
	return G_SOURCE_REMOVE;
}


/* Returns whether a line may go now, and otherwise schedules the next try */
static gboolean take_send_credit(IrcStream *stream) {
	if (!stream->is_rate_limited || stream->is_closing)
		return TRUE;
	gint64 now = g_get_monotonic_time();
	stream->send_credit += (double)(now - stream->credit_time) / G_USEC_PER_SEC * LINES_PER_SECOND;
	stream->send_credit = MIN(stream->send_credit, BURST_LINES);
	stream->credit_time = now;
	if (stream->send_credit >= 1.0) {
		stream->send_credit -= 1.0;
		return TRUE;
	}
	if (!stream->rate_source) {
		guint wait = (guint)((1.0 - stream->send_credit) / LINES_PER_SECOND * 1000) + 1;
		stream->rate_source = g_timeout_add(wait, resume_writing, stream);
	}
	return FALSE;
}


static void write_next(IrcStream *stream) {
	if (stream->is_writing || stream->has_finished || stream->has_failed || g_queue_is_empty(&stream->outgoing))
		return;
	if (!take_send_credit(stream))
		return;
	stream->current = g_queue_pop_head(&stream->outgoing);
	gsize length = 0;
	const void *bytes = g_bytes_get_data(stream->current, &length);
	stream->is_writing = TRUE;
	g_output_stream_write_all_async(g_io_stream_get_output_stream(stream->connection), bytes, length,
		G_PRIORITY_DEFAULT, stream->writing, finish_writing, hold_stream(stream));
}


static gboolean check_keepalive(gpointer data) {
	IrcStream *stream = data;
	gint64 silent = (g_get_monotonic_time() - stream->last_received) / G_USEC_PER_SEC;
	if (silent >= GIVE_UP_AFTER_SECONDS) {
		stream->keepalive_source = 0;
		fail_stream(stream, "Connection timed out");
		return G_SOURCE_REMOVE;
	}
	if (silent >= PING_AFTER_SECONDS && !stream->is_ping_sent) {
		stream->is_ping_sent = TRUE;
		send_irc_command(stream, "PING", KEEPALIVE_TOKEN, NULL);
	}
	return G_SOURCE_CONTINUE;
}


IrcStream *create_irc_stream(GIOStream *connection, IrcStreamReceived received, IrcStreamClosed closed,
	gpointer data) {
	IrcStream *stream = g_new0(IrcStream, 1);
	stream->references = 1;
	stream->connection = g_object_ref(connection);
	stream->reading = g_cancellable_new();
	stream->writing = g_cancellable_new();
	stream->unread = g_byte_array_new();
	g_queue_init(&stream->outgoing);
	stream->received = received;
	stream->closed = closed;
	stream->data = data;
	stream->last_received = g_get_monotonic_time();
	stream->send_credit = BURST_LINES;
	stream->credit_time = stream->last_received;
	stream->keepalive_source = g_timeout_add_seconds(KEEPALIVE_CHECK_SECONDS, check_keepalive, stream);
	read_more(stream);
	return stream;
}


void feed_irc_stream(IrcStream *stream, GBytes *bytes) {
	gsize length = 0;
	const guint8 *data = g_bytes_get_data(bytes, &length);
	g_byte_array_append(stream->unread, data, (guint)length);
	take_lines(stream);
}


void limit_irc_stream_rate(IrcStream *stream) {
	stream->is_rate_limited = TRUE;
}


void send_irc_message(IrcStream *stream, const IrcMessage *message) {
	if (stream->is_closing || stream->has_failed)
		return;
	g_autofree char *line = format_irc_line(message);
	g_queue_push_tail(&stream->outgoing, g_bytes_new_take(g_strconcat(line, "\r\n", NULL), strlen(line) + 2));
	write_next(stream);
}


void send_irc_command(IrcStream *stream, const char *command, ...) {
	g_autoptr(IrcMessage) message = create_irc_message(command, NULL);
	va_list parameters;
	va_start(parameters, command);
	for (const char *parameter = va_arg(parameters, const char *); parameter;
		parameter = va_arg(parameters, const char *))
		add_irc_parameter(message, parameter);
	va_end(parameters);
	send_irc_message(stream, message);
}


static gboolean stop_waiting_to_close(gpointer data) {
	IrcStream *stream = data;
	stream->deadline_source = 0;
	close_connection(stream);
	return G_SOURCE_REMOVE;
}


void close_irc_stream(IrcStream *stream) {
	if (!stream || stream->is_closing)
		return;
	stream->is_closing = TRUE;
	g_cancellable_cancel(stream->reading);
	g_clear_handle_id(&stream->keepalive_source, g_source_remove);
	g_clear_handle_id(&stream->rate_source, g_source_remove);
	if (stream->has_failed || (!stream->is_writing && g_queue_is_empty(&stream->outgoing))) {
		close_connection(stream);
		return;
	}
	write_next(stream);
	stream->deadline_source = g_timeout_add_seconds(CLOSE_DEADLINE_SECONDS, stop_waiting_to_close, stream);
}


GIOStream *get_irc_stream_connection(IrcStream *stream) {
	return stream->connection;
}
