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
/* Clicking the avatar calls change with data, and its corner button calls change, or remove once
 * show_picture_removable says a picture is set; returns the widget to place where the avatar would go */
GtkWidget *offer_picture_change(GtkWidget *avatar, GCallback change, GCallback remove, gpointer data);
void show_picture_removable(GtkWidget *avatar, gboolean is_removable);
/* Clicking the avatar asks before choosing a picture only the user sees, or one everyone sees where the user may set
 * it, and its corner button removes the picture in place; returns the widget to place where the avatar would go */
GtkWidget *offer_picture_choice(GtkWidget *avatar, Window *window, CrimsonNetwork *network, PictureKind kind,
	const char *name, gboolean waits_for_save);
/* In a form with a Save button, the picture chosen or removed since it opened takes effect only when this is called */
void save_picture_choice(GtkWidget *avatar);
