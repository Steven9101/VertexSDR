// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "common.h"
#include "config.h"
#include "band.h"
#include "dsp.h"
#include "audio.h"
#include "client.h"
#include "worker.h"
#include "fft_backend.h"
#include "orgserver.h"
#include "logging.h"
#include "waterfall.h"
#include "chat.h"
#include "logbook.h"

#ifdef USE_VULKAN
#include "vk_waterfall.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>

void init_network(int port);
void process_network_events(void);
void network_add_alsa_fd(int alsa_fd, int band_idx);
void network_flush_ws_clients(void);
void network_update_histogram(void);

static volatile int g_quit   = 0;

static volatile int g_reload = 0;

static void sig_quit(int s)   { (void)s; g_quit   = 1; }
static void sig_reload(int s) { (void)s; g_reload = 1; }

extern char g_config_file[256];

extern void ws_broadcast_text(const char *msg);

static void initialize_system(void)
{

    uid_t uid = getuid();
    if (seteuid(uid) != 0) {
        fprintf(stderr, "Startup failed while dropping privileges to uid %u: %s\n",
                (unsigned)uid, strerror(errno));
        exit(1);
    }

    struct sigaction act;
    act.sa_handler = SIG_IGN;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGPIPE, &act, NULL);

    act.sa_handler = sig_reload;
    sigaction(SIGHUP, &act, NULL);

    act.sa_handler = sig_quit;
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
}

static int select_initial_band_index(void)
{
    if (config.initial_freq <= 0.0)
        return -1;
    for (int i = 0; i < config.num_bands; i++) {
        const double half_span_khz = (double)config.bands[i].samplerate / 2000.0;
        const double min_khz = config.bands[i].centerfreq - half_span_khz - 4.0;
        const double max_khz = config.bands[i].centerfreq + half_span_khz + 4.0;
        if (config.initial_freq > min_khz && config.initial_freq < max_khz)
            return i;
    }
    return -1;
}

static void fill_band_from_config(struct BandState *b, int band_index, int initial_band_idx)
{
    memset(b, 0, sizeof(*b));
    b->fd = -1;
    b->fd2 = -1;
    b->rtltcp_fd = -1;
    b->band_index  = band_index;
    b->device_type = config.bands[band_index].device_type;
    b->device_ch   = config.bands[band_index].device_ch;
    b->samplerate  = config.bands[band_index].samplerate;
    b->centerfreq_hz  = config.bands[band_index].centerfreq * 1000.0;
    b->centerfreq_khz = config.bands[band_index].centerfreq;
    b->vfo_khz = (band_index == initial_band_idx) ? config.initial_freq : config.bands[band_index].centerfreq;
    b->noniq      = config.bands[band_index].noniq  ? 1 : 0;
    b->swapiq     = config.bands[band_index].swapiq ? 1 : 0;
    b->gain_db    = (float)config.bands[band_index].gain;
    b->gain_adj   = (float)config.bands[band_index].gain;
    b->noiseblanker = config.bands[band_index].noiseblanker ? 1 : 0;
    b->extrazoom  = config.bands[band_index].extrazoom;
    b->delay_samples = config.bands[band_index].delay * config.bands[band_index].samplerate;
    b->hpf_freq   = config.bands[band_index].hpf;
    b->device_name = config.bands[band_index].device;
    b->stdinformat_str = config.bands[band_index].stdinformat;
    strncpy(b->name, config.bands[band_index].name, sizeof(b->name) - 1);
}

static int reinitialize_bands_from_config(void)
{
    int old_count = num_bands;
    int new_count = config.num_bands;
    if (new_count < 0)
        new_count = 0;
    if (new_count > MAX_BANDS)
        new_count = MAX_BANDS;

    if (new_count < old_count) {
        for (int i = new_count; i < old_count; i++) {
            if (bands[i].started)
                band_deinit(&bands[i]);
            memset(&bands[i], 0, sizeof(bands[i]));
        }
        log_printf("Configuration reload removed %d band(s).\n", old_count - new_count);
    }
    num_bands = new_count;
    config.num_bands = new_count;

    if (config.fftplaneffort != fftplaneffort) {
        fftplaneffort = config.fftplaneffort;
        log_printf("Updated fft planning effort to %d. New FFT plans will be created on restart.\n",
                   fftplaneffort);
    }

    const int initial_band_idx = select_initial_band_index();

    for (int i = 0; i < new_count; i++) {
        struct BandState *b = &bands[i];
        const struct BandConfig *bc = &config.bands[i];
        if (!b->started)
            continue;

        if (b->samplerate != bc->samplerate ||
            b->noniq != (bc->noniq ? 1 : 0) ||
            b->swapiq != (bc->swapiq ? 1 : 0) ||
            strcmp(b->device_name ? b->device_name : "", bc->device) != 0) {
            log_printf("Band %d device or sample-rate settings changed; restart is required for a full apply.\n", i);
        }

        b->centerfreq_hz = bc->centerfreq * 1000.0;
        b->centerfreq_khz = bc->centerfreq;
        b->vfo_khz = (i == initial_band_idx && config.initial_freq > 0.0)
            ? config.initial_freq
            : bc->centerfreq;
        b->gain_db = (float)bc->gain;
        b->gain_adj = (float)bc->gain;
        b->noiseblanker = bc->noiseblanker ? 1 : 0;
        b->hpf_freq = bc->hpf;
        b->progfreq_khz = bc->progfreq;
        strncpy(b->name, bc->name, sizeof(b->name) - 1);
        b->name[sizeof(b->name) - 1] = '\0';
    }

    if (new_count > old_count) {
        for (int i = old_count; i < new_count; i++) {
            struct BandState *b = &bands[i];
            fill_band_from_config(b, i, initial_band_idx);

            if (band_init(b, 0) != 0) {
                log_printf("Configuration reload could not initialize band %d; keeping %d bands active.\n",
                           i, i);
                new_count = i;
                break;
            }
            if (config.bands[i].balance[0])
                band_load_balance(b, config.bands[i].balance);
            if (config.bands[i].equalize[0])
                band_load_equalize(b, config.bands[i].equalize);

            if (alsa_open_device(b) != 0) {
                log_printf("Configuration reload could not open the device for band %d; keeping %d bands active.\n",
                           i, i);
                if (b->started)
                    band_deinit(b);
                new_count = i;
                break;
            }
            if (b->fd >= 0)
                network_add_alsa_fd(b->fd, i);
        }
        if (new_count < config.num_bands) {
            for (int i = new_count; i < MAX_BANDS; i++)
                memset(&bands[i], 0, sizeof(bands[i]));
        }
        num_bands = new_count;
        config.num_bands = new_count;
    }

    for (int i = 0; i < MAX_BANDS; i++)
        bands[i].audio_clients = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (c->state == CLIENT_FREE)
            continue;
        if (c->band_idx >= new_count) {
            c->band_idx = (new_count > 0) ? 0 : -1;
            c->band_idx_req = c->band_idx;
            c->band_ptr = (c->band_idx >= 0) ? &bands[c->band_idx] : NULL;
            c->demod_filter_dirty = -1;
            c->uu_chseq = chat_get_pending_chseq();
        } else if (c->band_idx >= 0) {
            c->band_ptr = &bands[c->band_idx];
        } else {
            c->band_ptr = NULL;
        }

        if (c->state == CLIENT_WEBSOCKET && c->client_type == 0 &&
            c->band_idx >= 0 && c->band_idx < new_count) {
            bands[c->band_idx].audio_clients++;
        }
    }

    return 0;
}

static void regenerate_runtime_assets(void)
{
    if (!config.public_dir[0])
        return;

    char tmpdir_pngs[512];
    snprintf(tmpdir_pngs, sizeof(tmpdir_pngs), "%s/tmp", config.public_dir);
    mkdir(tmpdir_pngs, 0755);
    waterfall_init_palette();

    for (int i = 0; i < config.num_bands; i++) {
        struct BandState *b = &bands[i];
        if (!b->started)
            continue;
        int rc = waterfall_write_band_pngs(tmpdir_pngs, i,
                                           b->centerfreq_khz,
                                           b->samplerate,
                                           b->maxzoom);
        if (rc != 0)
            log_printf("Band %d label images could not be regenerated.\n", i);
    }

    if (band_write_bandinfo_js(config.public_dir) != 0)
        log_printf("bandinfo.js could not be regenerated.\n");
}

static void apply_config_reload(int how)
{
    pthread_mutex_lock(&worker_dsp_mutex);

    if (parse_config(g_config_file) != 0) {
        log_printf("Configuration reload failed for '%s'.\n", g_config_file);
        pthread_mutex_unlock(&worker_dsp_mutex);
        return;
    }

    if (!config.public_dir[0])
        strncpy(config.public_dir, "pub", sizeof(config.public_dir) - 1);

    if (reinitialize_bands_from_config() != 0) {
        pthread_mutex_unlock(&worker_dsp_mutex);
        return;
    }

    regenerate_runtime_assets();

    pthread_mutex_unlock(&worker_dsp_mutex);

    if (how == 2) {
        chat_force_reload_all();
        ws_broadcast_text("location.reload(true);");
    }
}

int main(int argc, char **argv)
{

    if (argc >= 2 && strcmp(argv[1], "--test-band") == 0) {
        double center_khz = (argc >= 3) ? atof(argv[2]) : 14200.0;
        double bw_khz     = (argc >= 4) ? atof(argv[3]) : 200.0;
        const char *out   = (argc >= 5) ? argv[4]       : "test_band.png";

        waterfall_init_palette();
        fprintf(stderr, "Rendering test band-label PNG: center=%.1f kHz, width=%.1f kHz, output=%s\n",
                center_khz, bw_khz, out);
        int rc = waterfall_write_bandlabel_png(out, center_khz, bw_khz, 1024);
        if (rc == 0)
            fprintf(stderr, "Wrote %s (1024x14 indexed PNG with 4-color palette).\n", out);
        else
            fprintf(stderr, "Failed to write %s.\n", out);
        return rc;
    }

    initialize_system();

    fprintf(stderr, "\n=== VertexSDR %s ===\n\n", VERSION_STRING);
    fprintf(stderr, "Copyright 2024-2026 magicint1337\n\n");

    if (argc > 1)
        strncpy(g_config_file, argv[1], sizeof(g_config_file) - 1);

    if (parse_config(g_config_file) != 0) {
        fprintf(stderr, "Configuration load failed for '%s'.\n", g_config_file);
        return 1;
    }

    if (!config.public_dir[0])
        strncpy(config.public_dir, "pub", sizeof(config.public_dir) - 1);

    {
        char tmpdir[512];
        snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", config.public_dir);
        mkdir(tmpdir, 0755);
        DIR *d = opendir(tmpdir);
        if (d) {
            struct dirent *ent;
            char fpath[768];
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                snprintf(fpath, sizeof(fpath), "%s/%s", tmpdir, ent->d_name);
                unlink(fpath);
            }
            closedir(d);
        }
    }

    logging_init();

    dsp_load_wisdom();

    fftplaneffort = config.fftplaneffort;

#ifdef USE_VULKAN
    if (config.fftbackend == FFT_BACKEND_VKFFT)
        vk_waterfall_init_global();
#endif

    fft_backend_set_requested(config.fftbackend);
    fft_backend_init_global();
    fprintf(stderr, "Selected FFT backend: %s\n", fft_backend_name(fft_backend_get_active()));

    dsp_init_demod_tables();

    client_init_all();

    worker_init(1);

    chat_init();
    logbook_init(config.public2_dir[0] ? config.public2_dir : config.public_dir);

    init_network(config.tcp_port);

    int initial_band_idx = select_initial_band_index();
    num_bands = config.num_bands;

    for (int i = 0; i < config.num_bands; i++) {
        struct BandState *b = &bands[i];
        fill_band_from_config(b, i, initial_band_idx);

        if (band_init(b, 0) != 0) {
            log_printf("Band %d initialization failed.\n", i);
            return 1;
        }

        if (config.bands[i].balance[0])
            band_load_balance(b, config.bands[i].balance);
        if (config.bands[i].equalize[0])
            band_load_equalize(b, config.bands[i].equalize);

        if (config.public_dir[0]) {
            char tmpdir_pngs[512];
            snprintf(tmpdir_pngs, sizeof(tmpdir_pngs), "%s/tmp", config.public_dir);
            mkdir(tmpdir_pngs, 0755);
            waterfall_init_palette();
            int rc = waterfall_write_band_pngs(tmpdir_pngs, i,
                                               b->centerfreq_khz,
                                               b->samplerate,
                                               b->maxzoom);
            if (rc != 0)
                fprintf(stderr, "Band %d label images could not be written.\n", i);
        }
    }

    dsp_save_wisdom();

    if (config.public_dir[0]) {
        if (band_write_bandinfo_js(config.public_dir) != 0)
            fprintf(stderr, "bandinfo.js could not be written.\n");
    }

    for (int i = 0; i < config.num_bands; i++) {
        struct BandState *b = &bands[i];

        if (alsa_open_device(b) != 0) {
            fprintf(stderr, "Audio input startup failed for band %d.\n", i);
            return 1;
        }

        if (b->fd >= 0 && b->device_type != DEVTYPE_STDIN)
            network_add_alsa_fd(b->fd, i);
    }

    if (config.chroot_dir[0]) {
        if (seteuid(0) != 0) {
            fprintf(stderr, "Privilege elevation for chroot failed: %s\n", strerror(errno));
            exit(1);
        }
        if (chroot(config.chroot_dir) != 0) {
            fprintf(stderr, "chroot to '%s' failed: %s\n", config.chroot_dir, strerror(errno));
            log_printf("chroot to '%s' failed: %s\n", config.chroot_dir, strerror(errno));
            exit(1);
        }
        uid_t ruid = getuid();
        if (seteuid(ruid) != 0) {
            fprintf(stderr, "Privilege drop after chroot failed for uid %u: %s\n",
                    (unsigned)ruid, strerror(errno));
            exit(1);
        }
    }

    if (setuid(getuid()) != 0) {
        fprintf(stderr, "Final privilege drop failed: %s\n", strerror(errno));
        exit(1);
    }

    if (!config.noorgserver)
        orgserver_start();

    dsp_save_wisdom();

    puts("\nInitialization complete. VertexSDR is now running.");

    time_t last_housekeeping = 0;

    while (!g_quit) {

        process_network_events();

        time_t now_s = time(NULL);
        if (now_s != last_housekeeping) {
            last_housekeeping = now_s;
            log_flush_dsp();
            logging_rotate();
            network_update_histogram();
        }

        if (g_reload) {
            g_reload = 0;
            log_printf("Reloading configuration after SIGHUP.\n");
            apply_config_reload(1);
        }

        if (g_configreload) {
            int how = g_configreload;
            g_configreload = 0;
            if (how == 3) {
                log_printf("Shutdown requested through ~~configreload.\n");
                g_quit = 1;
            } else {
                log_printf("Reloading configuration with request code %d.\n", how);
                apply_config_reload(how);
            }
        }
    }

    log_printf("Shutting down VertexSDR.\n");

    for (int i = 0; i < config.num_bands; i++)
        band_deinit(&bands[i]);

    dsp_destroy_demod_tables();
    dsp_save_wisdom();
    fft_backend_shutdown_global();
#ifdef USE_VULKAN
    if (config.fftbackend == FFT_BACKEND_VKFFT)
        vk_waterfall_shutdown_global();
#endif

    return 0;
}
