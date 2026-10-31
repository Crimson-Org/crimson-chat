#include "lang/en.h"
#include "register.h"

#define DIALOG_WIDTH 420
#define DIALOG_HEIGHT 560

/* The dialog's fields, the server it registers on and whether an answer is awaited */
typedef struct {
	Window *window;
	CrimsonNetwork *network;
	AdwDialog *dialog;
	GtkWidget *stack, *account, *email, *password, *repeat, *status, *register_button, *login_button;
	GtkWidget *code_text, *code, *verify_button;
	char *account_name;
	gboolean is_waiting;
} Registration;


static void free_registration(Registration *registration) {
	if (registration->network) {
		cancel_registration(registration->network);
		g_object_unref(registration->network);
	}
	g_free(registration->account_name);
	g_free(registration);
}


static void show_status(Registration *registration, const char *text) {
	gtk_label_set_text(GTK_LABEL(registration->status), text ? text : "");
	gtk_widget_set_visible(registration->status, text && *text);
}


static void set_waiting(Registration *registration, gboolean is_waiting) {
	registration->is_waiting = is_waiting;
	gtk_widget_set_sensitive(registration->register_button, !is_waiting);
	gtk_widget_set_sensitive(registration->verify_button, !is_waiting);
	gtk_button_set_label(GTK_BUTTON(registration->register_button), is_waiting ? TEXT_REGISTERING : TEXT_REGISTER);
}


static void hear_registration(RegistrationResult result, const char *message, gpointer data) {
	Registration *registration = data;
	set_waiting(registration, FALSE);
	if (result == REGISTRATION_DONE) {
		g_autofree char *text = g_strdup_printf(TEXT_REGISTERED_AS, registration->account_name);
		show_toast(registration->window, text);
		adw_dialog_close(registration->dialog);
		return;
	}
	/* The connection went away before an answer, so nothing more will come */
	if (!message) {
		show_status(registration, TEXT_REGISTRATION_LOST);
		return;
	}
	/* The nickname has an account already, so the way on is logging into it */
	gtk_widget_set_visible(registration->login_button, result == REGISTRATION_EXISTS);
	if (result == REGISTRATION_NEEDS_CODE) {
		gtk_label_set_text(GTK_LABEL(registration->code_text), message);
		gtk_stack_set_visible_child_name(GTK_STACK(registration->stack), "code");
		gtk_widget_grab_focus(registration->code);
		return;
	}
	show_status(registration, message);
}


static const char *read_text(GtkWidget *field) {
	return gtk_editable_get_text(GTK_EDITABLE(field));
}


static void send_registration(GtkWidget *widget, Registration *registration) {
	if (registration->is_waiting)
		return;
	g_autofree char *account = g_strstrip(g_strdup(read_text(registration->account)));
	g_autofree char *email = g_strstrip(g_strdup(read_text(registration->email)));
	const char *password = read_text(registration->password);
	if (!*account) {
		show_status(registration, TEXT_NEED_ACCOUNT_NAME);
		return;
	}
	if (!*email && is_email_required(registration->network)) {
		show_status(registration, TEXT_NEED_EMAIL);
		return;
	}
	if (!*password) {
		show_status(registration, TEXT_NEED_PASSWORD);
		return;
	}
	if (!g_str_equal(password, read_text(registration->repeat))) {
		show_status(registration, TEXT_PASSWORDS_DIFFER);
		return;
	}
	show_status(registration, NULL);
	g_free(registration->account_name);
	registration->account_name = g_strdup(account);
	set_waiting(registration, TRUE);
	request_registration(registration->network, account, email, password, hear_registration, registration);
}


static void send_code(GtkWidget *widget, Registration *registration) {
	g_autofree char *code = g_strstrip(g_strdup(read_text(registration->code)));
	if (!*code || registration->is_waiting)
		return;
	set_waiting(registration, TRUE);
	send_registration_code(registration->network, code);
}


static GtkWidget *add_entry(GtkWidget *list, const char *title, const char *value, gboolean is_password,
	Registration *registration) {
	GtkWidget *row = is_password ? adw_password_entry_row_new() : adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
	g_signal_connect(row, "entry-activated", G_CALLBACK(send_registration), registration);
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
}


static GtkWidget *create_pill_button(const char *label) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	return button;
}


static GtkWidget *create_form_page(Registration *registration) {
	CrimsonNetwork *network = registration->network;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "registration");
	GtkWidget *icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
	gtk_widget_add_css_class(icon, "accent");
	gtk_box_append(GTK_BOX(box), icon);
	g_autofree char *about = g_strdup_printf(TEXT_REGISTER_ABOUT, network->name);
	GtkWidget *intro = gtk_label_new(about);
	gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
	gtk_label_set_justify(GTK_LABEL(intro), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(intro, "dim-label");
	gtk_box_append(GTK_BOX(box), intro);
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	IrcState *state = get_network_state(network);
	registration->account = add_entry(list, TEXT_ACCOUNT_NAME, state ? state->nick : NULL, FALSE, registration);
	/* Most servers name the account after the nickname, so it only changes where the server allows */
	gtk_widget_set_sensitive(registration->account, allows_custom_account_name(network));
	registration->email = add_entry(list, is_email_required(network) ? TEXT_EMAIL_REQUIRED : TEXT_EMAIL_OPTIONAL,
		NULL, FALSE, registration);
	registration->password = add_entry(list, TEXT_PASSWORD, NULL, TRUE, registration);
	registration->repeat = add_entry(list, TEXT_REPEAT_PASSWORD, NULL, TRUE, registration);
	gtk_box_append(GTK_BOX(box), list);
	registration->status = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(registration->status), TRUE);
	gtk_label_set_justify(GTK_LABEL(registration->status), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(registration->status, "error");
	gtk_widget_set_visible(registration->status, FALSE);
	gtk_box_append(GTK_BOX(box), registration->status);
	registration->register_button = create_pill_button(TEXT_REGISTER);
	g_signal_connect(registration->register_button, "clicked", G_CALLBACK(send_registration), registration);
	gtk_box_append(GTK_BOX(box), registration->register_button);
	registration->login_button = create_pill_button(TEXT_LOG_IN);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(registration->login_button), "win.own-profile");
	gtk_actionable_set_action_target_value(GTK_ACTIONABLE(registration->login_button),
		g_variant_new_string(registration->network->key));
	g_signal_connect_swapped(registration->login_button, "clicked", G_CALLBACK(adw_dialog_close),
		registration->dialog);
	gtk_widget_set_visible(registration->login_button, FALSE);
	gtk_box_append(GTK_BOX(box), registration->login_button);
	return box;
}


/* The server's own words about the code it sent, such as that it went to the email address, and a field for it */
static GtkWidget *create_code_page(Registration *registration) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "registration");
	GtkWidget *icon = gtk_image_new_from_icon_name("mail-unread-symbolic");
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
	gtk_widget_add_css_class(icon, "accent");
	gtk_box_append(GTK_BOX(box), icon);
	registration->code_text = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(registration->code_text), TRUE);
	gtk_label_set_justify(GTK_LABEL(registration->code_text), GTK_JUSTIFY_CENTER);
	gtk_box_append(GTK_BOX(box), registration->code_text);
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	registration->code = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(registration->code), TEXT_CODE);
	g_signal_connect(registration->code, "entry-activated", G_CALLBACK(send_code), registration);
	gtk_list_box_append(GTK_LIST_BOX(list), registration->code);
	gtk_box_append(GTK_BOX(box), list);
	registration->verify_button = create_pill_button(TEXT_VERIFY);
	g_signal_connect(registration->verify_button, "clicked", G_CALLBACK(send_code), registration);
	gtk_box_append(GTK_BOX(box), registration->verify_button);
	return box;
}


void show_registration(Window *window, CrimsonNetwork *network) {
	Registration *registration = g_new0(Registration, 1);
	registration->window = window;
	registration->network = g_object_ref(network);
	registration->dialog = adw_dialog_new();
	/* The pages refer to the dialog, so it exists before they are built */
	g_autofree char *title = g_strdup_printf(TEXT_REGISTER_ON, network->name);
	adw_dialog_set_title(registration->dialog, title);
	adw_dialog_set_content_width(registration->dialog, DIALOG_WIDTH);
	adw_dialog_set_content_height(registration->dialog, DIALOG_HEIGHT);
	g_object_set_data_full(G_OBJECT(registration->dialog), "registration", registration,
		(GDestroyNotify)free_registration);
	registration->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(registration->stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
	gtk_stack_add_named(GTK_STACK(registration->stack), create_form_page(registration), "form");
	gtk_stack_add_named(GTK_STACK(registration->stack), create_code_page(registration), "code");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), registration->stack);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroller);
	adw_dialog_set_child(registration->dialog, toolbar);
	adw_dialog_present(registration->dialog, GTK_WIDGET(window->window));
	gtk_widget_grab_focus(registration->password);
}
