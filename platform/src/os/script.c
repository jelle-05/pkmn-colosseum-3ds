/*
 * script.c - scripted input and screenshots, for testing without a person.
 *
 * If sdmc:/colosseum-port-script.txt exists, it is read at the first frame.
 * Each line is an event at a presented frame:
 *
 *     300 shot                     save the frame as sdmc:/colosseum-shot-00300.bmp
 *     420 press START 10           hold START for 10 frames
 *     600 press A 5
 *     700 tnl 0                    draw on the CPU path only (1: the shader again)
 *
 * Buttons: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT (the 3DS's).
 * Held buttons are added to what the controller reads (pad/pad.c), so the
 * game sees them like real presses. Lines starting with # are ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"

extern void __port_log(const char* fmt, ...);

#define MAX_EVENTS 256

typedef struct {
    unsigned frame, length;
    unsigned keys;          /* 0: screenshot or setting */
    int tnl;                /* -1, or the transform shader off (0) or on (1) */
} Event;

static Event events[MAX_EVENTS];
static int n_events, loaded;
static unsigned held_until[16];

static const struct { const char* name; unsigned key; } buttons[] = {
    { "A", HOST_KEY_A }, { "B", HOST_KEY_B }, { "X", HOST_KEY_X }, { "Y", HOST_KEY_Y },
    { "L", HOST_KEY_L }, { "R", HOST_KEY_R }, { "ZL", HOST_KEY_ZL }, { "ZR", HOST_KEY_ZR },
    { "START", HOST_KEY_START }, { "SELECT", HOST_KEY_SELECT }, { "UP", HOST_KEY_DUP },
    { "DOWN", HOST_KEY_DDOWN }, { "LEFT", HOST_KEY_DLEFT }, { "RIGHT", HOST_KEY_DRIGHT },
};

static void load(void)
{
    loaded = 1;
    int fd = host_file_open("sdmc:/colosseum-port-script.txt", "rb");
    if (fd < 0)
        return;
    unsigned size = host_file_size(fd);
    char* text = host_malloc(size + 1);
    if (!text) {
        host_file_close(fd);
        return;
    }
    size = host_file_read(fd, text, size, 0);
    host_file_close(fd);
    text[size] = 0;
    for (char* line = strtok(text, "\r\n"); line && n_events < MAX_EVENTS; line = strtok(0, "\r\n")) {
        unsigned frame, length = 1;
        char what[16] = { 0 }, button[16] = { 0 };
        if (line[0] == '#' || sscanf(line, "%u %15s %15s %u", &frame, what, button, &length) < 2)
            continue;
        Event* e = &events[n_events];
        e->frame = frame;
        e->length = length;
        e->keys = 0;
        e->tnl = -1;
        if (!strcmp(what, "tnl")) {
            e->tnl = atoi(button) != 0;
        } else if (!strcmp(what, "press")) {
            for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
                if (!strcmp(button, buttons[i].name))
                    e->keys = buttons[i].key;
            if (!e->keys)
                continue;
        } else if (strcmp(what, "shot")) {
            continue;
        }
        n_events++;
    }
    host_free(text);
    __port_log("script: %d events", n_events);
}

static unsigned current_frame;

/* Called with each presented frame's number (gx/gx_frame.c). */
void __port_script_frame(unsigned frame)
{
    if (!loaded)
        load();
    current_frame = frame;
    for (int i = 0; i < n_events; i++) {
        const Event* e = &events[i];
        if (e->frame != frame)
            continue;
        if (e->tnl >= 0) {
            extern int __gx_tnl_enabled;
            __gx_tnl_enabled = e->tnl;
        } else if (!e->keys) {
            char path[64];
            snprintf(path, sizeof(path), "sdmc:/colosseum-shot-%05u.bmp", frame);
            host_dump_frame(path);
        } else {
            for (int b = 0; b < 16; b++)
                if (e->keys & (1u << b))
                    held_until[b] = frame + e->length;
        }
    }
}

unsigned __port_frame_number(void) { return current_frame; }
unsigned __port_frame_number_peek(void) { return current_frame; }

/* The buttons the script holds now (pad/pad.c adds them). */
unsigned __port_script_keys(void)
{
    unsigned keys = 0;
    for (int b = 0; b < 16; b++)
        if (held_until[b] > current_frame)
            keys |= 1u << b;
    return keys;
}
