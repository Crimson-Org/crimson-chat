#pragma once

#include <adwaita.h>

#include "model.h"

/* What a message bubble asks the conversation to do */
typedef struct {
	void (*reply)(CrimsonMessage *message, gpointer data);
	void (*react)(CrimsonMessage *message, const char *reaction, gpointer data);
	void (*remove)(CrimsonMessage *message, gpointer data);
	void (*show_message)(const char *message_id, gpointer data);
	void (*open_person)(const char *nick, gpointer data);
	gpointer data;
} MessageActions;

GtkWidget *create_message_row(void);
/* Rebuilds the row for a message, joining it to the group above it when the same person wrote both */
void fill_message_row(GtkWidget *row, CrimsonMessage *message, CrimsonMessage *previous, CrimsonRoom *room,
	const MessageActions *actions);
