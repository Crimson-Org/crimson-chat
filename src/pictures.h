#pragma once

#include "window.h"

/* Pictures the user chose for people, rooms and servers, kept on this computer and shown instead of the ones the
 * server offers */
typedef enum {
	PICTURE_SERVER,
	PICTURE_ROOM,
	PICTURE_PERSON,
} PictureKind;

/* The address of the user's own picture, or NULL when they chose none */
const char *find_own_picture(CrimsonNetwork *network, PictureKind kind, const char *name);
/* The picture to show: the user's own, or else the one the server offers */
const char *pick_network_picture(CrimsonNetwork *network);
const char *pick_person_picture(CrimsonRoom *room, const char *nick);
/* Clicking the avatar calls change with data, and pointing at it shows a picture sign in its corner; returns the
 * widget to place where the avatar would go */
GtkWidget *offer_picture_change(GtkWidget *avatar, GCallback change, gpointer data);
/* Clicking the avatar chooses a picture from disk, and pointing at it shows a picture sign and, once the user chose
 * one, a button to go back to the server's; returns the widget to place where the avatar would go */
GtkWidget *offer_picture_choice(GtkWidget *avatar, Window *window, CrimsonNetwork *network, PictureKind kind,
	const char *name);
