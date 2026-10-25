#pragma once

#include <adwaita.h>

#include "model.h"

/* The settings of a joined room, which moderators change and everyone else sees, with its ban, exception and
 * invite lists a page further */
AdwNavigationPage *create_room_settings_page(CrimsonRoom *room);
