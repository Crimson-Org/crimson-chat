#pragma once

#include "window.h"

/* The rooms a server lists, busiest first, to tick several and join them at once or to type one that is not
 * listed; the page closes its dialog once the rooms are joined */
AdwNavigationPage *create_room_picker_page(Window *window, CrimsonNetwork *network, AdwDialog *dialog);
void show_room_picker(Window *window, CrimsonNetwork *network);
