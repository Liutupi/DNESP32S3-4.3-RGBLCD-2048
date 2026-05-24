#include "radio_stations.h"

#include "esp_log.h"

#include <string.h>

#define TAG "RADIO_STATIONS"

static const char *STATIONS_SOURCE_URL =
    "https://raw.githubusercontent.com/Liutupi/xiaozhi-s3-radio-math/main/stations.json";

static const radio_station_t s_stations[] = {
    {"CNR China Voice", "News", "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
     {"https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3", NULL, NULL}, "mp3", true},
    {"Guangzhou News", "Guangdong", "http://lhttp.qingting.fm/live/4848/64k.mp3",
     {"https://lhttp.qtfm.cn/live/4848/64k.mp3", NULL, NULL}, "mp3", true},
    {"Guangzhou Traffic", "Traffic", "http://lhttp.qingting.fm/live/4955/64k.mp3",
     {"https://lhttp.qtfm.cn/live/4955/64k.mp3", NULL, NULL}, "mp3", true},
    {"Pearl River Economic Radio", "Guangdong", "http://lhttp.qingting.fm/live/1259/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1259/64k.mp3", NULL, NULL}, "mp3", true},
    {"Guangdong Music Radio", "Guangdong", "http://lhttp.qingting.fm/live/1260/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1260/64k.mp3", NULL, NULL}, "mp3", true},
    {"Guangdong Sports Radio", "Guangdong", "https://lhttp.qtfm.cn/live/471/64k.mp3",
     {"https://lhttp-hw.qtfm.cn/live/471/64k.mp3", NULL, NULL}, "mp3", true},
    {"Shenzhen Flying 971", "Guangdong", "http://lhttp.qingting.fm/live/1271/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1271/64k.mp3", NULL, NULL}, "mp3", true},
    {"Asia Cantonese", "Chinese Music", "https://lhttp.qingting.fm/live/15318569/64k.mp3",
     {"https://lhttp.qtfm.cn/live/15318569/64k.mp3", NULL, NULL}, "mp3", true},
    {"Chinese Classics 500", "Chinese Music", "https://lhttp.qtfm.cn/live/5022308/64k.mp3",
     {"https://lhttp-hw.qtfm.cn/live/5022308/64k.mp3", "http://lhttp.qingting.fm/live/5022308/64k.mp3", NULL}, "mp3", true},
    {"Morning Music", "Chinese Music", "http://lhttp.qingting.fm/live/4915/64k.mp3",
     {"https://lhttp.qtfm.cn/live/4915/64k.mp3", NULL, NULL}, "mp3", true},
    {"Nostalgic Voice", "Chinese Music", "http://lhttp.qingting.fm/live/1223/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1223/64k.mp3", NULL, NULL}, "mp3", true},
    {"959 Era Music", "Chinese Music", "http://lhttp.qingting.fm/live/5021381/64k.mp3",
     {"https://lhttp.qtfm.cn/live/5021381/64k.mp3", NULL, NULL}, "mp3", true},
    {"Touching Music", "Chinese Music", "http://lhttp.qingting.fm/live/5022107/64k.mp3",
     {"https://lhttp.qtfm.cn/live/5022107/64k.mp3", NULL, NULL}, "mp3", true},
    {"Jiangsu Classic Pop", "Chinese Music", "http://lhttp.qingting.fm/live/4938/64k.mp3",
     {"https://lhttp.qtfm.cn/live/4938/64k.mp3", NULL, NULL}, "mp3", true},

    {"Groove Salad", "English", "https://ice5.somafm.com/groovesalad-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"SomaFM Live", "English", "https://ice5.somafm.com/live-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"n5MD Radio", "English", "https://ice5.somafm.com/n5md-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"The In-Sound", "English", "https://ice5.somafm.com/insound-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"Dark Zone", "English", "https://ice5.somafm.com/darkzone-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"Mission Control", "English", "https://ice5.somafm.com/missioncontrol-128-mp3",
     {NULL, NULL, NULL}, "mp3", true},
    {"Jiangsu News Radio", "News", "https://lhttp.qtfm.cn/live/4944/64k.mp3",
     {"https://lhttp-hw.qtfm.cn/live/4944/64k.mp3", NULL, NULL}, "mp3", true},
    {"Anhui General Radio", "News", "https://lhttp.qingting.fm/live/4919/64k.mp3",
     {"https://lhttp.qtfm.cn/live/4919/64k.mp3", NULL, NULL}, "mp3", true},
    {"Guangdong Pearl River Voice", "Guangdong", "http://lhttp.qingting.fm/live/470/64k.mp3",
     {"https://lhttp.qtfm.cn/live/470/64k.mp3", NULL, NULL}, "mp3", true},
    {"Huizhou Music Radio", "Guangdong", "http://lhttp.qingting.fm/live/5021523/64k.mp3",
     {"https://lhttp.qtfm.cn/live/5021523/64k.mp3", NULL, NULL}, "mp3", true},
    {"Maoming Traffic Radio", "Traffic", "https://lhttp.qingting.fm/live/20211574/64k.mp3",
     {"https://lhttp.qtfm.cn/live/20211574/64k.mp3", NULL, NULL}, "mp3", true},
    {"Liangguang Voice Music", "Chinese Music", "http://lhttp.qingting.fm/live/20500149/64k.mp3",
     {"https://lhttp.qtfm.cn/live/20500149/64k.mp3", NULL, NULL}, "mp3", true},
    {"Shanghai Love Radio", "Chinese Music", "http://lhttp.qingting.fm/live/273/64k.mp3",
     {"https://lhttp.qtfm.cn/live/273/64k.mp3", NULL, NULL}, "mp3", true},
    {"Shanghai Dynamic 101", "Chinese Music", "http://lhttp.qingting.fm/live/274/64k.mp3",
     {"https://lhttp.qtfm.cn/live/274/64k.mp3", NULL, NULL}, "mp3", true},
    {"Beijing Music Radio", "Chinese Music", "http://lhttp.qingting.fm/live/332/64k.mp3",
     {"https://lhttp.qtfm.cn/live/332/64k.mp3", NULL, NULL}, "mp3", true},
    {"Hubei Classic Music", "Chinese Music", "http://lhttp.qingting.fm/live/1296/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1296/64k.mp3", NULL, NULL}, "mp3", true},
    {"Wuhan Classic Music", "Chinese Music", "http://lhttp.qingting.fm/live/1297/64k.mp3",
     {"https://lhttp.qtfm.cn/live/1297/64k.mp3", NULL, NULL}, "mp3", true},
    {"90.7 MIX FM", "Chinese Music", "https://lhttp.qingting.fm/live/15318146/64k.mp3",
     {"https://lhttp.qtfm.cn/live/15318146/64k.mp3", NULL, NULL}, "mp3", true},
    {"Baohe Voice FM100.8", "Local", "https://lhttp-hw.qtfm.cn/live/5022668/64k.mp3",
     {"https://lhttp.qtfm.cn/live/5022668/64k.mp3", NULL, NULL}, "mp3", true},
};

static bool s_logged;

static bool station_is_enabled_mp3(const radio_station_t *station)
{
    return station && station->enabled && station->type && strcmp(station->type, "mp3") == 0;
}

void radio_stations_init(void)
{
    if (!s_logged) {
        ESP_LOGI(TAG, "RADIO_STATIONS_SOURCE url=%s", STATIONS_SOURCE_URL);
        ESP_LOGI(TAG, "RADIO_STATIONS_LOADED count=%d", radio_stations_count());
        int first = radio_stations_first_enabled_mp3();
        ESP_LOGI(TAG, "RADIO_FIRST_STATION index=%d", first);
        s_logged = true;
    }
}

int radio_stations_count(void)
{
    return (int)(sizeof(s_stations) / sizeof(s_stations[0]));
}

const radio_station_t *radio_stations_get(int index)
{
    if (index < 0 || index >= radio_stations_count()) {
        return NULL;
    }
    return &s_stations[index];
}

int radio_stations_next_enabled_mp3(int current_index)
{
    int count = radio_stations_count();
    if (count <= 0) {
        return -1;
    }
    for (int step = 1; step <= count; ++step) {
        int index = (current_index + step + count) % count;
        if (station_is_enabled_mp3(&s_stations[index])) {
            return index;
        }
    }
    return -1;
}

int radio_stations_first_enabled_mp3(void)
{
    for (int i = 0; i < radio_stations_count(); ++i) {
        if (station_is_enabled_mp3(&s_stations[i])) {
            return i;
        }
    }
    return -1;
}

const radio_station_t *radio_station_get(int index)
{
    return radio_stations_get(index);
}

const char *radio_station_url_get(int station_index, int url_index)
{
    const radio_station_t *station = radio_stations_get(station_index);
    if (!station) {
        return NULL;
    }
    if (url_index == 0) {
        return station->url;
    }
    if (url_index > 0 && url_index <= RADIO_MAX_FALLBACK_URLS) {
        return station->fallback_urls[url_index - 1];
    }
    return NULL;
}
