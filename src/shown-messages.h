#pragma once

#include <gio/gio.h>

/* A room's messages from the earliest one the chat shows onward. The chat builds a row for each message it holds,
 * so messages added before the earliest shown one stay hidden until revealed, a few at a time */
#define CRIMSON_TYPE_SHOWN_MESSAGES (crimson_shown_messages_get_type())
G_DECLARE_FINAL_TYPE(CrimsonShownMessages, crimson_shown_messages, CRIMSON, SHOWN_MESSAGES, GObject)

/* It starts with the latest shown_count messages, and a room with nothing shown takes its latest latest_count */
CrimsonShownMessages *create_shown_messages(GListModel *messages, guint shown_count, guint latest_count);
guint count_hidden_messages(CrimsonShownMessages *shown);
/* Shows up to count of the hidden messages just before the earliest shown one */
void reveal_hidden_messages(CrimsonShownMessages *shown, guint count);
/* Shows all messages from position in the room onward */
void reveal_messages_from(CrimsonShownMessages *shown, guint position);
