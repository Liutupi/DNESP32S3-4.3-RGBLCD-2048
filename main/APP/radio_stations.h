#ifndef RADIO_STATIONS_H
#define RADIO_STATIONS_H

#include <stdbool.h>
#include <stddef.h>

#define RADIO_MAX_URLS      3
#define RADIO_MAX_NAME      64
#define RADIO_MAX_CATEGORY  32
#define RADIO_MAX_URL       192
#define RADIO_MAX_STATIONS  16

typedef struct {
    char name[RADIO_MAX_NAME];
    char category[RADIO_MAX_CATEGORY];
    char urls[RADIO_MAX_URLS][RADIO_MAX_URL];
} radio_station_t;

void radio_stations_init(void);
int radio_stations_count(void);
const radio_station_t *radio_station_get(int index);
const char *radio_station_url_get(int station_index, int url_index);

#endif
