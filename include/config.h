// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_BANDS 8

struct BandConfig {
    char name[16];
    double centerfreq;
    int samplerate;
    char device[256];
    int  device_type;
    int  device_ch;
    char audioformat[32];
    char stdinformat[32];
    double progfreq;
    int gain;
    bool swapiq;
    bool noniq;
    char balance[256];
    char equalize[256];
    bool noiseblanker;
    int extrazoom;
    int delay;
    float hpf;
    char antenna[64];

};

struct WebSDRConfig {
    int max_clients;
    int tcp_port;
    char public_dir[256];
    char public2_dir[256];
    char log_dir[256];
    char chroot_dir[256];

    char myhost[128];
    char hostname[128];
    char orgserver[128];
    char org_info[4096];
    bool noorgserver;

    double initial_freq;
    char initial_mode[8];

    int logfileinterval;
    int idletimeout;
    char waterfallformat[16];

    bool donttrustlocalhost;
    bool dotrustlocalnet;
    char dotrust[256];

    char chatboxlogfile[256];
    char chatrejectipranges[256];

    char rawpassword[256];
    int slowdownusers;
    int slowdownusers2;
    int fftplaneffort;
    int fftbackend;
    int audioformat;

    bool allowwide;

    int num_bands;
    struct BandConfig bands[MAX_BANDS];
};

extern struct WebSDRConfig config;
#define g_config config

int parse_config(const char *filename);

#endif
