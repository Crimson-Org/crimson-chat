#include <gtk/gtk.h>

#include "settings.h"
#include "sounds.h"

#define SOUNDS_PATH "/io/github/CrimsonOrg/CrimsonChat/sounds/"

static const char *const SOUND_FILES[] = {"message.ogg", "mention.ogg"};
static GtkMediaStream *streams[G_N_ELEMENTS(SOUND_FILES)];


void play_sound(SoundKind kind) {
	if (!get_settings()->plays_sounds)
		return;
	if (!streams[kind]) {
		g_autofree char *path = g_strconcat(SOUNDS_PATH, SOUND_FILES[kind], NULL);
		streams[kind] = gtk_media_file_new_for_resource(path);
	}
	/* A sound asked for again while it still plays starts over */
	if (gtk_media_stream_is_prepared(streams[kind]))
		gtk_media_stream_seek(streams[kind], 0);
	gtk_media_stream_play(streams[kind]);
}
