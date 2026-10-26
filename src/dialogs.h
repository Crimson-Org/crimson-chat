#pragma once

#include "window.h"

/* The header every settings dialog shares: Cancel closes it untouched, and nothing changes until the suggested
 * button at the other end is pressed */
GtkWidget *create_form_header(AdwDialog *dialog, const char *action, GtkWidget **save);
/* Edits or removes the login of a bouncer */
void show_bouncer_dialog(Window *window, Session *editing);
/* Edits a direct server, or a network the bouncer keeps */
void show_network_settings(Window *window, CrimsonNetwork *network);
void show_message_dialog(Window *window, CrimsonNetwork *network);
/* Asks for an away message, then marks the user away with it on all networks */
void show_away_dialog(Window *window);
/* Asks for the password of a room that refused the user without it, then joins with it */
void ask_room_password(Window *window, CrimsonNetwork *network, const char *room_name);
/* Picks someone to invite to the room, the same way as someone to message */
void show_room_invite_dialog(Window *window, CrimsonRoom *room);
void ask_certificate_trust(Window *window, Session *session, const char *hash);
void confirm_network_removal(Window *window, CrimsonNetwork *network);
/* Forgets the bouncer login, leaving its servers on the bouncer */
void confirm_bouncer_disconnect(Window *window, Session *session);
void forget_network_in_window(Window *window, CrimsonNetwork *network);
/* Every link the user clicks shows its address and opens in the browser only once they agree */
void confirm_open_link(GtkWidget *source, const char *url);
/* Makes the links in a label with markup ask first */
void ask_before_label_links(GtkWidget *label);
