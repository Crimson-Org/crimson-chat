#pragma once

#include "window.h"

/* A person's profile from WHOIS, with their name, account, address, server, rooms and idle time, and a button to
 * message them */
void show_profile(Window *window, CrimsonRoom *room, const char *nick);
/* How long ago someone idle for this long was last active, such as "5 minutes ago" */
char *describe_idle(gint64 seconds);
/* A room's own page, laid out like a person's profile: its picture, name, topic and details, with its members and
 * leaving at hand; showing the members calls back into the open conversation */
void show_room_details(Window *window, CrimsonRoom *room, GCallback show_members, gpointer data);
/* The user's own profile on a server: nickname, display name, real name and picture, each marked when the server
 * does not support it */
void show_own_profile(Window *window, CrimsonNetwork *network);
