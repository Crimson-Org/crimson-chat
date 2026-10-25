#pragma once

#include "window.h"

/* Hears which found message the user picked */
typedef void (*MessagePicked)(CrimsonMessage *message, gpointer data);

/* A dialog that searches a room's messages as the user types, listing the newest first */
void show_message_search(Window *window, CrimsonRoom *room, MessagePicked picked, gpointer data);
