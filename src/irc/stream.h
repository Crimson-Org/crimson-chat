#pragma once

#include <gio/gio.h>

#include "message.h"

/* A connection that reads and writes protocol lines, answers PING and notices a silent peer */
typedef struct IrcStream IrcStream;

typedef void (*IrcStreamReceived)(IrcStream *stream, IrcMessage *message, gpointer data);
/* Called once when the peer closes the connection or it fails, after which the owner closes the stream */
typedef void (*IrcStreamClosed)(IrcStream *stream, const char *reason, gpointer data);

IrcStream *create_irc_stream(GIOStream *connection, IrcStreamReceived received, IrcStreamClosed closed,
	gpointer data);
/* Servers drop clients that send too fast, so a stream to a server spaces out its lines after a short burst */
void limit_irc_stream_rate(IrcStream *stream);
/* Hands the stream bytes that were read before it existed, such as those used to tell IRC from a web request */
void feed_irc_stream(IrcStream *stream, GBytes *bytes);

void send_irc_message(IrcStream *stream, const IrcMessage *message);
void send_irc_command(IrcStream *stream, const char *command, ...) G_GNUC_NULL_TERMINATED;
/* Writes what is still queued, then closes the connection and releases the stream */
void close_irc_stream(IrcStream *stream);

GIOStream *get_irc_stream_connection(IrcStream *stream);
