#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

struct buffer {
    uint32_t handle, fb, pitch;
    uint64_t size;
    uint8_t *pixels;
};
static volatile sig_atomic_t stopping;
static void stop(int signal) { (void)signal; stopping = 1; }

static int make_buffer(int fd, struct buffer *b)
{
    struct drm_mode_create_dumb create = { .width = 240, .height = 240, .bpp = 32 };
    struct drm_mode_map_dumb map = {0};
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) return -1;
    b->handle = create.handle; b->pitch = create.pitch; b->size = create.size;
    handles[0] = b->handle; pitches[0] = b->pitch;
    if (drmModeAddFB2(fd, 240, 240, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b->fb, 0)) return -1;
    map.handle = b->handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) return -1;
    b->pixels = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    return b->pixels == MAP_FAILED ? -1 : 0;
}

static void destroy_buffer(int fd, struct buffer *b)
{
    if (b->pixels && b->pixels != MAP_FAILED) munmap(b->pixels, b->size);
    if (b->fb) drmModeRmFB(fd, b->fb);
    if (b->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = b->handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}

static void draw(struct buffer *b, unsigned frame)
{
    const uint32_t colours[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0xffff00, 0x00ffff };
    for (unsigned y = 0; y < 240; y++) {
        uint32_t *row = (uint32_t *)(b->pixels + y * b->pitch);
        for (unsigned x = 0; x < 240; x++) {
            uint32_t colour = ((x / 20 + y / 20 + frame) & 1) ? 0 : colours[(x / 40 + frame) % 6];
            if (x < 3 || y < 3 || x >= 237 || y >= 237) colour = 0xffffff;
            row[x] = colour;
        }
    }
}

static void flipped(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data)
{
    (void)fd; (void)sequence; (void)sec; (void)usec;
    *(bool *)data = true;
}

static int wait_flip(int fd, bool *done)
{
    struct timespec start, now;
    drmEventContext events = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = flipped };
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    while (!*done) {
        if (stopping) { errno = EINTR; return -1; }
        struct pollfd pollfd = { .fd = fd, .events = POLLIN };
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        long ms = 25000 - (now.tv_sec - start.tv_sec) * 1000 - (now.tv_nsec - start.tv_nsec) / 1000000;
        if (ms <= 0) { errno = ETIMEDOUT; return -1; }
        int result = poll(&pollfd, 1, (int)ms);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { if (!result) errno = ETIMEDOUT; return -1; }
        if ((pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            !(pollfd.revents & POLLIN)) { errno = EIO; return -1; }
        if (drmHandleEvent(fd, &events)) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction action = { .sa_handler = stop };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 1;
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
    unsigned count = 4;
    unsigned damage_width = 0, damage_height = 0;
    if (argc > 4) {
        fprintf(stderr, "usage: %s [DRM_DEVICE [FRAMES [WxH]]]\n", argv[0]);
        return 2;
    }
    if (argc >= 3) {
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (errno || *end || parsed < 1 || parsed > 1000) return 2;
        count = parsed;
    }
    /* With a rectangle, repeat DIRTYFB over that region instead of flipping
     * whole frames: this is the path a console or a partial redraw takes. */
    if (argc == 4) {
        char *end;
        errno = 0;
        unsigned long w = strtoul(argv[3], &end, 10);
        if (errno || *end != 'x' || w < 1 || w > 240) return 2;
        unsigned long h = strtoul(end + 1, &end, 10);
        if (errno || *end || h < 1 || h > 240) return 2;
        damage_width = w;
        damage_height = h;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(path); return 1; }
    drmVersionPtr version = drmGetVersion(fd);
    if (!version || !version->name || strcmp(version->name, "sg2002-c906l")) {
        fprintf(stderr, "refusing a non-C906L DRM device\n");
        if (version) drmFreeVersion(version);
        close(fd); return 1;
    }
    drmFreeVersion(version);
    drmModeRes *resources = drmModeGetResources(fd);
    drmModeConnector *connector = NULL;
    drmModeCrtc *saved = NULL;
    struct buffer buffers[2] = {0};
    int result = 1;
    bool modeset = false;
    if (!resources) goto out;
    if (resources->count_crtcs != 1 || resources->count_connectors != 1) {
        fprintf(stderr, "test requires exactly one CRTC and connector\n");
        errno = EINVAL; goto out;
    }
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *candidate = drmModeGetConnector(fd, resources->connectors[i]);
        if (candidate && candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes &&
            candidate->modes[0].hdisplay == 240 && candidate->modes[0].vdisplay == 240) {
            connector = candidate; break;
        }
        if (candidate) drmModeFreeConnector(candidate);
    }
    if (!connector) { errno = ENODEV; goto out; }
    uint32_t crtc = resources->crtcs[0];
    saved = drmModeGetCrtc(fd, crtc);
    if (!saved || make_buffer(fd, &buffers[0]) || make_buffer(fd, &buffers[1])) goto out;
    draw(&buffers[0], 0);
    if (drmModeSetCrtc(fd, crtc, buffers[0].fb, 0, 0, &connector->connector_id, 1, &connector->modes[0])) goto out;
    modeset = true;
    puts("DRM modeset completed: 240x240 XRGB8888 -> shared RGB565BE -> C906L SPI");
    for (unsigned frame = 1; frame < count; frame++) {
        if (stopping) { errno = EINTR; goto out; }
        if (damage_width) {
            drmModeClip clip = { .x1 = 0, .y1 = 0,
                                 .x2 = damage_width, .y2 = damage_height };
            draw(&buffers[0], frame);
            if (drmModeDirtyFB(fd, buffers[0].fb, &clip, 1)) goto out;
            printf("dirty_complete=%u\n", frame);
        } else {
            bool done = false;
            draw(&buffers[frame & 1], frame);
            if (drmModePageFlip(fd, crtc, buffers[frame & 1].fb, DRM_MODE_PAGE_FLIP_EVENT, &done) ||
                wait_flip(fd, &done)) goto out;
            printf("page_flip_complete=%u\n", frame);
        }
        fflush(stdout);
    }
    printf("scanout_completed frames=%u; holding last pattern for 15 seconds\n", count);
    fflush(stdout);
    for (unsigned i = 0; i < 15 && !stopping; i++) sleep(1);
    result = stopping ? 1 : 0;
out:
    if (result) perror("C906L DRM test");
    if (modeset && saved) {
        int restored;
        if (saved->mode_valid)
            restored = drmModeSetCrtc(fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                           &connector->connector_id, 1, &saved->mode);
        else restored = drmModeSetCrtc(fd, saved->crtc_id, 0, 0, 0, NULL, 0, NULL);
        if (restored) { perror("restore previous DRM mode"); result = 1; }
    }
    destroy_buffer(fd, &buffers[0]); destroy_buffer(fd, &buffers[1]);
    if (saved) drmModeFreeCrtc(saved);
    if (connector) drmModeFreeConnector(connector);
    if (resources) drmModeFreeResources(resources);
    close(fd);
    if (!result) printf("result=ok frames=%u display_restored=yes\n", count);
    return result;
}
