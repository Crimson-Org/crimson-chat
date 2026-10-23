#pragma once

#include <adwaita.h>

#include "model.h"

/* How a room presents itself in the list and the conversation header */
void fill_room_avatar(AdwAvatar *avatar, CrimsonRoom *room, int size);
/* The display name from metadata where the room or person has one, and the plain name otherwise */
char *describe_room_title(CrimsonRoom *room);
/* Who is typing, or the latest line without formatting codes */
char *describe_room_preview(CrimsonRoom *room);
/* Shows a room's line in a label, with dots that keep counting up while it says who is typing */
void show_room_line(GtkWidget *label, const char *text, gboolean is_typing);
/* Members and topic for a channel, presence for a private chat and the connection for a server room */
char *describe_room_subtitle(CrimsonRoom *room);
/* When the person in a private chat was last active, counted from what the server last said, or NULL */
char *describe_room_activity(CrimsonRoom *room);
const char *find_metadata(CrimsonRoom *room, const char *nick, const char *key);
