#include "radio_stations.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "RadioStations";

static const radio_station_t s_builtin_stations[] = {
    {
        .name = "Groove Salad",
        .category = "English",
        .urls = {
            "https://ice5.somafm.com/groovesalad-128-mp3",
            "",
            ""
        }
    },
    {
        .name = "SomaFM Live",
        .category = "English",
        .urls = {
            "https://ice5.somafm.com/live-128-mp3",
            "",
            ""
        }
    },
    {
        .name = "500 Chinese Classics",
        .category = "Chinese",
        .urls = {
            "https://lhttp.qtfm.cn/live/5022308/64k.mp3",
            "https://lhttp-hw.qtfm.cn/live/5022308/64k.mp3",
            "http://lhttp.qingting.fm/live/5022308/64k.mp3"
        }
    },
    {
        .name = "Guangdong Music",
        .category = "Chinese",
        .urls = {
            "http://lhttp.qingting.fm/live/1260/64k.mp3",
            "https://lhttp.qtfm.cn/live/1260/64k.mp3",
            ""
        }
    },
    {
        .name = "Guangzhou Traffic",
        .category = "Chinese",
        .urls = {
            "http://lhttp.qingting.fm/live/4955/64k.mp3",
            "https://lhttp.qtfm.cn/live/4955/64k.mp3",
            ""
        }
    },
};

static radio_station_t s_stations[RADIO_MAX_STATIONS];
static int s_station_count = 0;

void radio_stations_init(void)
{
    s_station_count = sizeof(s_builtin_stations) / sizeof(s_builtin_stations[0]);
    if (s_station_count > RADIO_MAX_STATIONS) {
        s_station_count = RADIO_MAX_STATIONS;
    }
    memcpy(s_stations, s_builtin_stations, s_station_count * sizeof(radio_station_t));
    ESP_LOGI(TAG, "Loaded %d built-in stations", s_station_count);
}

int radio_stations_count(void)
{
    return s_station_count;
}

const radio_station_t *radio_station_get(int index)
{
    if (index < 0 || index >= s_station_count) {
        return NULL;
    }
    return &s_stations[index];
}

const char *radio_station_url_get(int station_index, int url_index)
{
    const radio_station_t *station = radio_station_get(station_index);
    if (!station || url_index < 0 || url_index >= RADIO_MAX_URLS) {
        return NULL;
    }
    return station->urls[url_index][0] ? station->urls[url_index] : NULL;
}
