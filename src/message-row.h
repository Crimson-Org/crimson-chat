#pragma once

#include <adwaita.h>

#include "model.h"

/* What a message bubble asks the conversation to do */
typedef struct {
	void (*reply)(CrimsonMessage *message, gpointer data);
	void (*react)(CrimsonMessage *message, const char *reaction, gpointer data);
	void (*remove)(CrimsonMessage *message, gpointer data);
	/* Sends again a message the server never took */
	void (*resend)(CrimsonMessage *message, gpointer data);
	void (*show_message)(const char *message_id, gpointer data);
	/* A click on a person's name or picture opens their profile, and a right click their menu */
	void (*show_person)(const char *nick, gpointer data);
	void (*person_menu)(GtkWidget *anchor, const char *nick, double x, double y, gpointer data);
	/* A click on a note that offers help runs its window action for the room's server */
	void (*run_note_action)(const char *action, gpointer data);
	gpointer data;
} MessageActions;

#define MESSAGE_AVATAR_SIZE 34

GtkWidget *create_message_row(void);
/* Where a row sits among its neighbours, which decides its group, day line and merged notes */
typedef struct {
	CrimsonMessage *previous;
	gboolean starts_unread, ends_group;
	/* An event run's notes one per line on its first row, and NULL on the rest, which draw nothing */
	const char *notes;
	/* What clicking the run's pill does, from its newest event that offers something, or NULL */
	const char *action;
} RowPlace;

/* Rebuilds the row for a message, joining it to the group above it when the same person wrote both, with a line
 * above it where the messages not yet read start */
void fill_message_row(GtkWidget *row, CrimsonMessage *message, const RowPlace *place, CrimsonRoom *room,
	const MessageActions *actions);
/* Events following each other on the same day show as one pill on the run's first row */
gboolean continues_note_run(CrimsonMessage *message, CrimsonMessage *previous, gboolean starts_unread);
/* The new messages start a group of their own, as does another person, a long pause or a new day */
gboolean starts_message_group(CrimsonMessage *message, CrimsonMessage *previous, gboolean starts_unread);
/* Where a row of someone else's message holds its avatar, the avatar itself on a group's first row and an empty
 * space of its size on the rest, or NULL on rows without avatars */
GtkWidget *find_avatar_slot(GtkWidget *row);
gboolean is_group_start(GtkWidget *row);
/* Writes the row's day heading again, such as Today turning into Yesterday after midnight */
void refresh_day_heading(GtkWidget *row);
