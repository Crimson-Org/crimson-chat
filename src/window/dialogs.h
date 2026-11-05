#pragma once

#include "window/window.h"

/* Every dialog that holds a page, such as settings, a profile, a guide or a list to pick from, has one size, so they
 * all match */
#define DIALOG_WIDTH 460
#define DIALOG_HEIGHT 660
/* The picture or emblem that lies half over a banded page's band, so a new size changes the 48 px in style.css too */
#define DIALOG_PICTURE_SIZE 96

/* A dialog page that opens on a band in the accent's colours behind the header, with the picture over the band's
 * lower edge, the details under it scrolling when they do not fit, and a button such as Leave Room kept at the bottom
 * when there is one, so the space above it reads as the page's own */
GtkWidget *create_banded_page(GtkWidget *header, GtkWidget *picture, GtkWidget *details, GtkWidget *footer);
/* The picture of a banded page without a person or room, such as a guide's step, its icon on a disc */
GtkWidget *create_dialog_emblem(const char *icon);
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
