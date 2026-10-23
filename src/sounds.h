#pragma once

/* A short sound for a message in the chat on screen, and a longer one for a mention or private message elsewhere */
typedef enum {
	SOUND_MESSAGE,
	SOUND_MENTION,
} SoundKind;

/* Plays the sound from its start, unless the user switched sounds off */
void play_sound(SoundKind kind);
