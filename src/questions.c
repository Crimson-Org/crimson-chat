#include <string.h>

#include "questions.h"

/* How long a server may take to answer a command before its replies stop counting as the answer */
#define QUESTION_WAIT_SECONDS 60
/* How long after the replies end, or after a message went out, a reply about the same person or room still answers
 * it, as when a service or another server of the network answers late */
#define LATE_ANSWER_SECONDS 30


void free_question(gpointer data) {
	Question *question = data;
	g_free(question->label);
	g_free(question->target);
	g_free(question);
}


static void end_question(Question *question) {
	question->is_answered = TRUE;
	question->until = g_get_monotonic_time() + LATE_ANSWER_SECONDS * G_USEC_PER_SEC;
}


static Question *find_labeled(NetworkWork *work, const char *label) {
	for (guint index = 0; label && index < work->questions->len; index++) {
		Question *question = g_ptr_array_index(work->questions, index);
		if (g_str_equal(question->label, label))
			return question;
	}
	return NULL;
}


static void forget_old_questions(NetworkWork *work) {
	gint64 now = g_get_monotonic_time();
	for (guint index = work->questions->len; index > 0; index--) {
		Question *question = g_ptr_array_index(work->questions, index - 1);
		if (question != work->asking && now > question->until)
			g_ptr_array_remove_index(work->questions, index - 1);
	}
}


/* A server without labels answers lines in order, so a PING before and after what the user sent puts its replies
 * between the two PONGs */
static void send_question_ping(CrimsonNetwork *network, Question *question) {
	send_irc_client_command(network->client, "PING", question->label, NULL);
}


void start_question(CrimsonNetwork *network, CrimsonRoom *room, gboolean waits_for_replies) {
	NetworkWork *work = get_network_work(network);
	if (work->asking_depth++)
		return;
	forget_old_questions(work);
	Question *question = g_new0(Question, 1);
	question->label = create_irc_identifier();
	question->was_typed_in_server = room && room->kind == ROOM_SERVER;
	question->until = g_get_monotonic_time() + QUESTION_WAIT_SECONDS * G_USEC_PER_SEC;
	g_ptr_array_add(work->questions, question);
	work->asking = question;
	if (!waits_for_replies || !network->client)
		end_question(question);
	else if (can_network(network, "labeled-response"))
		label_irc_client_lines(network->client, question->label);
	else
		send_question_ping(network, question);
}


void finish_question(CrimsonNetwork *network) {
	NetworkWork *work = get_network_work(network);
	if (!work->asking_depth || --work->asking_depth)
		return;
	Question *question = work->asking;
	work->asking = NULL;
	if (question->is_answered)
		return;
	if (can_network(network, "labeled-response"))
		label_irc_client_lines(network->client, NULL);
	else
		send_question_ping(network, question);
}


void note_user_target(CrimsonNetwork *network, const char *target) {
	if (!target || !*target)
		return;
	/* Sending them something is the user trying again, so the app may too */
	forgive_target(network, target);
	NetworkWork *work = get_network_work(network);
	if (work->asking && !work->asking->target)
		work->asking->target = fold_network_name(network, target);
}


/* Who a reply is about: the name a numeric gives after the user's nick, or who sent a message */
static char *read_reply_subject(CrimsonNetwork *network, const IrcMessage *message) {
	const char *command = message->command;
	if (g_ascii_isdigit(*command)) {
		const char *subject = get_irc_parameter(message, 1);
		return subject ? fold_network_name(network, subject) : NULL;
	}
	if (!message->source || (!g_str_equal(command, "PRIVMSG") && !g_str_equal(command, "NOTICE")))
		return NULL;
	g_autofree char *sender = get_source_nick(message->source);
	return fold_network_name(network, sender);
}


Question *find_question(CrimsonNetwork *network, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	forget_old_questions(work);
	const char *label = get_irc_tag(message, "label");
	const char *batch = get_irc_tag(message, "batch");
	Question *labeled = find_labeled(work, label ? label : batch ? g_hash_table_lookup(work->answer_batches, batch)
		: NULL);
	if (labeled)
		return labeled;
	for (guint index = 0; index < work->questions->len; index++) {
		Question *question = g_ptr_array_index(work->questions, index);
		if (question->pongs == 1)
			return question;
	}
	g_autofree char *subject = read_reply_subject(network, message);
	for (guint index = work->questions->len; subject && index > 0; index--) {
		Question *question = g_ptr_array_index(work->questions, index - 1);
		if (g_strcmp0(question->target, subject) == 0)
			return question;
	}
	return NULL;
}


void follow_answers(CrimsonNetwork *network, const IrcMessage *message) {
	NetworkWork *work = get_network_work(network);
	const char *command = message->command;
	const char *label = get_irc_tag(message, "label");
	const char *reference = g_str_equal(command, "BATCH") ? get_irc_parameter(message, 0) : NULL;
	if (reference && *reference == '+' && label
		&& g_strcmp0(get_irc_parameter(message, 1), "labeled-response") == 0) {
		g_hash_table_insert(work->answer_batches, g_strdup(reference + 1), g_strdup(label));
		return;
	}
	if (reference && *reference == '-') {
		Question *question = find_labeled(work, g_hash_table_lookup(work->answer_batches, reference + 1));
		if (question)
			end_question(question);
		g_hash_table_remove(work->answer_batches, reference + 1);
		return;
	}
	if (g_str_equal(command, "PONG")) {
		Question *question = find_labeled(work, get_last_irc_parameter(message));
		if (question && ++question->pongs == 2)
			end_question(question);
		return;
	}
	/* A labeled line outside a batch is the full answer, and ACK means there is none */
	Question *question = label && !get_irc_tag(message, "batch") ? find_labeled(work, label) : NULL;
	if (question)
		end_question(question);
}
