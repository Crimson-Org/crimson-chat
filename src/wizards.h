#pragma once

#include "window.h"

/* What an invite link already knows about a bouncer, so its guide can skip to the login */
typedef struct {
	const char *name, *host, *username;
	int port;
} BouncerHint;

/* Walks through adding a server one question at a time, starting from well known IRCv3 networks */
void show_server_guide(Window *window);
/* Walks through adding a bouncer, starting at the login when a hint already holds the address */
void show_bouncer_guide(Window *window, const BouncerHint *hint);
/* Opens the guide a crimson-chat:// link asks for, such as the one on a bouncer's welcome page */
void open_application_link(Window *window, const char *link);
/* Starts what is typed into a nickname row with a capital letter */
void capitalize_nick_field(GtkWidget *row);
/* Creates an invite link on a bouncer the user administers and shows it ready to copy */
void show_invite_dialog(Window *window, CrimsonNetwork *network);
void show_invite_link(Window *window, Session *session, const char *token, const char *uses);
