#pragma once

#include <stdbool.h>

typedef struct {
    int  wmo;    // wttr.in weather code (113–395)
    int  temp;   // temperature °C
    int  hour;   // 0–23
} weather_hour_t;

typedef struct {
    int  temp_c;        // current temperature °C
    int  humidity;      // current humidity %
    int  wind_kmh;      // current wind speed km/h
    int  wmo;           // current wttr.in weather code (113–395)
    char condition[32]; // weather description, e.g. "Partly Cloudy"
    char location[64];  // city name from ip-api.com
    char tz_posix[32];  // POSIX TZ string, e.g. "UTC-8"
    bool valid;
    bool tz_valid;
    weather_hour_t hourly[6];  // next 3-hour slots (up to 6)
    bool hourly_valid;
} weather_data_t;

void weather_init(void);
void weather_get(weather_data_t *out);
