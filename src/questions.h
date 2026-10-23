#pragma once

#include "session.h"

void free_question(gpointer question);

/* Starts a question for what the user sends next, from the room they typed in or NULL elsewhere in the window,
 * lasting until finish_question, and for commands waits for every reply instead of only refusals */
void start_question(CrimsonNetwork *network, CrimsonRoom *room, gboolean waits_for_replies);
void finish_question(CrimsonNetwork *network);
/* Names the person or room what the user is sending is about, whose replies answer it even after the rest ended */
void note_user_target(CrimsonNetwork *network, const char *target);
/* The question a line from the server answers, or NULL when the user did not ask for it */
Question *find_question(CrimsonNetwork *network, const IrcMessage *message);
/* Notes where the replies to a question end, from ACK, PONG, labeled batches and single labeled replies */
void follow_answers(CrimsonNetwork *network, const IrcMessage *message);
