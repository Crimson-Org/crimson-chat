#pragma once

#include <adwaita.h>

/* An address that names an icon the app ships instead of a picture on the web */
#define ICON_PICTURE_PREFIX "icon:"
/* Loads a picture from the web once and shares it, keeping avatars on disk so they show at once on the next start */
void show_avatar_image(AdwAvatar *avatar, const char *url);
typedef void (*ImageReady)(GObject *owner);
/* The picture at the address once it has loaded, or NULL while it loads or after it failed */
GdkTexture *get_loaded_image(const char *url);
/* Loads a picture and calls back with its owner once it is ready, unless the owner is gone by then */
void preload_image(const char *url, ImageReady ready, GObject *owner);
/* Replaces the {size} placeholder some avatar and icon addresses carry with a pixel size */
char *fill_image_size(const char *url, int size);
