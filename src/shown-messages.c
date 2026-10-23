#include "shown-messages.h"

struct _CrimsonShownMessages {
	GObject parent;
	GListModel *messages;
	gulong handler;
	/* Where the earliest shown message is in the room, and how many of the latest show while none do */
	guint offset, latest_count;
};

static void crimson_shown_messages_list_init(GListModelInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(CrimsonShownMessages, crimson_shown_messages, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, crimson_shown_messages_list_init))


static GType get_shown_type(GListModel *model) {
	return g_list_model_get_item_type(CRIMSON_SHOWN_MESSAGES(model)->messages);
}


static guint count_shown(GListModel *model) {
	CrimsonShownMessages *shown = CRIMSON_SHOWN_MESSAGES(model);
	return g_list_model_get_n_items(shown->messages) - shown->offset;
}


static gpointer get_shown(GListModel *model, guint position) {
	CrimsonShownMessages *shown = CRIMSON_SHOWN_MESSAGES(model);
	return g_list_model_get_item(shown->messages, shown->offset + position);
}


static void crimson_shown_messages_list_init(GListModelInterface *interface) {
	interface->get_item_type = get_shown_type;
	interface->get_n_items = count_shown;
	interface->get_item = get_shown;
}


static void crimson_shown_messages_dispose(GObject *object) {
	CrimsonShownMessages *shown = CRIMSON_SHOWN_MESSAGES(object);
	if (shown->messages)
		g_clear_signal_handler(&shown->handler, shown->messages);
	g_clear_object(&shown->messages);
	G_OBJECT_CLASS(crimson_shown_messages_parent_class)->dispose(object);
}


static void crimson_shown_messages_class_init(CrimsonShownMessagesClass *class) {
	G_OBJECT_CLASS(class)->dispose = crimson_shown_messages_dispose;
}


static void crimson_shown_messages_init(CrimsonShownMessages *shown) {
}


static void show_latest(CrimsonShownMessages *shown, guint latest_count) {
	guint count = g_list_model_get_n_items(shown->messages);
	shown->offset = count > latest_count ? count - latest_count : 0;
}


static void follow_messages(GListModel *messages, guint position, guint removed, guint added,
	CrimsonShownMessages *shown) {
	guint count = g_list_model_get_n_items(messages);
	guint shown_before = count - added + removed - shown->offset;
	/* A room with nothing shown yet, such as one whose history was just received, shows its latest messages */
	if (!shown_before) {
		show_latest(shown, shown->latest_count);
		g_list_model_items_changed(G_LIST_MODEL(shown), 0, 0, count - shown->offset);
		return;
	}
	/* Older messages, such as a page of history, go before the earliest shown one and wait there */
	if (position + removed < shown->offset || (position == shown->offset && !removed)) {
		shown->offset = shown->offset + added - removed;
		return;
	}
	if (position >= shown->offset) {
		g_list_model_items_changed(G_LIST_MODEL(shown), position - shown->offset, removed, added);
		return;
	}
	/* A removal across the earliest shown message shows from where it starts */
	shown->offset = position;
	g_list_model_items_changed(G_LIST_MODEL(shown), 0, shown_before, count - position);
}


CrimsonShownMessages *create_shown_messages(GListModel *messages, guint shown_count, guint latest_count) {
	CrimsonShownMessages *shown = g_object_new(CRIMSON_TYPE_SHOWN_MESSAGES, NULL);
	shown->messages = g_object_ref(messages);
	shown->latest_count = latest_count;
	show_latest(shown, shown_count);
	shown->handler = g_signal_connect(messages, "items-changed", G_CALLBACK(follow_messages), shown);
	return shown;
}


guint count_hidden_messages(CrimsonShownMessages *shown) {
	return shown->offset;
}


void reveal_hidden_messages(CrimsonShownMessages *shown, guint count) {
	count = MIN(count, shown->offset);
	if (!count)
		return;
	shown->offset -= count;
	g_list_model_items_changed(G_LIST_MODEL(shown), 0, 0, count);
}


void reveal_messages_from(CrimsonShownMessages *shown, guint position) {
	if (position < shown->offset)
		reveal_hidden_messages(shown, shown->offset - position);
}
