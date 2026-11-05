#include <gtk/gtk.h>

#include "app/settings.h"
#include "app/sounds.h"
#include "common/app/identity.h"

#define SOUNDS_FOLDER "sounds/"

static const char *const SOUND_FILES[] = {"message.ogg", "mention.ogg"};
static GtkMediaStream *streams[G_N_ELEMENTS(SOUND_FILES)];


void play_sound(SoundKind kind) {
	if (!get_settings()->plays_sounds)
		return;
	if (!streams[kind]) {
		g_autofree char *name = g_strconcat(SOUNDS_FOLDER, SOUND_FILES[kind], NULL);
		g_autofree char *path = get_resource_path(name);
		streams[kind] = gtk_media_file_new_for_resource(path);
	}
	/* A sound asked for again while it still plays starts over */
	if (gtk_media_stream_is_prepared(streams[kind]))
		gtk_media_stream_seek(streams[kind], 0);
	gtk_media_stream_play(streams[kind]);
}
