#include <adwaita.h>

#include "../lang/en.h"
#include "common/themes.h"
#include "dialogs.h"
#include "formatting.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"

#define SETTINGS_WIDTH 480
#define SETTINGS_HEIGHT 640
/* An override ending within this many days names its weekday rather than its date */
#define OVERRIDE_WEEKDAY_DAYS 6

/* A switch bound to one field of the copy the dialog changes, kept only when the user saves */
typedef struct {
	const char *title, *detail;
	gboolean *value;
} SwitchSetting;


static void change_switch(AdwSwitchRow *row, GParamSpec *spec, gboolean *value) {
	*value = adw_switch_row_get_active(row);
}


/* Rows never wrap, even in the narrowest window, and cut off what does not fit instead */
static void keep_row_on_one_line(GtkWidget *row) {
	adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
	adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
}


static void add_switches(GtkWidget *group, const SwitchSetting *switches, guint count) {
	for (guint index = 0; index < count; index++) {
		GtkWidget *row = adw_switch_row_new();
		keep_row_on_one_line(row);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), switches[index].title);
		if (switches[index].detail)
			adw_action_row_set_subtitle(ADW_ACTION_ROW(row), switches[index].detail);
		adw_switch_row_set_active(ADW_SWITCH_ROW(row), *switches[index].value);
		g_signal_connect(row, "notify::active", G_CALLBACK(change_switch), switches[index].value);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	}
}


static GtkWidget *add_page(GtkWidget *stack, const char *title, const char *icon) {
	GtkWidget *page = adw_preferences_page_new();
	adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), page, NULL, title, icon);
	return page;
}


static GtkWidget *add_group(GtkWidget *page, const char *title) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
	return group;
}


static void change_style(AdwComboRow *row, GParamSpec *spec, Style *style) {
	*style = (Style)adw_combo_row_get_selected(row);
}


static void change_tray_badge(AdwComboRow *row, GParamSpec *spec, TrayBadge *tray_badge) {
	*tray_badge = (TrayBadge)adw_combo_row_get_selected(row);
}


static void change_theme(AdwComboRow *row, GParamSpec *spec, char **theme) {
	const char *const *ids = g_object_get_data(G_OBJECT(row), "theme-ids");
	g_free(*theme);
	*theme = g_strdup(ids[adw_combo_row_get_selected(row)]);
}


/* Lists every theme by name with the one in use for that style picked
 * The row keeps its own ids, since the theme list is read again when a theme file changes */
static void add_theme_row(GtkWidget *group, const char *title, char **theme, const char *fallback) {
	GPtrArray *themes = list_themes();
	g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
	char **ids = g_new0(char *, themes->len + 1);
	guint selected = 0;
	for (guint index = 0; index < themes->len; index++) {
		const Theme *listed = g_ptr_array_index(themes, index);
		gtk_string_list_append(names, listed->name);
		ids[index] = g_strdup(listed->id);
		if (g_strcmp0(listed->id, *theme ? *theme : fallback) == 0)
			selected = index;
	}
	GtkWidget *row = adw_combo_row_new();
	keep_row_on_one_line(row);
	g_object_set_data_full(G_OBJECT(row), "theme-ids", ids, (GDestroyNotify)g_strfreev);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(names));
	adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
	g_signal_connect(row, "notify::selected", G_CALLBACK(change_theme), theme);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
}


static void change_words(GtkEditable *row, char **words) {
	g_free(*words);
	*words = g_strdup(gtk_editable_get_text(row));
}


static void fill_general(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_GENERAL, "preferences-system-symbolic");
	GtkWidget *chats = add_group(page, TEXT_CONVERSATIONS);
	const SwitchSetting CHATS[] = {
		{TEXT_SHOW_EVENTS, NULL, &settings->shows_events},
		{TEXT_SHOW_IMAGES, NULL, &settings->shows_images},
		{TEXT_SHOW_TYPING, NULL, &settings->shows_typing},
		{TEXT_24_HOURS, NULL, &settings->uses_24_hours},
	};
	add_switches(chats, CHATS, G_N_ELEMENTS(CHATS));
	GtkWidget *window = add_group(page, TEXT_WINDOW_GROUP);
	const SwitchSetting WINDOW[] = {
		{TEXT_CLOSE_TO_TRAY, TEXT_CLOSE_TO_TRAY_DETAIL, &settings->closes_to_tray},
	};
	add_switches(window, WINDOW, G_N_ELEMENTS(WINDOW));
}


static void fill_appearance(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_APPEARANCE, "applications-graphics-symbolic");
	GtkWidget *look = add_group(page, TEXT_STYLE_GROUP);
	static const char *const STYLES[] = {TEXT_FOLLOW_SYSTEM, TEXT_LIGHT, TEXT_DARK, NULL};
	g_autoptr(GtkStringList) styles = gtk_string_list_new(STYLES);
	GtkWidget *style = adw_combo_row_new();
	keep_row_on_one_line(style);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(style), TEXT_STYLE);
	adw_combo_row_set_model(ADW_COMBO_ROW(style), G_LIST_MODEL(styles));
	adw_combo_row_set_selected(ADW_COMBO_ROW(style), settings->style);
	g_signal_connect(style, "notify::selected", G_CALLBACK(change_style), &settings->style);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(look), style);
	add_theme_row(look, TEXT_LIGHT_THEME, &settings->light_theme, DEFAULT_LIGHT_THEME);
	add_theme_row(look, TEXT_DARK_THEME, &settings->dark_theme, DEFAULT_DARK_THEME);
}


static const char *find_network_name(const char *key) {
	Window *window = get_main_window();
	guint count = window ? g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks)) : 0;
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), index);
		if (g_str_equal(network->key, key))
			return network->name;
	}
	return key;
}


/* Names the server and whether the override is on or off, with the moment it ends, short enough for one line: the
 * time alone today, the weekday within a week and the date after that */
static char *describe_override(const RoomOverride *override) {
	const char *state = override->is_enabled ? TEXT_OVERRIDE_ON : TEXT_OVERRIDE_OFF;
	g_autofree char *when = NULL;
	if (override->until) {
		g_autofree char *clock = format_clock_time(override->until);
		g_autoptr(GDateTime) end = g_date_time_new_from_unix_local(override->until / G_USEC_PER_SEC);
		gboolean is_this_week = override->until - g_get_real_time() < OVERRIDE_WEEKDAY_DAYS * G_TIME_SPAN_DAY;
		g_autofree char *day = g_date_time_format(end, is_this_week ? "%a" : "%e %b");
		when = is_same_day(override->until, g_get_real_time()) ? g_steal_pointer(&clock)
			: g_strdup_printf("%s %s", g_strstrip(day), clock);
	}
	g_autofree char *status = when ? g_strdup_printf(TEXT_OVERRIDE_UNTIL, state, when) : g_strdup(state);
	return g_strdup_printf(TEXT_OVERRIDE_ROOM, find_network_name(override->network_key), status);
}


static void show_overrides_hint(GtkWidget *group, GPtrArray *overrides) {
	gboolean is_empty = overrides->len == 0;
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), is_empty ? TEXT_OVERRIDES_EMPTY : NULL);
}


static void remove_override_row(GtkButton *button, GtkWidget *row) {
	GtkWidget *group = gtk_widget_get_ancestor(row, ADW_TYPE_PREFERENCES_GROUP);
	GPtrArray *overrides = g_object_get_data(G_OBJECT(group), "overrides");
	g_ptr_array_remove(overrides, g_object_get_data(G_OBJECT(row), "override"));
	adw_preferences_group_remove(ADW_PREFERENCES_GROUP(group), row);
	show_overrides_hint(group, overrides);
}


/* Each room or chat set apart from the defaults, which a button returns to them once the form is saved */
static void add_overrides(GtkWidget *page, GPtrArray *overrides, const char *restore) {
	GtkWidget *group = add_group(page, TEXT_OVERRIDES_GROUP);
	g_object_set_data(G_OBJECT(group), "overrides", overrides);
	for (guint index = 0; index < overrides->len; index++) {
		RoomOverride *override = g_ptr_array_index(overrides, index);
		GtkWidget *row = adw_action_row_new();
		keep_row_on_one_line(row);
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), override->room_name);
		g_autofree char *description = describe_override(override);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), description);
		g_object_set_data(G_OBJECT(row), "override", override);
		GtkWidget *remove = gtk_button_new_from_icon_name("edit-delete-symbolic");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_tooltip_text(remove, restore);
		gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL, restore, -1);
		g_signal_connect(remove, "clicked", G_CALLBACK(remove_override_row), row);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	}
	show_overrides_hint(group, overrides);
}


static void fill_notifications(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_NOTIFICATIONS, "preferences-system-notifications-symbolic");
	GtkWidget *group = add_group(page, TEXT_NOTIFY_GROUP);
	const SwitchSetting NOTICES[] = {
		{TEXT_NOTIFY_MENTIONS, TEXT_NOTIFY_MENTIONS_DETAIL, &settings->notifies_mentions},
		{TEXT_NOTIFY_PRIVATE, NULL, &settings->notifies_private},
		{TEXT_NOTIFY_ROOM_MESSAGES, NULL, &settings->notifies_room_messages},
	};
	add_switches(group, NOTICES, G_N_ELEMENTS(NOTICES));
	GtkWidget *words = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(words), TEXT_HIGHLIGHT_WORDS);
	gtk_editable_set_text(GTK_EDITABLE(words), settings->highlight_words ? settings->highlight_words : "");
	g_signal_connect(words, "changed", G_CALLBACK(change_words), &settings->highlight_words);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), words);
	GtkWidget *sound = add_group(page, TEXT_SOUND_GROUP);
	const SwitchSetting SOUND[] = {
		{TEXT_PLAY_SOUNDS, NULL, &settings->plays_sounds},
	};
	add_switches(sound, SOUND, G_N_ELEMENTS(SOUND));
	add_overrides(page, settings->notification_overrides, TEXT_RESTORE_NOTIFICATIONS);
}


static void fill_badges(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_BADGES, "mail-unread-symbolic");
	GtkWidget *group = add_group(page, TEXT_BADGE_GROUP);
	const SwitchSetting BADGES[] = {
		{TEXT_NOTIFY_MENTIONS, TEXT_NOTIFY_MENTIONS_DETAIL, &settings->shows_mention_badges},
		{TEXT_NOTIFY_PRIVATE, NULL, &settings->shows_private_badges},
		{TEXT_NOTIFY_ROOM_MESSAGES, NULL, &settings->shows_room_badges},
	};
	add_switches(group, BADGES, G_N_ELEMENTS(BADGES));
	static const char *const TRAY_BADGES[] = {TEXT_TRAY_BADGE_NOTHING, TEXT_TRAY_BADGE_MENTIONS, TEXT_TRAY_BADGE_ANY,
		NULL};
	g_autoptr(GtkStringList) tray_badges = gtk_string_list_new(TRAY_BADGES);
	GtkWidget *tray = adw_combo_row_new();
	keep_row_on_one_line(tray);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tray), TEXT_TRAY_BADGE);
	adw_combo_row_set_model(ADW_COMBO_ROW(tray), G_LIST_MODEL(tray_badges));
	adw_combo_row_set_selected(ADW_COMBO_ROW(tray), settings->tray_badge);
	g_signal_connect(tray, "notify::selected", G_CALLBACK(change_tray_badge), &settings->tray_badge);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), tray);
	add_overrides(page, settings->badge_overrides, TEXT_RESTORE_BADGES);
}


static void fill_privacy(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_PRIVACY, "security-high-symbolic");
	GtkWidget *group = add_group(page, TEXT_PRIVACY_GROUP);
	const SwitchSetting PRIVACY[] = {
		{TEXT_SEND_TYPING, NULL, &settings->sends_typing},
		{TEXT_SEND_READ, TEXT_SEND_READ_DETAIL, &settings->sends_read_markers},
	};
	add_switches(group, PRIVACY, G_N_ELEMENTS(PRIVACY));
}


/* All rows in the room list draw again, since their badges and previews follow the settings */
static void announce_every_room(Window *window) {
	guint network_count = g_list_model_get_n_items(G_LIST_MODEL(window->chat->networks));
	for (guint network_index = 0; network_index < network_count; network_index++) {
		g_autoptr(CrimsonNetwork) network = g_list_model_get_item(G_LIST_MODEL(window->chat->networks), network_index);
		guint room_count = g_list_model_get_n_items(G_LIST_MODEL(network->rooms));
		for (guint room_index = 0; room_index < room_count; room_index++) {
			g_autoptr(CrimsonRoom) room = g_list_model_get_item(G_LIST_MODEL(network->rooms), room_index);
			announce_room_change(room);
		}
	}
}


static void save_settings_form(GtkButton *button, AdwDialog *dialog) {
	use_settings(g_object_get_data(G_OBJECT(dialog), "draft"));
	Window *window = get_main_window();
	if (window) {
		announce_every_room(window);
		refresh_window_later(window);
	}
	adw_dialog_close(dialog);
}


/* The pages change a copy of the settings, switched between from a bar along the bottom that fits a narrow window */
void show_settings(GtkWidget *parent) {
	Settings *draft = copy_settings(get_settings());
	AdwDialog *dialog = adw_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	adw_dialog_set_content_width(dialog, SETTINGS_WIDTH);
	adw_dialog_set_content_height(dialog, SETTINGS_HEIGHT);
	g_object_set_data_full(G_OBJECT(dialog), "draft", draft, (GDestroyNotify)free_settings);
	GtkWidget *stack = adw_view_stack_new();
	fill_general(stack, draft);
	fill_appearance(stack, draft);
	fill_notifications(stack, draft);
	fill_badges(stack, draft);
	fill_privacy(stack, draft);
	GtkWidget *save = NULL;
	GtkWidget *header = create_form_header(dialog, TEXT_SAVE, &save);
	g_signal_connect(save, "clicked", G_CALLBACK(save_settings_form), dialog);
	GtkWidget *switcher = adw_view_switcher_bar_new();
	adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(switcher), ADW_VIEW_STACK(stack));
	adw_view_switcher_bar_set_reveal(ADW_VIEW_SWITCHER_BAR(switcher), TRUE);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), switcher);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);
	adw_dialog_set_child(dialog, toolbar);
	adw_dialog_present(dialog, parent);
}
