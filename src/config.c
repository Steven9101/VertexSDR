// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "config.h"
#include "fft_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct WebSDRConfig config;

static void trim_whitespace(char *str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

static void append_org_info_line(const char *subkey, const char *value)
{
    if (!subkey || !subkey[0]) return;

    char label[64];
    int li = 0;
    for (int i = 0; subkey[i] && li < (int)sizeof(label) - 1; i++) {
        unsigned char ch = (unsigned char)subkey[i];
        if (ch <= ' ') break;
        if (i == 0) label[li++] = (char)toupper(ch);
        else        label[li++] = (char)tolower(ch);
    }
    label[li] = '\0';
    if (!label[0]) return;

    char line[640];
    if (value && value[0])
        snprintf(line, sizeof(line), "%s: %s\n", label, value);
    else
        snprintf(line, sizeof(line), "%s:\n", label);

    size_t used = strlen(config.org_info);
    size_t left = sizeof(config.org_info) - used - 1;
    if (left == 0) return;
    strncat(config.org_info, line, left);
}

int parse_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Could not open config file %s\n", filename);
        return -1;
    }

    memset(&config, 0, sizeof(config));

    config.tcp_port = 8901;
    config.max_clients = 100;
    config.fftbackend = FFT_BACKEND_FFTW;

    struct BandConfig *current_band = NULL;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char *key = strtok(line, " \t");
        if (!key) continue;
        char *val = strtok(NULL, "");

        char empty[] = "";
        if (!val) val = empty;
        else trim_whitespace(val);

        if (strcmp(key, "maxusers") == 0) {
            config.max_clients = atoi(val);
        } else if (strcmp(key, "tcpport") == 0) {
            config.tcp_port = atoi(val);
        } else if (strcmp(key, "public") == 0) {
            strncpy(config.public_dir, val, sizeof(config.public_dir)-1);
        } else if (strcmp(key, "public2") == 0) {
            strncpy(config.public2_dir, val, sizeof(config.public2_dir)-1);
        } else if (strcmp(key, "logdir") == 0) {
            strncpy(config.log_dir, val, sizeof(config.log_dir)-1);
        } else if (strcmp(key, "chroot") == 0) {
            strncpy(config.chroot_dir, val, sizeof(config.chroot_dir)-1);
        } else if (strcmp(key, "myhost") == 0) {
            strncpy(config.myhost, val, sizeof(config.myhost)-1);
        } else if (strcmp(key, "hostname") == 0) {
            strncpy(config.hostname, val, sizeof(config.hostname)-1);
        } else if (strcmp(key, "orgserver") == 0) {
            strncpy(config.orgserver, val, sizeof(config.orgserver)-1);
        } else if (strcmp(key, "noorgserver") == 0) {
            config.noorgserver = true;
            (void)val;
        } else if (strcmp(key, "idletimeout") == 0) {
            config.idletimeout = atoi(val);
        } else if (strcmp(key, "logfileinterval") == 0) {
            config.logfileinterval = atoi(val);
        } else if (strcmp(key, "slowdownusers") == 0) {
            config.slowdownusers = atoi(val);
        } else if (strcmp(key, "slowdownusers2") == 0) {
            config.slowdownusers2 = atoi(val);
        } else if (strcmp(key, "fftplaneffort") == 0) {
            config.fftplaneffort = atoi(val);
        } else if (strcmp(key, "fftbackend") == 0) {
            if (strcasecmp(val, "vkfft") == 0)
                config.fftbackend = FFT_BACKEND_VKFFT;
            else
                config.fftbackend = FFT_BACKEND_FFTW;
        } else if (strcmp(key, "waterfallformat") == 0) {
            strncpy(config.waterfallformat, val, sizeof(config.waterfallformat)-1);
        } else if (strcmp(key, "audioformat") == 0) {
            config.audioformat = (int)strtol(val, NULL, 10);
        } else if (strcmp(key, "allowwide") == 0) {
            config.allowwide = true;
        } else if (strcmp(key, "donttrustlocalhost") == 0) {
            config.donttrustlocalhost = true;
        } else if (strcmp(key, "dotrust") == 0) {
            strncpy(config.dotrust, val, sizeof(config.dotrust)-1);
        } else if (strcmp(key, "dotrustlocalnet") == 0) {
            config.dotrustlocalnet = true;
        } else if (strcmp(key, "chatboxlogfile") == 0) {
            strncpy(config.chatboxlogfile, val, sizeof(config.chatboxlogfile)-1);
        } else if (strcmp(key, "chatrejectipranges") == 0) {
            strncpy(config.chatrejectipranges, val, sizeof(config.chatrejectipranges)-1);
        } else if (strcmp(key, "rawpassword") == 0) {
            strncpy(config.rawpassword, val, sizeof(config.rawpassword)-1);
        } else if (strcmp(key, "org") == 0) {
            char subkey[64] = "";
            char subval[512] = "";
            int n = sscanf(val, "%63s %511[^\n]", subkey, subval);
            if (n >= 1) {
                trim_whitespace(subval);
                append_org_info_line(subkey, (n >= 2) ? subval : "");
            }
        } else if (strcmp(key, "initial") == 0) {
            sscanf(val, "%lf %7s", &config.initial_freq, config.initial_mode);
        } else if (strcmp(key, "band") == 0) {
            if (config.num_bands < MAX_BANDS) {
                current_band = &config.bands[config.num_bands++];
                strncpy(current_band->name, val, sizeof(current_band->name)-1);
            } else {
                fprintf(stderr, "Too many bands defined.\n");
            }
        }

        else if (current_band) {
            if (strcmp(key, "centerfreq") == 0) {
                current_band->centerfreq = atof(val);
            } else if (strcmp(key, "samplerate") == 0) {
                current_band->samplerate = atoi(val);
            } else if (strcmp(key, "device") == 0) {

                char arg1[256] = "", arg2[256] = "";
                if (sscanf(val, "%255s %255[^\n]", arg1, arg2) == 2
                    && arg1[0] >= '0' && arg1[0] <= '9' && !arg1[1]) {

                    current_band->device_ch = atoi(arg1);
                    snprintf(current_band->device, sizeof(current_band->device), "%s", arg2);
                    current_band->device_type = 0;
                } else {
                    snprintf(current_band->device, sizeof(current_band->device), "%s", val);
                }

                const char *v = current_band->device;
                if (*v == '!') v++;
                if (strncasecmp(v, "rtlsdr", 6) == 0 || strncmp(v, "rtl:", 4) == 0)
                    current_band->device_type = 5;
                else if (strncasecmp(v, "tcpsdr", 6) == 0 || strncmp(v, "tcp:", 4) == 0)
                    current_band->device_type = 6;
                else if (strncasecmp(v, "stdin", 5) == 0)
                    current_band->device_type = 7;
            } else if (strcmp(key, "audioformat") == 0) {
                strncpy(current_band->audioformat, val, sizeof(current_band->audioformat)-1);
            } else if (strcmp(key, "stdinformat") == 0) {
                strncpy(current_band->stdinformat, val, sizeof(current_band->stdinformat)-1);
            } else if (strcmp(key, "progfreq") == 0) {
                current_band->progfreq = atof(val);
            } else if (strcmp(key, "swapiq") == 0) {
                current_band->swapiq = true;
            } else if (strcmp(key, "noniq") == 0) {
                current_band->noniq = true;
            } else if (strcmp(key, "gain") == 0) {
                current_band->gain = atoi(val);
            } else if (strcmp(key, "antenna") == 0) {
                strncpy(current_band->antenna, val, sizeof(current_band->antenna)-1);
            } else if (strcmp(key, "noiseblanker") == 0) {
                current_band->noiseblanker = atoi(val);
            } else if (strcmp(key, "extrazoom") == 0) {
                current_band->extrazoom = atoi(val);
            } else if (strcmp(key, "delay") == 0) {
                current_band->delay = atoi(val);
            } else if (strcmp(key, "hpf") == 0) {
                current_band->hpf = (float)atof(val);
            } else if (strcmp(key, "balance") == 0) {
                strncpy(current_band->balance, val, sizeof(current_band->balance) - 1);
            } else if (strcmp(key, "equalize") == 0) {
                strncpy(current_band->equalize, val, sizeof(current_band->equalize) - 1);
            }
        }
    }

    fclose(f);
    return 0;
}
