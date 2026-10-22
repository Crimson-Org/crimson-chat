#pragma once

#include "window.h"

/* Edits or removes the login of a bouncer */
void show_bouncer_dialog(Window *window, Session *editing);
/* Edits a direct server, or a network the bouncer keeps */
void show_network_settings(Window *window, CrimsonNetwork *network);
void show_message_dialog(Window *window, CrimsonNetwork *network);
void ask_certificate_trust(Window *window, Session *session, const char *hash);
void confirm_network_removal(Window *window, CrimsonNetwork *network);
/* Forgets the bouncer login, leaving its servers on the bouncer */
void confirm_bouncer_disconnect(Window *window, Session *session);
void forget_network_in_window(Window *window, CrimsonNetwork *network);
