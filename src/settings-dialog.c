#include <adwaita.h>

#include "../lang/en.h"
#include "common/themes.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"

/* A switch bound to one field of the settings, which saves when flipped */
typedef struct {
	const char *title, *detail;
	gboolean *value;
} SwitchSetting;


static void change_switch(AdwSwitchRow *row, GParamSpec *spec, gboolean *value) {
	*value = adw_switch_row_get_active(row);
	save_settings();
	Window *window = get_main_window();
	if (window)
		refresh_window_later(window);
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


static GtkWidget *add_page(AdwPreferencesDialog *dialog, const char *title, const char *icon) {
	GtkWidget *page = adw_preferences_page_new();
	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), title);
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), icon);
	adw_preferences_dialog_add(dialog, ADW_PREFERENCES_PAGE(page));
	return page;
}


static GtkWidget *add_group(GtkWidget *page, const char *title) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
	return group;
}


static void change_style(AdwComboRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->style = (Style)adw_combo_row_get_selected(row);
	save_settings();
	apply_style();
}


static void change_theme(AdwComboRow *row, GParamSpec *spec, char **theme) {
	const char *const *ids = g_object_get_data(G_OBJECT(row), "theme-ids");
	g_free(*theme);
	*theme = g_strdup(ids[adw_combo_row_get_selected(row)]);
	save_settings();
	apply_style();
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


static void change_words(AdwEntryRow *row, gpointer data) {
	g_free(get_settings()->highlight_words);
	get_settings()->highlight_words = g_strdup(gtk_editable_get_text(GTK_EDITABLE(row)));
	save_settings();
}


static void fill_appearance(AdwPreferencesDialog *dialog) {
	Settings *settings = get_settings();
	GtkWidget *page = add_page(dialog, TEXT_APPEARANCE, "applications-graphics-symbolic");
	GtkWidget *look = add_group(page, TEXT_STYLE_GROUP);
	static const char *const STYLES[] = {TEXT_FOLLOW_SYSTEM, TEXT_LIGHT, TEXT_DARK, NULL};
	g_autoptr(GtkStringList) styles = gtk_string_list_new(STYLES);
	GtkWidget *style = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(style), TEXT_STYLE);
	adw_combo_row_set_model(ADW_COMBO_ROW(style), G_LIST_MODEL(styles));
	adw_combo_row_set_selected(ADW_COMBO_ROW(style), settings->style);
	g_signal_connect(style, "notify::selected", G_CALLBACK(change_style), NULL);
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


static void fill_notifications(AdwPreferencesDialog *dialog) {
	Settings *settings = get_settings();
	GtkWidget *page = add_page(dialog, TEXT_NOTIFICATIONS, "preferences-system-notifications-symbolic");
	GtkWidget *group = add_group(page, TEXT_NOTIFY_GROUP);
	const SwitchSetting NOTICES[] = {
		{TEXT_NOTIFY_MENTIONS, NULL, &settings->notifies_mentions},
		{TEXT_NOTIFY_PRIVATE, NULL, &settings->notifies_private},
		{TEXT_PLAY_SOUNDS, NULL, &settings->plays_sounds},
	};
	add_switches(group, NOTICES, G_N_ELEMENTS(NOTICES));
	GtkWidget *words = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(words), TEXT_HIGHLIGHT_WORDS);
	adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(words), TRUE);
	gtk_editable_set_text(GTK_EDITABLE(words), settings->highlight_words ? settings->highlight_words : "");
	g_signal_connect(words, "apply", G_CALLBACK(change_words), NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), words);
}


static void fill_privacy(AdwPreferencesDialog *dialog) {
	Settings *settings = get_settings();
	GtkWidget *page = add_page(dialog, TEXT_PRIVACY, "security-high-symbolic");
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


void show_settings(GtkWidget *parent) {
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	fill_appearance(ADW_PREFERENCES_DIALOG(dialog));
	fill_notifications(ADW_PREFERENCES_DIALOG(dialog));
	fill_privacy(ADW_PREFERENCES_DIALOG(dialog));
	adw_dialog_present(dialog, parent);
}
