#pragma once

#include <adwaita.h>

#include "common/tray.h"
#include "common/window-geometry.h"
#include "session.h"

typedef struct Window Window;
typedef struct RoomList RoomList;
typedef struct Conversation Conversation;

/* The main window: a rail of networks, the rooms of the chosen one and the open conversation */
struct Window {
	AdwApplicationWindow *window;
	Chat *chat;
	AdwToastOverlay *toasts;
	AdwNavigationSplitView *split;
	GtkWidget *rail, *rail_list, *all_button;
	RoomList *room_list;
	Conversation *conversation;
	/* The server or bouncer the room list shows, or NULL for every chat */
	Session *account;
	gboolean is_narrow, is_quitting;
	guint refresh_source;
	Tray *tray;
	WindowGeometry geometry;
};

Window *create_window(GtkApplication *application, Chat *chat);
Window *get_main_window(void);
void open_room(Window *window, CrimsonRoom *room);
CrimsonRoom *get_open_room(Window *window);
void choose_account(Window *window, Session *account);
void show_toast(Window *window, const char *text);
/* Redraws badges, the room order and the open conversation once the current changes are done */
void refresh_window_later(Window *window);
/* Hides the window in the tray when there is one, so the chats stay connected */
void hide_window(Window *window);
gboolean is_room_in_view(Window *window, CrimsonRoom *room);
/* Opens the network and room from the last run, once the chats from the last run are back */
void restore_window_choice(Window *window);
GMenuModel *create_main_menu(void);
/* An unread badge, crimson for a mention or a private message and grey for ordinary chatter */
void style_unread_badge(GtkWidget *badge, guint count, gboolean has_mention);

/* Room list */
RoomList *create_room_list(Window *window);
GtkWidget *get_room_list_widget(RoomList *list);
void show_room_list_account(RoomList *list, Session *account);
void select_listed_room(RoomList *list, CrimsonRoom *room);
void resort_room_list(RoomList *list);
void set_room_list_narrow(RoomList *list, gboolean is_narrow);
void refresh_room_list_header(RoomList *list);
void focus_room_search(RoomList *list);
/* The room above or below the chosen one in the list, for keyboard stepping */
CrimsonRoom *step_listed_room(RoomList *list, int step);

/* Conversation */
Conversation *create_conversation(Window *window);
GtkWidget *get_conversation_widget(Conversation *conversation);
void show_conversation_room(Conversation *conversation, CrimsonRoom *room);
CrimsonRoom *get_conversation_room(Conversation *conversation);
void refresh_conversation(Conversation *conversation);
void set_conversation_narrow(Conversation *conversation, gboolean is_narrow);
void focus_composer(Conversation *conversation);
