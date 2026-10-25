#pragma once

#include "images.h"

/* What a web page says about itself in its head, for the card under a message that links to it */
typedef struct {
	char *url, *site, *title, *description, *image_url;
} LinkPreview;

/* The page's preview once it has loaded, or NULL while it loads, after it failed or for a page without a title */
const LinkPreview *get_link_preview(const char *url);
/* Loads a page's preview and calls back with its owner once it is ready, unless the owner is gone by then */
void preload_link_preview(const char *url, ImageReady ready, GObject *owner);
