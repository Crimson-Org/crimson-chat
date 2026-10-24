#pragma once

#include "model.h"

/* Where the bouncer behind a server takes uploaded pictures, or NULL where it does not */
char *find_avatar_upload_url(CrimsonNetwork *network);

/* Hears the address the picture got, or why it failed */
typedef void (*UploadDone)(const char *url, const char *problem, gpointer data);

/* Sends a picture to the bouncer, which shapes it and sets it as the avatar of the user, with "*", or of a room */
void upload_avatar(CrimsonNetwork *network, const char *target, GFile *file, UploadDone done, gpointer data);

/* Where the server takes files shared in chats, as soju's file host extension names it, or NULL */
char *find_file_upload_url(CrimsonNetwork *network);

/* Shows the file as a card in the room while it uploads, then sends a line naming it with its size and address */
void share_file(CrimsonRoom *room, GFile *file);
