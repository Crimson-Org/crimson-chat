#include <adwaita.h>

#include "../lang/en.h"
#include "common/themes.h"
#include "dialogs.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"

#define SETTINGS_WIDTH 480
#define SETTINGS_HEIGHT 640

/* A switch bound to one field of the copy the dialog changes, kept only when the user saves */
typedef struct {
	const char *title, *detail;
	gboolean *value;
} SwitchSetting;


static void change_switch(AdwSwitchRow *row, GParamSpec *spec, gboolean *value) {
	*value = adw_switch_row_get_active(row);
}


static void add_switches(GtkWidget *group, const SwitchSetting *switches, guint count) {
	for (guint index = 0; index < count; index++) {
		GtkWidget *row = adw_switch_row_new();
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


static void fill_appearance(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_APPEARANCE, "applications-graphics-symbolic");
	GtkWidget *look = add_group(page, TEXT_STYLE_GROUP);
	static const char *const STYLES[] = {TEXT_FOLLOW_SYSTEM, TEXT_LIGHT, TEXT_DARK, NULL};
	g_autoptr(GtkStringList) styles = gtk_string_list_new(STYLES);
	GtkWidget *style = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(style), TEXT_STYLE);
	adw_combo_row_set_model(ADW_COMBO_ROW(style), G_LIST_MODEL(styles));
	adw_combo_row_set_selected(ADW_COMBO_ROW(style), settings->style);
	g_signal_connect(style, "notify::selected", G_CALLBACK(change_style), &settings->style);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(look), style);
	add_theme_row(look, TEXT_LIGHT_THEME, &settings->light_theme, DEFAULT_LIGHT_THEME);
	add_theme_row(look, TEXT_DARK_THEME, &settings->dark_theme, DEFAULT_DARK_THEME);
	const SwitchSetting LOOK[] = {
		{TEXT_24_HOURS, NULL, &settings->uses_24_hours},
	};
	add_switches(look, LOOK, G_N_ELEMENTS(LOOK));
	GtkWidget *chat = add_group(page, TEXT_CONVERSATIONS);
	const SwitchSetting CHAT[] = {
		{TEXT_SHOW_EVENTS, NULL, &settings->shows_events},
		{TEXT_SHOW_IMAGES, NULL, &settings->shows_images},
		{TEXT_SHOW_TYPING, NULL, &settings->shows_typing},
	};
	add_switches(chat, CHAT, G_N_ELEMENTS(CHAT));
}


static void fill_notifications(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_NOTIFICATIONS, "preferences-system-notifications-symbolic");
	GtkWidget *group = add_group(page, TEXT_NOTIFY_GROUP);
	const SwitchSetting NOTICES[] = {
		{TEXT_NOTIFY_MENTIONS, NULL, &settings->notifies_mentions},
		{TEXT_NOTIFY_PRIVATE, NULL, &settings->notifies_private},
		{TEXT_PLAY_SOUNDS, NULL, &settings->plays_sounds},
	};
	add_switches(group, NOTICES, G_N_ELEMENTS(NOTICES));
	GtkWidget *words = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(words), TEXT_HIGHLIGHT_WORDS);
	gtk_editable_set_text(GTK_EDITABLE(words), settings->highlight_words ? settings->highlight_words : "");
	g_signal_connect(words, "changed", G_CALLBACK(change_words), &settings->highlight_words);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), words);
}


static void fill_privacy(GtkWidget *stack, Settings *settings) {
	GtkWidget *page = add_page(stack, TEXT_PRIVACY, "security-high-symbolic");
	GtkWidget *group = add_group(page, TEXT_PRIVACY_GROUP);
	const SwitchSetting PRIVACY[] = {
		{TEXT_SEND_TYPING, NULL, &settings->sends_typing},
		{TEXT_SEND_READ, NULL, &settings->sends_read_markers},
	};
	add_switches(group, PRIVACY, G_N_ELEMENTS(PRIVACY));
	GtkWidget *window = add_group(page, TEXT_WINDOW_GROUP);
	const SwitchSetting WINDOW[] = {
		{TEXT_CLOSE_TO_TRAY, NULL, &settings->closes_to_tray},
	};
	add_switches(window, WINDOW, G_N_ELEMENTS(WINDOW));
}


static void save_settings_form(GtkButton *button, AdwDialog *dialog) {
	use_settings(g_object_get_data(G_OBJECT(dialog), "draft"));
	Window *window = get_main_window();
	if (window)
		refresh_window_later(window);
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
	fill_appearance(stack, draft);
	fill_notifications(stack, draft);
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
