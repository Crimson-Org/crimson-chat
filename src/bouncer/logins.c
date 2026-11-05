#include <string.h>

#include "logins.h"
#include "setup.h"

/* Hashes running at once, and checks waiting for them, beyond which a login is refused at once */
#define MOST_RUNNING_CHECKS 2
#define MOST_WAITING_CHECKS 32
/* An address that fails this often within the window is refused until the window has passed */
#define MOST_FAILED_LOGINS 10
#define FAILED_LOGIN_WINDOW_SECONDS (15 * 60)

/* One name and password on their way through the checks */
typedef struct {
	Bouncer *bouncer;
	char *host, *password, *hash;
	gint64 user_id;
	gboolean is_correct;
	GCancellable *cancellable;
	LoginChecked checked;
	gpointer data;
} LoginCheck;

/* How often an address failed since its first failure in the window */
typedef struct {
	guint count;
	gint64 first;
} FailedLogins;

static GQueue waiting = G_QUEUE_INIT;
static guint running;
static GHashTable *failures;

static void start_waiting_checks(void);


static void free_check(LoginCheck *check) {
	g_free(check->host);
	memset(check->password, 0, strlen(check->password));
	g_free(check->password);
	g_free(check->hash);
	g_clear_object(&check->cancellable);
	g_free(check);
}


static gint64 get_seconds(void) {
	return g_get_monotonic_time() / G_USEC_PER_SEC;
}


static gboolean has_window_passed(gpointer host, gpointer value, gpointer data) {
	FailedLogins *failed = value;
	return get_seconds() - failed->first >= FAILED_LOGIN_WINDOW_SECONDS;
}


static gboolean is_held_back(const char *host) {
	if (!failures)
		failures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_foreach_remove(failures, has_window_passed, NULL);
	FailedLogins *failed = g_hash_table_lookup(failures, host);
	return failed && failed->count >= MOST_FAILED_LOGINS;
}


static void note_failure(const char *host) {
	FailedLogins *failed = g_hash_table_lookup(failures, host);
	if (!failed) {
		failed = g_new0(FailedLogins, 1);
		failed->first = get_seconds();
		g_hash_table_insert(failures, g_strdup(host), failed);
	}
	failed->count++;
}


static void answer_check(LoginCheck *check) {
	if (!check->is_correct && check->hash)
		note_failure(check->host);
	if (!g_cancellable_is_cancelled(check->cancellable))
		check->checked(check->is_correct ? find_user(check->bouncer, check->user_id) : NULL, check->data);
	free_check(check);
}


static gboolean answer_refused_check(gpointer data) {
	answer_check(data);
	return G_SOURCE_REMOVE;
}


static void hash_check(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	LoginCheck *check = data;
	check->is_correct = check_password(check->password, check->hash) && check->user_id;
	g_task_return_boolean(task, TRUE);
}


static void finish_check(GObject *source, GAsyncResult *result, gpointer data) {
	running--;
	answer_check(g_task_get_task_data(G_TASK(result)));
	start_waiting_checks();
}


static void start_waiting_checks(void) {
	while (running < MOST_RUNNING_CHECKS && !g_queue_is_empty(&waiting)) {
		LoginCheck *check = g_queue_pop_head(&waiting);
		g_autoptr(GTask) task = g_task_new(NULL, NULL, finish_check, NULL);
		g_task_set_task_data(task, check, NULL);
		running++;
		g_task_run_in_thread(task, hash_check);
	}
}


/* An unknown name is hashed against a stand-in, so it answers no faster than a wrong password */
static char *find_hash(Bouncer *bouncer, const char *name, gint64 *user_id) {
	static char *stand_in;
	for (guint index = 0; index < bouncer->users->len; index++) {
		BouncerUser *user = g_ptr_array_index(bouncer->users, index);
		if (g_strcmp0(user->name, name) == 0) {
			*user_id = user->id;
			return g_strdup(user->password_hash);
		}
	}
	if (!stand_in)
		stand_in = hash_password(BOUNCER_NAME);
	return g_strdup(stand_in);
}


void check_login(Bouncer *bouncer, const char *host, const char *name, const char *password,
	GCancellable *cancellable, LoginChecked checked, gpointer data) {
	LoginCheck *check = g_new0(LoginCheck, 1);
	*check = (LoginCheck){bouncer, g_strdup(host), g_strdup(password ? password : ""), NULL, 0, FALSE,
		cancellable ? g_object_ref(cancellable) : NULL, checked, data};
	if (!name || !password || is_held_back(host) || g_queue_get_length(&waiting) >= MOST_WAITING_CHECKS) {
		g_idle_add(answer_refused_check, check);
		return;
	}
	check->hash = find_hash(bouncer, name, &check->user_id);
	g_queue_push_tail(&waiting, check);
	start_waiting_checks();
}
