#pragma once

#include "window.h"

/* A person's profile from WHOIS, with their name, account, address, server, rooms and idle time, and a button to
 * message them */
void show_profile(Window *window, CrimsonRoom *room, const char *nick);
/* The user's own profile on a server: nickname, display name, real name and picture, each marked when the server
 * does not support it */
void show_own_profile(Window *window, CrimsonNetwork *network);
