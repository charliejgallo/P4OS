/*
 * P4OS - Radio (from AmoledOS): the list of stations.
 *
 * The portal's /radio page keeps "My list" on the card as
 * radio/library.json ({"v":1,"stations":[{"name":..,"url":..,"cc":..,
 * "codec":..,"kbps":..,"hls":..}, ...]}). The watch never read it: 368 px
 * had no room for more than the nine keys. The 5" screen shows it whole,
 * beside the dial. Without the file - the portal only writes it once the
 * list is edited - it is the same 33 stations the page starts with, checked
 * against the firmware's own stream code.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_LIB_MAX   64

typedef struct {
    char     name[48];
    char     url[256];
    char     cc[4];
    char     codec[8];
    uint16_t kbps;
    bool     hls;
} radio_lib_station_t;

typedef struct {
    radio_lib_station_t *s;     /* RADIO_LIB_MAX, PSRAM on the board        */
    int      count;
    bool     from_card;         /* library.json, not the built-in list      */
} radio_lib_t;

bool radio_lib_load(radio_lib_t *lib);
void radio_lib_free(radio_lib_t *lib);
int  radio_lib_find(const radio_lib_t *lib, const char *url);   /* -1 none */
