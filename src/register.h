#pragma once

#include "window.h"

/* A dialog that registers the user's nickname as an account on a server, asking for a code where the server sends
 * one, and saves the login once done */
void show_registration(Window *window, CrimsonNetwork *network);
