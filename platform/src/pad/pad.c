/*
 * pad.c - GameCube controller input from the 3DS buttons.
 *
 * Port 1 is the 3DS; ports 2-4 report no controller. Mapping:
 *
 *   3DS                     GameCube
 *   circle pad              main stick
 *   C-stick (New 3DS)       C stick
 *   D-pad                   D-pad
 *   A B X Y                 A B X Y
 *   L R                     L R (pressed fully: digital bit and analog 255)
 *   ZL or ZR                Z
 *   START                   START
 *
 * The SDK samples the controllers from SI interrupts and calls the sampling
 * callback; here the platform samples once per frame from VIWaitForRetrace
 * (__port_pad_sample).
 */
#include <3ds.h>
#include <string.h>

typedef struct {
    u16 button;
    s8 stickX, stickY;
    s8 substickX, substickY;
    u8 triggerL, triggerR;
    u8 analogA, analogB;
    s8 err;
} PADStatus;                                   /* 12 bytes, as in the SDK */

enum {
    PAD_BUTTON_LEFT = 0x0001, PAD_BUTTON_RIGHT = 0x0002, PAD_BUTTON_DOWN = 0x0004, PAD_BUTTON_UP = 0x0008,
    PAD_TRIGGER_Z = 0x0010, PAD_TRIGGER_R = 0x0020, PAD_TRIGGER_L = 0x0040,
    PAD_BUTTON_A = 0x0100, PAD_BUTTON_B = 0x0200, PAD_BUTTON_X = 0x0400, PAD_BUTTON_Y = 0x0800,
    PAD_BUTTON_START = 0x1000,
};
enum { PAD_ERR_NONE = 0, PAD_ERR_NO_CONTROLLER = -1 };

typedef void (*PADSamplingCallback)(void);

static PADSamplingCallback sampling_cb;
static PADStatus current;
static u32 analog_mode;

/* The circle pad reads about +-155; GameCube sticks reach about +-100. */
static s8 scale_stick(int v)
{
    v = v * 100 / 155;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (s8)v;
}

void __port_pad_sample(void)
{
    hidScanInput();
    u32 held = hidKeysHeld();
    circlePosition cp, cs;
    hidCircleRead(&cp);
    irrstCstickRead(&cs);

    PADStatus s;
    memset(&s, 0, sizeof(s));
    if (held & KEY_DLEFT) s.button |= PAD_BUTTON_LEFT;
    if (held & KEY_DRIGHT) s.button |= PAD_BUTTON_RIGHT;
    if (held & KEY_DDOWN) s.button |= PAD_BUTTON_DOWN;
    if (held & KEY_DUP) s.button |= PAD_BUTTON_UP;
    if (held & (KEY_ZL | KEY_ZR)) s.button |= PAD_TRIGGER_Z;
    if (held & KEY_R) { s.button |= PAD_TRIGGER_R; s.triggerR = 255; }
    if (held & KEY_L) { s.button |= PAD_TRIGGER_L; s.triggerL = 255; }
    if (held & KEY_A) { s.button |= PAD_BUTTON_A; s.analogA = 255; }
    if (held & KEY_B) { s.button |= PAD_BUTTON_B; s.analogB = 255; }
    if (held & KEY_X) s.button |= PAD_BUTTON_X;
    if (held & KEY_Y) s.button |= PAD_BUTTON_Y;
    if (held & KEY_START) s.button |= PAD_BUTTON_START;
    s.stickX = scale_stick(cp.dx);
    s.stickY = scale_stick(cp.dy);
    s.substickX = scale_stick(cs.dx);
    s.substickY = scale_stick(cs.dy);
    s.err = PAD_ERR_NONE;
    current = s;

    if (sampling_cb)
        sampling_cb();
}

int PADInit(void)
{
    irrstInit();
    return 1;
}

u32 PADRead(PADStatus* status)
{
    status[0] = current;
    for (int i = 1; i < 4; i++) {
        memset(&status[i], 0, sizeof(PADStatus));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0;
}

int PADRecalibrate(u32 mask) { (void)mask; return 1; }
int PADReset(u32 mask) { (void)mask; return 1; }
void PADSetAnalogMode(u32 mode) { analog_mode = mode; }
void PADSetSpec(u32 spec) { (void)spec; }
void PADControlMotor(s32 chan, u32 command) { (void)chan; (void)command; }
int PADSync(void) { return 1; }

/* SDK functions the decomp has not named; see the decomp's
 * src/game/input/input.c and src/dolphin/pad/PAD.c. */
void fn_800AB4FC(const u32* commands) { (void)commands; }    /* PADControlAllMotors: no rumble */
int fn_800AAD34(u32 mask) { return PADReset(mask); }         /* PADReset */
int fn_800ABD68(int final) { (void)final; return 1; }        /* PAD's OSReset handler */

PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback cb)
{
    PADSamplingCallback old = sampling_cb;
    sampling_cb = cb;
    return old;
}

/* Some games convert raw SI data themselves; the SDK exports this helper. */
void SPEC2_MakeStatus(s32 chan, PADStatus* status, u32 data[2])
{
    (void)chan; (void)data;
    *status = current;
}
