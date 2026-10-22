#include "../lang/en.h"
#include "settings.h"
#include "common/startup.h"
#include "composer.h"
#include "profile.h"
#include "formatting.h"
#include "images.h"
#include "message-row.h"
#include "room-look.h"
#include "window.h"

#define HEADER_AVATAR_SIZE 38
#define MEMBER_AVATAR_SIZE 32
#define BOTTOM_SLACK 48.0
#define MEMBERS_WIDTH 260
#define MESSAGE_KEY "message"
#define HANDLER_KEY "changed-handler"

struct Conversation {
	Window *window;
	GtkWidget *stack, *split, *avatar, *title, *subtitle, *members_button, *list_view, *scroller, *jump_button;
	GtkWidget *member_view, *empty_note;
	GtkStringList *members;
	GtkNoSelection *messages;
	Composer *composer;
	CrimsonRoom *room;
	gulong room_handler;
	/* The view follows the newest message until the user scrolls away from it */
	gboolean is_pinned, has_user_scrolled;
	/* Whether the user opened the member list, which the split view would otherwise change when it resizes */
	gboolean wants_members;
	guint member_source;
	MessageActions actions;
};


static GtkAdjustment *get_scroll(Conversation *conversation) {
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(conversation->scroller));
}


/* The list only knows its true length once its last rows are measured, so it is asked to show the last row */
static void scroll_to_bottom(Conversation *conversation) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages));
	if (count)
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), count - 1, GTK_LIST_SCROLL_NONE, NULL);
	GtkAdjustment *scroll = get_scroll(conversation);
	gtk_adjustment_set_value(scroll, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll));
}


static void mark_read_when_seen(Conversation *conversation) {
	if (conversation->room && conversation->is_pinned && is_room_in_view(conversation->window, conversation->room))
		mark_room_read(conversation->room);
}


static void pin_to_bottom(Conversation *conversation) {
	conversation->is_pinned = TRUE;
	conversation->has_user_scrolled = FALSE;
	gtk_widget_set_visible(conversation->jump_button, FALSE);
	scroll_to_bottom(conversation);
}


/* Rows measure themselves as they appear and move the view, so only the user's own scrolling unpins it */
static void follow_scroll(GtkAdjustment *scroll, Conversation *conversation) {
	if (conversation->has_user_scrolled) {
		double bottom = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
		conversation->is_pinned = gtk_adjustment_get_value(scroll) >= bottom - BOTTOM_SLACK;
		gtk_widget_set_visible(conversation->jump_button, !conversation->is_pinned);
	}
	mark_read_when_seen(conversation);
}


/* New lines keep the view at the bottom when it was there, and leave it alone while reading back */
static void follow_growth(GtkAdjustment *scroll, GParamSpec *spec, Conversation *conversation) {
	if (conversation->is_pinned)
		scroll_to_bottom(conversation);
}


static gboolean note_user_scroll(GtkEventControllerScroll *controller, double x, double y, gpointer data) {
	Conversation *conversation = data;
	conversation->has_user_scrolled = TRUE;
	return FALSE;
}


static void reach_edge(GtkScrolledWindow *scroller, GtkPositionType position, Conversation *conversation) {
	if (position == GTK_POS_TOP && conversation->room)
		load_older_history(conversation->room);
	else if (position == GTK_POS_BOTTOM && conversation->has_user_scrolled)
		pin_to_bottom(conversation);
}


static void jump_down(GtkButton *button, Conversation *conversation) {
	pin_to_bottom(conversation);
}


static CrimsonMessage *get_previous_message(Conversation *conversation, guint position) {
	if (!position)
		return NULL;
	CrimsonMessage *previous = g_list_model_get_item(G_LIST_MODEL(conversation->messages), position - 1);
	g_object_unref(previous);
	return previous;
}


static void refill_row(CrimsonMessage *message, gpointer item) {
	GtkWidget *row = gtk_list_item_get_child(item);
	Conversation *conversation = g_object_get_data(G_OBJECT(row), "conversation");
	if (conversation->room)
		fill_message_row(row, message, get_previous_message(conversation, gtk_list_item_get_position(item)),
			conversation->room, &conversation->actions);
}


static void set_up_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	GtkWidget *row = create_message_row();
	g_object_set_data(G_OBJECT(row), "conversation", conversation);
	gtk_list_item_set_child(item, row);
	gtk_list_item_set_activatable(item, FALSE);
	gtk_list_item_set_focusable(item, FALSE);
}


static void bind_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	CrimsonMessage *message = gtk_list_item_get_item(item);
	GtkWidget *row = gtk_list_item_get_child(item);
	fill_message_row(row, message, get_previous_message(conversation, gtk_list_item_get_position(item)),
		conversation->room, &conversation->actions);
	gulong handler = g_signal_connect(message, "changed", G_CALLBACK(refill_row), item);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, GSIZE_TO_POINTER(handler));
	g_object_set_data(G_OBJECT(row), MESSAGE_KEY, message);
}


static void unbind_message(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	GtkWidget *row = gtk_list_item_get_child(item);
	CrimsonMessage *message = g_object_get_data(G_OBJECT(row), MESSAGE_KEY);
	gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), HANDLER_KEY));
	if (message && handler)
		g_signal_handler_disconnect(message, handler);
	g_object_set_data(G_OBJECT(row), MESSAGE_KEY, NULL);
	g_object_set_data(G_OBJECT(row), HANDLER_KEY, NULL);
}


static void reply_to(CrimsonMessage *message, gpointer data) {
	Conversation *conversation = data;
	reply_in_composer(conversation->composer, message);
}


static void react_to(CrimsonMessage *message, const char *reaction, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		send_reaction(conversation->room, message, reaction);
}


static void remove_message(CrimsonMessage *message, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		redact_message(conversation->room, message);
}


static void show_message(const char *message_id, gpointer data) {
	Conversation *conversation = data;
	CrimsonMessage *target = conversation->room ? find_room_message(conversation->room, message_id) : NULL;
	guint position = 0;
	if (target && g_list_store_find(conversation->room->messages, target, &position)) {
		conversation->is_pinned = FALSE;
		conversation->has_user_scrolled = TRUE;
		gtk_widget_set_visible(conversation->jump_button, TRUE);
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), position, GTK_LIST_SCROLL_NONE, NULL);
	}
}


static void open_person(const char *nick, gpointer data) {
	Conversation *conversation = data;
	if (!conversation->room || !nick)
		return;
	CrimsonRoom *room = open_private_room(conversation->room->network, nick);
	open_room(conversation->window, room);
}


/* An empty room says so, unless its history is still on the way */
static void show_empty_note(Conversation *conversation) {
	gboolean is_empty = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages)) == 0;
	gboolean is_waiting = conversation->room && conversation->room->is_loading_history;
	gtk_widget_set_visible(conversation->empty_note, is_empty && !is_waiting);
}


static GtkWidget *create_message_list(Conversation *conversation) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_message), conversation);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_message), conversation);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_message), conversation);
	conversation->messages = gtk_no_selection_new(NULL);
	conversation->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(conversation->messages)), factory);
	gtk_widget_add_css_class(conversation->list_view, "messages");
	conversation->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(conversation->scroller), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(conversation->scroller), conversation->list_view);
	gtk_widget_set_vexpand(conversation->scroller, TRUE);
	g_signal_connect(conversation->scroller, "edge-reached", G_CALLBACK(reach_edge), conversation);
	GtkAdjustment *scroll = get_scroll(conversation);
	g_signal_connect(scroll, "value-changed", G_CALLBACK(follow_scroll), conversation);
	g_signal_connect(scroll, "notify::upper", G_CALLBACK(follow_growth), conversation);
	g_signal_connect(scroll, "notify::page-size", G_CALLBACK(follow_growth), conversation);
	GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(note_user_scroll), conversation);
	gtk_widget_add_controller(conversation->scroller, wheel);
	conversation->jump_button = gtk_button_new_from_icon_name("go-down-symbolic");
	gtk_widget_set_tooltip_text(conversation->jump_button, TEXT_JUMP_TO_LATEST);
	gtk_widget_add_css_class(conversation->jump_button, "circular");
	gtk_widget_add_css_class(conversation->jump_button, "osd");
	gtk_widget_add_css_class(conversation->jump_button, "jump-button");
	gtk_widget_set_halign(conversation->jump_button, GTK_ALIGN_END);
	gtk_widget_set_valign(conversation->jump_button, GTK_ALIGN_END);
	gtk_widget_set_visible(conversation->jump_button, FALSE);
	g_signal_connect(conversation->jump_button, "clicked", G_CALLBACK(jump_down), conversation);
	conversation->empty_note = gtk_label_new(TEXT_NO_MESSAGES);
	gtk_widget_add_css_class(conversation->empty_note, "empty-pill");
	gtk_widget_set_halign(conversation->empty_note, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(conversation->empty_note, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(conversation->empty_note, FALSE);
	g_signal_connect_swapped(conversation->messages, "items-changed", G_CALLBACK(show_empty_note), conversation);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), conversation->scroller);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->empty_note);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), conversation->jump_button);
	gtk_widget_add_css_class(overlay, "chat-background");
	return overlay;
}


static const char *describe_role(char symbol) {
	switch (symbol) {
	case '~': return TEXT_ROLE_OWNER;
	case '&': return TEXT_ROLE_ADMIN;
	case '@': return TEXT_ROLE_OPERATOR;
	case '%': return TEXT_ROLE_HALF_OPERATOR;
	case '+': return TEXT_ROLE_VOICE;
	default: return "";
	}
}


/* Whether the user can moderate the open channel, as its operator or half-operator */
static gboolean can_moderate(Conversation *conversation, IrcState *state, IrcChannel *channel) {
	g_autofree char *own = fold_state_name(state, state->nick);
	const char *symbols = g_hash_table_lookup(channel->members, own);
	return symbols && strpbrk(symbols, "~&@%");
}


static void append_member_item(GMenu *menu, const char *label, const char *action, const char *nick) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(nick));
	g_menu_append_item(menu, item);
}


static GMenuModel *create_member_menu(Conversation *conversation, const char *nick) {
	GMenu *menu = g_menu_new();
	GMenu *person = g_menu_new();
	append_member_item(person, TEXT_SEND_MESSAGE, "member.message", nick);
	append_member_item(person, TEXT_MENTION, "member.mention", nick);
	append_member_item(person, TEXT_VIEW_PROFILE, "member.profile", nick);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(person));
	g_object_unref(person);
	IrcState *state = get_network_state(conversation->room->network);
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	if (!channel || !can_moderate(conversation, state, channel) || is_own_nick(state, nick))
		return G_MENU_MODEL(menu);
	g_autofree char *folded = fold_state_name(state, nick);
	const char *symbols = g_hash_table_lookup(channel->members, folded);
	gboolean is_operator = symbols && strchr(symbols, '@');
	gboolean has_voice = symbols && strchr(symbols, '+');
	GMenu *moderation = g_menu_new();
	append_member_item(moderation, is_operator ? TEXT_REMOVE_OPERATOR : TEXT_MAKE_OPERATOR,
		is_operator ? "member.deop" : "member.op", nick);
	append_member_item(moderation, has_voice ? TEXT_REMOVE_VOICE : TEXT_GIVE_VOICE,
		has_voice ? "member.devoice" : "member.voice", nick);
	append_member_item(moderation, TEXT_KICK, "member.kick", nick);
	append_member_item(moderation, TEXT_BAN, "member.ban", nick);
	g_menu_append_section(menu, NULL, G_MENU_MODEL(moderation));
	g_object_unref(moderation);
	return G_MENU_MODEL(menu);
}


static void forget_member_menu(GtkPopover *popover, gpointer data) {
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


/* Right-clicking a person offers what can be done with them, and moderation where the user may moderate */
static void open_member_menu(GtkGestureClick *gesture, int presses, double x, double y, Conversation *conversation) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	const char *nick = gtk_label_get_text(g_object_get_data(G_OBJECT(row), "name"));
	if (!conversation->room || !nick || !*nick)
		return;
	g_autoptr(GMenuModel) model = create_member_menu(conversation, nick);
	GtkWidget *popover = gtk_popover_menu_new_from_model(model);
	gtk_widget_set_parent(popover, row);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	g_signal_connect(popover, "closed", G_CALLBACK(forget_member_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}


static void set_up_member(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(row, "member-row");
	GtkWidget *avatar = adw_avatar_new(MEMBER_AVATAR_SIZE, NULL, TRUE);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(texts, TRUE);
	GtkWidget *name = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	GtkWidget *role = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(role), 0);
	gtk_widget_add_css_class(role, "member-role");
	gtk_box_append(GTK_BOX(texts), name);
	gtk_box_append(GTK_BOX(texts), role);
	gtk_box_append(GTK_BOX(row), avatar);
	gtk_box_append(GTK_BOX(row), texts);
	g_object_set_data(G_OBJECT(row), "avatar", avatar);
	g_object_set_data(G_OBJECT(row), "name", name);
	g_object_set_data(G_OBJECT(row), "role", role);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_member_menu), data);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	gtk_list_item_set_child(item, row);
}


static void bind_member(GtkSignalListItemFactory *factory, GtkListItem *item, Conversation *conversation) {
	const char *nick = gtk_string_object_get_string(gtk_list_item_get_item(item));
	GtkWidget *row = gtk_list_item_get_child(item);
	IrcState *state = conversation->room ? get_network_state(conversation->room->network) : NULL;
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	IrcUser *user = state ? find_irc_user(state, nick) : NULL;
	g_autofree char *folded = state ? fold_state_name(state, nick) : NULL;
	const char *symbols = channel && folded ? g_hash_table_lookup(channel->members, folded) : NULL;
	adw_avatar_set_text(g_object_get_data(G_OBJECT(row), "avatar"), nick);
	const char *url = conversation->room ? find_metadata(conversation->room, nick, "avatar") : NULL;
	g_autofree char *sized = fill_image_size(url, MEMBER_AVATAR_SIZE * 2);
	show_avatar_image(g_object_get_data(G_OBJECT(row), "avatar"), sized);
	gtk_label_set_text(g_object_get_data(G_OBJECT(row), "name"), nick);
	const char *role = describe_role(get_highest_prefix(symbols));
	gtk_label_set_text(g_object_get_data(G_OBJECT(row), "role"), role);
	gtk_widget_set_visible(g_object_get_data(G_OBJECT(row), "role"), *role != '\0');
	gtk_widget_set_opacity(row, user && user->is_away ? 0.5 : 1.0);
}


static void message_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	open_person(g_variant_get_string(parameter, NULL), data);
}


static void mention_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	mention_in_composer(conversation->composer, g_variant_get_string(parameter, NULL));
}


static void show_member_profile(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	if (conversation->room)
		show_profile(conversation->window, conversation->room, g_variant_get_string(parameter, NULL));
}


static void send_channel_mode(Conversation *conversation, const char *mode, const char *nick) {
	if (conversation->room && conversation->room->network->client)
		send_irc_client_command(conversation->room->network->client, "MODE", conversation->room->name, mode, nick,
			NULL);
}


static void moderate_member(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Conversation *conversation = data;
	const char *name = g_action_get_name(G_ACTION(action));
	const char *nick = g_variant_get_string(parameter, NULL);
	CrimsonRoom *room = conversation->room;
	if (!room || !room->network->client)
		return;
	static const struct {
		const char *action, *mode;
	} MODES[] = {{"op", "+o"}, {"deop", "-o"}, {"voice", "+v"}, {"devoice", "-v"}};
	for (guint index = 0; index < G_N_ELEMENTS(MODES); index++) {
		if (g_str_equal(name, MODES[index].action))
			send_channel_mode(conversation, MODES[index].mode, nick);
	}
	/* A ban covers the person's host where it is known, so a new nick does not get around it */
	if (g_str_equal(name, "ban")) {
		IrcState *state = get_network_state(room->network);
		IrcUser *user = state ? find_irc_user(state, nick) : NULL;
		g_autofree char *mask = user && user->host ? g_strdup_printf("*!*@%s", user->host)
			: g_strdup_printf("%s!*@*", nick);
		send_channel_mode(conversation, "+b", mask);
	}
	if (g_str_equal(name, "kick") || g_str_equal(name, "ban"))
		send_irc_client_command(room->network->client, "KICK", room->name, nick, NULL);
}


static void add_member_actions(Conversation *conversation) {
	static const GActionEntry ENTRIES[] = {
		{"message", message_member, "s", NULL, NULL, {0}},
		{"mention", mention_member, "s", NULL, NULL, {0}},
		{"profile", show_member_profile, "s", NULL, NULL, {0}},
		{"op", moderate_member, "s", NULL, NULL, {0}},
		{"deop", moderate_member, "s", NULL, NULL, {0}},
		{"voice", moderate_member, "s", NULL, NULL, {0}},
		{"devoice", moderate_member, "s", NULL, NULL, {0}},
		{"kick", moderate_member, "s", NULL, NULL, {0}},
		{"ban", moderate_member, "s", NULL, NULL, {0}},
	};
	GSimpleActionGroup *group = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(group), ENTRIES, G_N_ELEMENTS(ENTRIES), conversation);
	gtk_widget_insert_action_group(conversation->member_view, "member", G_ACTION_GROUP(group));
	g_object_unref(group);
}


/* Double-clicking a person opens a private chat with them, while one click only picks them */
static void open_member(GtkListView *view, guint position, Conversation *conversation) {
	g_autoptr(GtkStringObject) item = g_list_model_get_item(G_LIST_MODEL(conversation->members), position);
	if (item)
		open_person(gtk_string_object_get_string(item), conversation);
}


typedef struct {
	int rank;
	const char *nick;
} RankedMember;


static int order_members(gconstpointer first, gconstpointer second) {
	const RankedMember *one = first, *other = second;
	if (one->rank != other->rank)
		return one->rank - other->rank;
	return g_ascii_strcasecmp(one->nick, other->nick);
}


/* Members ordered by status, highest first, then by name */
static gboolean fill_members(gpointer data) {
	Conversation *conversation = data;
	conversation->member_source = 0;
	IrcState *state = conversation->room ? get_network_state(conversation->room->network) : NULL;
	IrcChannel *channel = state ? find_irc_channel(state, conversation->room->name) : NULL;
	g_autoptr(GArray) ranked = g_array_new(FALSE, FALSE, sizeof(RankedMember));
	if (channel) {
		GHashTableIter iterator;
		gpointer folded, symbols;
		g_hash_table_iter_init(&iterator, channel->members);
		while (g_hash_table_iter_next(&iterator, &folded, &symbols)) {
			IrcUser *user = g_hash_table_lookup(state->users, folded);
			int rank = rank_irc_prefix(state, symbols);
			RankedMember member = {rank < 0 ? G_MAXINT : rank, user ? user->nick : folded};
			g_array_append_val(ranked, member);
		}
	}
	g_array_sort(ranked, order_members);
	g_autoptr(GPtrArray) nicks = g_ptr_array_new();
	for (guint index = 0; index < ranked->len; index++)
		g_ptr_array_add(nicks, (gpointer)g_array_index(ranked, RankedMember, index).nick);
	g_ptr_array_add(nicks, NULL);
	gtk_string_list_splice(conversation->members, 0, g_list_model_get_n_items(G_LIST_MODEL(conversation->members)),
		(const char *const *)nicks->pdata);
	return G_SOURCE_REMOVE;
}


static void refresh_members_later(Conversation *conversation) {
	gboolean is_shown = adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split));
	if (is_shown && !conversation->member_source)
		conversation->member_source = g_idle_add(fill_members, conversation);
}


static GtkWidget *create_member_panel(Conversation *conversation) {
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(panel, "member-panel");
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "top-bar");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new(TEXT_MEMBERS_TITLE, NULL));
	/* The panel sits at the window's right edge, so its header carries the window buttons there */
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	gtk_box_append(GTK_BOX(panel), header);
	conversation->members = gtk_string_list_new(NULL);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_member), conversation);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_member), conversation);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(conversation->members)));
	gtk_single_selection_set_autoselect(selection, FALSE);
	gtk_single_selection_set_can_unselect(selection, TRUE);
	gtk_single_selection_set_selected(selection, GTK_INVALID_LIST_POSITION);
	conversation->member_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
	add_member_actions(conversation);
	gtk_widget_add_css_class(conversation->member_view, "member-list");
	g_signal_connect(conversation->member_view, "activate", G_CALLBACK(open_member), conversation);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), conversation->member_view);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(panel), scroller);
	return panel;
}


static void toggle_members(GtkToggleButton *button, Conversation *conversation) {
	conversation->wants_members = gtk_toggle_button_get_active(button);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		conversation->wants_members);
	refresh_members_later(conversation);
}


/* Closing the overlaid list by clicking beside it counts as the user closing it */
static void follow_member_panel(AdwOverlaySplitView *split, GParamSpec *spec, Conversation *conversation) {
	gboolean is_shown = adw_overlay_split_view_get_show_sidebar(split);
	if (adw_overlay_split_view_get_collapsed(split))
		conversation->wants_members = is_shown;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(conversation->members_button), is_shown);
}


static GtkWidget *create_header(Conversation *conversation) {
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "top-bar");
	GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	conversation->avatar = adw_avatar_new(HEADER_AVATAR_SIZE, NULL, TRUE);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	conversation->title = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(conversation->title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(conversation->title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(conversation->title, "conversation-title");
	conversation->subtitle = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(conversation->subtitle), 0);
	gtk_label_set_ellipsize(GTK_LABEL(conversation->subtitle), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(conversation->subtitle, "conversation-subtitle");
	gtk_box_append(GTK_BOX(texts), conversation->title);
	gtk_box_append(GTK_BOX(texts), conversation->subtitle);
	gtk_box_append(GTK_BOX(title), conversation->avatar);
	gtk_box_append(GTK_BOX(title), texts);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), title);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new(NULL));
	conversation->members_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(conversation->members_button), "system-users-symbolic");
	gtk_widget_set_tooltip_text(conversation->members_button, TEXT_MEMBERS_TOOLTIP);
	g_signal_connect(conversation->members_button, "toggled", G_CALLBACK(toggle_members), conversation);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), conversation->members_button);
	return header;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(page, "chat-background");
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "top-bar");
	gtk_widget_add_css_class(header, "flat");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new(NULL));
	gtk_box_append(GTK_BOX(page), header);
	return page;
}


static GtkWidget *create_room_page(Conversation *conversation) {
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(conversation));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), create_message_list(conversation));
	conversation->composer = create_composer();
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), get_composer_widget(conversation->composer));
	adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_RAISED_BORDER);
	conversation->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(conversation->split), GTK_PACK_END);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split), FALSE);
	adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(conversation->split), MEMBERS_WIDTH);
	adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(conversation->split), MEMBERS_WIDTH);
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(conversation->split), toolbar);
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		create_member_panel(conversation));
	g_signal_connect(conversation->split, "notify::show-sidebar", G_CALLBACK(follow_member_panel), conversation);
	return conversation->split;
}


Conversation *create_conversation(Window *window) {
	Conversation *conversation = g_new0(Conversation, 1);
	conversation->window = window;
	conversation->is_pinned = TRUE;
	conversation->actions = (MessageActions){reply_to, react_to, remove_message, show_message, open_person,
		conversation};
	conversation->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(conversation->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(conversation->stack), create_room_page(conversation), "room");
	return conversation;
}


GtkWidget *get_conversation_widget(Conversation *conversation) {
	return conversation->stack;
}


CrimsonRoom *get_conversation_room(Conversation *conversation) {
	return conversation->room;
}


static void fill_header(Conversation *conversation) {
	CrimsonRoom *room = conversation->room;
	g_autofree char *title = describe_room_title(room);
	g_autofree char *topic = room->topic ? strip_irc_formatting(room->topic) : NULL;
	/* A room's topic says what it is about, so it takes the place of the member count unless someone types */
	gboolean shows_topic = room->kind == ROOM_CHANNEL && topic && *topic
		&& !(is_room_typing(room) && get_settings()->shows_typing);
	g_autofree char *subtitle = shows_topic ? g_strdup(topic) : describe_room_subtitle(room);
	gtk_label_set_text(GTK_LABEL(conversation->title), title);
	gtk_label_set_text(GTK_LABEL(conversation->subtitle), subtitle);
	gtk_widget_set_tooltip_text(conversation->subtitle, shows_topic ? topic : NULL);
	if (is_room_typing(room))
		gtk_widget_add_css_class(conversation->subtitle, "typing");
	else
		gtk_widget_remove_css_class(conversation->subtitle, "typing");
	fill_room_avatar(ADW_AVATAR(conversation->avatar), room, HEADER_AVATAR_SIZE);
	gtk_widget_set_visible(conversation->members_button, room->kind == ROOM_CHANNEL);
}


void refresh_conversation(Conversation *conversation) {
	if (!conversation->room)
		return;
	fill_header(conversation);
	show_empty_note(conversation);
	refresh_members_later(conversation);
	mark_read_when_seen(conversation);
}


static void follow_room(CrimsonRoom *room, Conversation *conversation) {
	refresh_conversation(conversation);
}


/* A newly opened room starts at its latest message, once the list has measured its rows */
static gboolean settle_at_bottom(gpointer data) {
	Conversation *conversation = data;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(conversation->messages));
	if (count)
		gtk_list_view_scroll_to(GTK_LIST_VIEW(conversation->list_view), count - 1, GTK_LIST_SCROLL_NONE, NULL);
	pin_to_bottom(conversation);
	mark_read_when_seen(conversation);
	return G_SOURCE_REMOVE;
}


void show_conversation_room(Conversation *conversation, CrimsonRoom *room) {
	if (conversation->room == room)
		return;
	if (conversation->room)
		g_clear_signal_handler(&conversation->room_handler, conversation->room);
	switch_composer_room(conversation->composer, room);
	g_set_object(&conversation->room, room);
	gtk_no_selection_set_model(conversation->messages, room ? G_LIST_MODEL(room->messages) : NULL);
	gtk_stack_set_visible_child_name(GTK_STACK(conversation->stack), room ? "room" : "empty");
	if (!room)
		return;
	conversation->room_handler = g_signal_connect(room, "changed", G_CALLBACK(follow_room), conversation);
	fill_header(conversation);
	show_empty_note(conversation);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(conversation->split),
		room->kind == ROOM_CHANNEL && conversation->wants_members);
	refresh_members_later(conversation);
	if (!room->has_history)
		request_latest_history(room);
	pin_to_bottom(conversation);
	run_after_frame(conversation->list_view, settle_at_bottom, conversation);
}


void set_conversation_narrow(Conversation *conversation, gboolean is_narrow) {
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(conversation->split);
	adw_overlay_split_view_set_collapsed(split, is_narrow);
	adw_overlay_split_view_set_show_sidebar(split, conversation->wants_members && !is_narrow);
}


void focus_composer(Conversation *conversation) {
	focus_composer_field(conversation->composer);
}
