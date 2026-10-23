#ifndef RADIO_STORE_H
#define RADIO_STORE_H

#include <stdbool.h>

#include "radio.h"

// Favourites, recents and hand-added stations, in the app's data folder.
// The file is a few tab-separated lines; every call is safe when the card is
// gone, and says so by failing or returning zero.

#define RADIO_FAVOURITE_MAX 100
#define RADIO_RECENT_MAX 30
#define RADIO_CUSTOM_MAX 50

bool radio_store_load(void);
void radio_store_save(void);

bool radio_store_is_favourite(const radio_station_t *station);
// Adds or removes, saves, and returns true when the station is now a
// favourite.
bool radio_store_toggle_favourite(const radio_station_t *station);

int radio_store_favourites(radio_station_t *out, int max);
int radio_store_recent(radio_station_t *out, int max);
int radio_store_custom(radio_station_t *out, int max);

// Moves a station to the front of the recent list and saves.
void radio_store_note_played(const radio_station_t *station);

// Adds to the custom list unless the address is already there. Returns false
// when the list is full or the card cannot be written.
bool radio_store_add_custom(const radio_station_t *station);

#endif /* RADIO_STORE_H */
