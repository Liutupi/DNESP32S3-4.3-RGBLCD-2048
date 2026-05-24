#ifndef RADIO_STATIONS_H
#define RADIO_STATIONS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MAX_FALLBACK_URLS 3

typedef struct {
    const char *name;
    const char *category;
    const char *url;
    const char *fallback_urls[RADIO_MAX_FALLBACK_URLS];
    const char *type;
    bool enabled;
} radio_station_t;

void radio_stations_init(void);
int radio_stations_count(void);
const radio_station_t *radio_stations_get(int index);
int radio_stations_next_enabled_mp3(int current_index);
int radio_stations_first_enabled_mp3(void);

const radio_station_t *radio_station_get(int index);
const char *radio_station_url_get(int station_index, int url_index);

#ifdef __cplusplus
}
#endif

#endif
