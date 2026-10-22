#pragma once

#include "window.h"

/* Edits or removes the login of a bouncer */
void show_bouncer_dialog(Window *window, Session *editing);
/* Edits a direct server, or a network the bouncer keeps */
void show_network_settings(Window *window, CrimsonNetwork *network);
void show_join_dialog(Window *window, CrimsonNetwork *network);
void show_message_dialog(Window *window, CrimsonNetwork *network);
void ask_certificate_trust(Window *window, Session *session, const char *hash);
void confirm_network_removal(Window *window, CrimsonNetwork *network);
void confirm_account_removal(Window *window, Session *session);
void forget_account_in_window(Window *window, Session *session);
