/*
 * gpu_metrics_fix.c
 *
 * Patches the broken GPU utilization field in amdgpu's gpu_metrics sysfs.
 * Some AMD APUs (BC-250 / PS5 Oberon) report 0xFFFF there permanently,
 * which makes MangoHud show 655%. We compute real usage from DRM fdinfo
 * and inject it via bind mount.
 *
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <getopt.h>

#define DEFAULT_CARD    "card1"
#define PATCHED_DIR     "/var/lib/gpu-metrics-fix"
#define PATCHED_FILE    "patched_metrics"
#define METRICS_SIZE    128
#define USAGE_OFFSET    0x1C
#define INTERVAL_US     1000000
#define MAX_CLIENTS     4096

static volatile sig_atomic_t running = 1;
static char sysfs_path[256];
static char patched_path[256];

static void on_signal(int sig) { (void)sig; running = 0; }

/* Walk proc fdinfo and sum up drm-engine-gfx ns per client id */
static uint64_t get_gfx_time(void)
{
    static uint64_t keys[MAX_CLIENTS];
    static uint64_t vals[MAX_CLIENTS];
    memset(keys, 0, sizeof(keys));
    memset(vals, 0, sizeof(vals));

    DIR *proc = opendir("/proc");
    if (!proc) return 0;

    struct dirent *pe;
    while ((pe = readdir(proc))) {
        if (pe->d_name[0] < '0' || pe->d_name[0] > '9')
            continue;

        char dir[64];
        snprintf(dir, sizeof(dir), "/proc/%s/fdinfo", pe->d_name);

        DIR *fdi = opendir(dir);
        if (!fdi) continue;

        struct dirent *fe;
        while ((fe = readdir(fdi))) {
            if (fe->d_name[0] == '.') continue;

            char path[128];
            snprintf(path, sizeof(path), "%s/%s", dir, fe->d_name);

            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;

            char buf[2048];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';

            uint64_t cid = 0, gfx = 0;
            int has_cid = 0;

            char *line = buf;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';

                if (!strncmp(line, "drm-client-id:", 14)) {
                    cid = strtoull(line + 14, NULL, 10);
                    has_cid = 1;
                } else if (!strncmp(line, "drm-engine-gfx:", 15)) {
                    gfx = strtoull(line + 15, NULL, 10);
                }

                line = nl ? nl + 1 : NULL;
            }

            if (!has_cid || !gfx) continue;

            uint32_t idx = (uint32_t)(cid * 2654435761u) % MAX_CLIENTS;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                uint32_t s = (idx + (uint32_t)i) % MAX_CLIENTS;
                if (keys[s] == 0 && vals[s] == 0) {
                    keys[s] = cid;
                    vals[s] = gfx;
                    break;
                }
                if (keys[s] == cid) {
                    if (gfx > vals[s]) vals[s] = gfx;
                    break;
                }
            }
        }
        closedir(fdi);
    }
    closedir(proc);

    uint64_t total = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        total += vals[i];
    return total;
}

static void usage(const char *name)
{
    fprintf(stderr,
        "Usage: %s [-c card] [-d] [-h] [-v]\n"
        "  -c, --card NAME   DRM card (default: %s)\n"
        "  -d, --dry-run     Just print usage, don't patch\n"
        "  -h, --help\n"
        "  -v, --version\n",
        name, DEFAULT_CARD);
}

int main(int argc, char *argv[])
{
    const char *card = DEFAULT_CARD;
    int dry_run = 0;

    static struct option opts[] = {
        {"card",    required_argument, NULL, 'c'},
        {"dry-run", no_argument,       NULL, 'd'},
        {"help",    no_argument,       NULL, 'h'},
        {"version", no_argument,       NULL, 'v'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:dhv", opts, NULL)) != -1) {
        switch (c) {
        case 'c': card = optarg; break;
        case 'd': dry_run = 1;  break;
        case 'v': puts("gpu-metrics-fix 1.0.0"); return 0;
        default:  usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }

    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/class/drm/%s/device/gpu_metrics", card);
    snprintf(patched_path, sizeof(patched_path),
             "%s/%s", PATCHED_DIR, PATCHED_FILE);

    if (dry_run) {
        fprintf(stderr, "dry-run, reading %s\n", sysfs_path);
        uint64_t prev = get_gfx_time();
        while (running) {
            usleep(INTERVAL_US);
            uint64_t cur = get_gfx_time();
            unsigned pct = (unsigned)((cur - prev) / 10000000);
            if (pct > 100) pct = 100;
            printf("GPU: %u%%\n", pct);
            prev = cur;
        }
        return 0;
    }

    int real_fd = open(sysfs_path, O_RDONLY);
    if (real_fd < 0) {
        fprintf(stderr, "can't open %s: %s\n", sysfs_path, strerror(errno));
        return 1;
    }

    mkdir(PATCHED_DIR, 0755);
    int patch_fd = open(patched_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (patch_fd < 0) {
        perror("can't create patched file");
        close(real_fd);
        return 1;
    }
    uint8_t zeros[METRICS_SIZE] = {0};
    write(patch_fd, zeros, METRICS_SIZE);

    if (mount(patched_path, sysfs_path, NULL, MS_BIND, NULL)) {
        fprintf(stderr, "bind mount failed: %s\n", strerror(errno));
        close(patch_fd);
        close(real_fd);
        return 1;
    }
    fprintf(stderr, "gpu-metrics-fix: active on %s (pid %d)\n", sysfs_path, getpid());

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    uint64_t prev = get_gfx_time();
    while (running) {
        usleep(INTERVAL_US);

        uint64_t cur = get_gfx_time();
        unsigned pct = (unsigned)((cur - prev) / 10000000);
        if (pct > 100) pct = 100;
        prev = cur;

        uint8_t raw[METRICS_SIZE];
        lseek(real_fd, 0, SEEK_SET);
        ssize_t n = read(real_fd, raw, METRICS_SIZE);
        if (n < 30) continue;

        raw[USAGE_OFFSET]     = (uint8_t)(pct & 0xFF);
        raw[USAGE_OFFSET + 1] = (uint8_t)((pct >> 8) & 0xFF);

        lseek(patch_fd, 0, SEEK_SET);
        write(patch_fd, raw, (size_t)n);
    }

    fprintf(stderr, "gpu-metrics-fix: shutting down\n");
    umount2(sysfs_path, MNT_DETACH);
    close(patch_fd);
    close(real_fd);
    unlink(patched_path);

    return 0;
}