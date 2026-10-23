#pragma once

#include <adwaita.h>

#include "common/tray.h"
#include "common/window-geometry.h"
#include "session.h"

typedef struct Window Window;
typedef struct RoomList RoomList;
typedef struct Conversation Conversation;

/* The main window: a welcome until there is a server, then a rail of servers when there are several, the rooms
 * of the chosen one and the open conversation */
struct Window {
	AdwApplicationWindow *window;
	Chat *chat;
	AdwToastOverlay *toasts;
	GtkWidget *pages, *welcome, *welcome_bouncer, *welcome_server;
	/* Whether the chat list shows its connection spinner, which waits a moment so a quick connection does not
	 * flash, and the connection it is about */
	gboolean is_connection_shown;
	guint connection_source;
	char *lost_name, *lost_message;
	/* The server whose connection the corner is about, so switching servers does not count as reconnecting */
	CrimsonNetwork *lost_network;
	gboolean is_problem;
	AdwNavigationSplitView *split;
	GtkWidget *rail, *rail_list;
	RoomList *room_list;
	Conversation *conversation;
	/* The server whose rooms the list shows, NULL only while there is none */
	CrimsonNetwork *network;
	/* The server chosen last time, waited for until it is listed, since a bouncer only lists its servers once
	 * connected */
	char *wanted_network;
	/* The rail only shows with more than one server */
	gboolean is_narrow, has_rail, is_quitting;
	guint refresh_source;
	/* Waits out the moment after connecting before offering to register, and the servers whose login offer was
	 * closed while the app runs */
	guint banner_source;
	GHashTable *dismissed_logins;
	/* The server being dragged in the rail, and the row that shows where it would land */
	CrimsonNetwork *dragged_network;
	GtkWidget *drop_line_row;
	Tray *tray;
	WindowGeometry geometry;
};

Window *create_window(GtkApplication *application, Chat *chat);
Window *get_main_window(void);
void open_room(Window *window, CrimsonRoom *room);
CrimsonRoom *get_open_room(Window *window);
/* Closes the open chat, deselects it and leaves the middle empty */
void close_open_room(Window *window);
/* What a server that is not connected waits for, as the rail, the list and the send button say it, or NULL when it
 * is connected */
char *describe_connection_wait(CrimsonNetwork *network);
void choose_network(Window *window, CrimsonNetwork *network);
/* Everything about one server, and the other servers to switch to when there is no rail to show them */
GMenuModel *create_server_menu(Window *window, CrimsonNetwork *network);
/* The menu button for the whole app, which also names the bouncer and holds its settings */
GtkWidget *create_main_menu_button(Window *window);
void show_toast(Window *window, const char *text);
/* Redraws badges, the room order and the open conversation once the current changes are done */
void refresh_window_later(Window *window);
/* A wait only shows once it has lasted this long, so quick ones pass without anything flashing up */
#define WAIT_SHOWN_MS 500
/* Keeps a widget invisible until it has been on screen for WAIT_SHOWN_MS, each time it appears, then calls shown */
void show_after_wait(GtkWidget *widget, GCallback shown, gpointer data);
/* A spinner that only shows once its wait has lasted WAIT_SHOWN_MS */
GtkWidget *create_spinner(void);
/* Hides the window in the tray when there is one, so the chats stay connected */
void hide_window(Window *window);
gboolean is_room_in_view(Window *window, CrimsonRoom *room);
/* Chooses the server from the last run, once the chats from the last run are back */
void restore_window_choice(Window *window);
/* An unread badge, crimson for a mention or a private message and grey for ordinary chatter */
void style_unread_badge(GtkWidget *badge, guint count, gboolean has_mention);

/* Room list */
RoomList *create_room_list(Window *window);
GtkWidget *get_room_list_widget(RoomList *list);
void show_room_list_network(RoomList *list, CrimsonNetwork *network);
void select_listed_room(RoomList *list, CrimsonRoom *room);
void resort_room_list(RoomList *list);
/* An offer at the top of the chat list about the chosen server's account: to register the nickname, or to log in
 * where the server said the nickname is registered */
typedef enum {
	ACCOUNT_BANNER_NONE,
	ACCOUNT_BANNER_REGISTER,
	ACCOUNT_BANNER_LOG_IN,
} AccountBanner;

void show_room_list_banner(RoomList *list, AccountBanner banner, const char *network_key);
/* The list's header, which the side panel places across the rail and the list together */
GtkWidget *get_room_list_header(RoomList *list);
void refresh_room_list_header(RoomList *list);
void focus_room_search(RoomList *list);
/* Lets the room list follow whether the window has focus, for its + button */
void watch_room_list_focus(RoomList *list, GtkWidget *window);
/* What the chat list's corner says about a connection that is down, and what clicking it does */
typedef struct {
	const char *message;
	/* A problem the user has to fix, shown as a warning, where otherwise the app is waiting */
	gboolean is_problem;
	const char *action;
	GVariant *target;
	/* A button in the note that fixes the problem, such as opening the account for a refused login, or NULL */
	const char *fix_label, *fix_action;
	GVariant *fix_target;
} ConnectionNote;

/* Shows the connection note in the list's corner and dims the chats, or hides it with NULL */
void show_room_list_connection(RoomList *list, const ConnectionNote *note);
/* The room above or below the chosen one in the list, for keyboard stepping */
CrimsonRoom *step_listed_room(RoomList *list, int step);

/* Conversation */
Conversation *create_conversation(Window *window);
GtkWidget *get_conversation_widget(Conversation *conversation);
void show_conversation_room(Conversation *conversation, CrimsonRoom *room);
CrimsonRoom *get_conversation_room(Conversation *conversation);
void refresh_conversation(Conversation *conversation);
/* The open room's card, whose members button opens the members beside it, or the profile of the person a private
 * chat is with */
void show_conversation_details(Conversation *conversation);
void set_conversation_narrow(Conversation *conversation, gboolean is_narrow);
void focus_composer(Conversation *conversation);
