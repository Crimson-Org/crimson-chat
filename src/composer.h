#pragma once

#include <adwaita.h>

#include "model.h"

/* The message field under a conversation, with its reply bar, emoji button and send button */
typedef struct Composer Composer;

Composer *create_composer(void);
GtkWidget *get_composer_widget(Composer *composer);
/* Keeps the unsent text of the room being left and brings back the one of the room being opened */
void switch_composer_room(Composer *composer, CrimsonRoom *room);
void reply_in_composer(Composer *composer, CrimsonMessage *message);
void focus_composer_field(Composer *composer);
/* Puts a person's nick into the message, at the start as "Nick: " or where the cursor is */
void mention_in_composer(Composer *composer, const char *nick);
