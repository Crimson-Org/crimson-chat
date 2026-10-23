#include <string.h>

#include "bouncer.h"

#define NAMES_LINE_BYTES 400


static char *describe_own_source(IrcState *state, const char *nick) {
	IrcUser *self = find_irc_user(state, nick);
	if (self && self->user && self->host)
		return g_strdup_printf("%s!%s@%s", nick, self->user, self->host);
	return g_strdup(nick);
}


static void send_join(Downstream *downstream, IrcState *state, IrcChannel *channel) {
	g_autoptr(IrcMessage) join = create_irc_message("JOIN", channel->name, NULL);
	join->source = describe_own_source(state, downstream->nick);
	IrcUser *self = find_irc_user(state, downstream->nick);
	add_irc_parameter(join, self && self->account ? self->account : "*");
	add_irc_parameter(join, self && self->realname ? self->realname : downstream->nick);
	send_to_downstream(downstream, join);
}


static void send_topic(Downstream *downstream, IrcChannel *channel) {
	if (!channel->topic)
		return;
	send_downstream_numeric(downstream, "332", channel->name, channel->topic, NULL);
	if (!channel->topic_setter)
		return;
	g_autofree char *time = g_strdup_printf("%" G_GINT64_FORMAT, channel->topic_time);
	send_downstream_numeric(downstream, "333", channel->name, channel->topic_setter, time, NULL);
}


/* NAMES entries carry every status symbol and the full source, and sending them shapes them for the client */
static void send_names(Downstream *downstream, IrcState *state, IrcChannel *channel) {
	GString *line = g_string_new(NULL);
	GHashTableIter iterator;
	gpointer folded, symbols;
	g_hash_table_iter_init(&iterator, channel->members);
	while (TRUE) {
		gboolean has_more = g_hash_table_iter_next(&iterator, &folded, &symbols);
		if (line->len && (!has_more || line->len > NAMES_LINE_BYTES)) {
			send_downstream_numeric(downstream, "353", "=", channel->name, line->str, NULL);
			g_string_truncate(line, 0);
		}
		if (!has_more)
			break;
		IrcUser *user = g_hash_table_lookup(state->users, folded);
		if (!user)
			continue;
		g_string_append_printf(line, "%s%s%s", line->len ? " " : "", (const char *)symbols, user->nick);
		if (user->user && user->host)
			g_string_append_printf(line, "!%s@%s", user->user, user->host);
	}
	g_string_free(line, TRUE);
	send_downstream_numeric(downstream, "366", channel->name, "End of /NAMES list", NULL);
}


static void send_metadata_table(Downstream *downstream, const char *target, GHashTable *metadata,
	const char *batch) {
	GHashTableIter iterator;
	gpointer key, value;
	g_hash_table_iter_init(&iterator, metadata);
	while (g_hash_table_iter_next(&iterator, &key, &value)) {
		g_autoptr(IrcMessage) message = create_irc_message("METADATA", target, key, "*", value, NULL);
		message->source = g_strdup(BOUNCER_NAME);
		if (batch)
			set_irc_tag(message, "batch", batch);
		send_to_downstream(downstream, message);
	}
}


/* Avatars and display names of the channel and its members, in a metadata batch as a server sends after JOIN */
static void send_channel_metadata(Downstream *downstream, IrcState *state, IrcChannel *channel) {
	if (!has_downstream_cap(downstream, "draft/metadata-2") || !has_downstream_cap(downstream, "batch"))
		return;
	g_autofree char *reference = create_irc_identifier();
	g_autofree char *opening = g_strconcat("+", reference, NULL);
	g_autofree char *closing = g_strconcat("-", reference, NULL);
	g_autoptr(IrcMessage) start = create_irc_message("BATCH", opening, "metadata", channel->name, NULL);
	start->source = g_strdup(BOUNCER_NAME);
	send_to_downstream(downstream, start);
	send_metadata_table(downstream, channel->name, channel->metadata, reference);
	GHashTableIter iterator;
	gpointer folded;
	g_hash_table_iter_init(&iterator, channel->members);
	while (g_hash_table_iter_next(&iterator, &folded, NULL)) {
		IrcUser *user = g_hash_table_lookup(state->users, folded);
		if (user)
			send_metadata_table(downstream, user->nick, user->metadata, reference);
	}
	g_autoptr(IrcMessage) end = create_irc_message("BATCH", closing, NULL);
	end->source = g_strdup(BOUNCER_NAME);
	send_to_downstream(downstream, end);
}


void replay_upstream(Downstream *downstream) {
	IrcState *state = get_irc_client_state(downstream->upstream->client);
	if (!state->is_registered)
		return;
	/* The client's own login was to the bouncer, so it learns which account, if any, the network knows it as */
	g_autofree char *mask = g_strdup_printf("%s!%s@%s", state->nick, state->user ? state->user : "*",
		state->host ? state->host : "*");
	if (state->account) {
		g_autofree char *text = g_strdup_printf("You are now logged in as %s", state->account);
		send_downstream_numeric(downstream, "900", mask, state->account, text, NULL);
	} else {
		send_downstream_numeric(downstream, "901", mask, "You are now logged out", NULL);
	}
	if (state->is_away)
		send_downstream_numeric(downstream, "306", "You have been marked as being away", NULL);
	Database *database = downstream->bouncer->database;
	GHashTableIter iterator;
	gpointer folded, value;
	g_hash_table_iter_init(&iterator, state->channels);
	while (g_hash_table_iter_next(&iterator, &folded, &value)) {
		IrcChannel *channel = value;
		send_join(downstream, state, channel);
		g_autofree char *marker = load_read_marker(database, downstream->upstream->record.id, folded);
		send_read_marker(downstream, channel->name, marker);
		send_topic(downstream, channel);
		send_names(downstream, state, channel);
		send_channel_metadata(downstream, state, channel);
	}
}
