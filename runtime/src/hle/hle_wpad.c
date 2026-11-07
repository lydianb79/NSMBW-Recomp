/*
 * hle_wpad.c - Wii Remote layer.
 *
 * The Revolution SDK talks to Wii Remotes through a full Bluetooth stack
 * (WPAD -> WUD -> BTA/BTE -> HCI over /dev/usb/oh1). There is no radio here,
 * so instead of emulating HCI we
 *   - report the Bluetooth stack as ready so WUD/WPAD initialisation finishes;
 *   - replace KPADiRead (behind KPADRead/KPADReadEx) and fill KPADStatus
 *     samples from the host input (keyboard / game controllers).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../rt_internal.h"
#include "addrs_smnp01.h"

/* bool WUDiIsBtReady(void): BTA state == enabled. */
PPC_HLE(HLE_WUDIsBtReady) {
    (void)base;
    rt_stats.hle_hits++;
    ctx->r[3] = 1;
}

/* WPAD core button bits */
enum {
    WPAD_LEFT = 0x0001, WPAD_RIGHT = 0x0002, WPAD_DOWN = 0x0004, WPAD_UP = 0x0008,
    WPAD_PLUS = 0x0010, WPAD_2 = 0x0100, WPAD_1 = 0x0200, WPAD_B = 0x0400, WPAD_A = 0x0800,
    WPAD_MINUS = 0x1000, WPAD_Z = 0x2000, WPAD_C = 0x4000, WPAD_HOME = 0x8000,
};

/* KPADStatus (SDK 3.x, 0x84 bytes) */
enum {
    KS_HOLD = 0x00, KS_TRIG = 0x04, KS_RELEASE = 0x08, KS_ACC = 0x0C, KS_ACC_VALUE = 0x18,
    KS_ACC_SPEED = 0x1C, KS_POS = 0x20, KS_HORIZON = 0x34, KS_DIST = 0x48,
    KS_DEV_TYPE = 0x5C, KS_WPAD_ERR = 0x5D, KS_DPD_VALID = 0x5E, KS_DATA_FORMAT = 0x5F,
    KS_EX_STICK = 0x60, KS_SIZE = 0x84,
};

static uint32_t g_prev_hold[4];
static float g_prev_acc_mag[4];
static int g_shake_phase[4];

/* Map the host's logical buttons onto a sideways-held Wii Remote:
 * screen-left is the remote's d-pad UP, screen-right is DOWN, etc. */
static uint32_t wpad_buttons(uint32_t held) {
    uint32_t b = 0;
    if (held & (1u << RT_BTN_LEFT))  b |= WPAD_UP;
    if (held & (1u << RT_BTN_RIGHT)) b |= WPAD_DOWN;
    if (held & (1u << RT_BTN_UP))    b |= WPAD_RIGHT;
    if (held & (1u << RT_BTN_DOWN))  b |= WPAD_LEFT;
    if (held & (1u << RT_BTN_A))     b |= WPAD_A;
    if (held & (1u << RT_BTN_B))     b |= WPAD_B;
    if (held & (1u << RT_BTN_ONE))   b |= WPAD_1;
    if (held & (1u << RT_BTN_TWO))   b |= WPAD_2;
    if (held & (1u << RT_BTN_PLUS))  b |= WPAD_PLUS;
    if (held & (1u << RT_BTN_MINUS)) b |= WPAD_MINUS;
    if (held & (1u << RT_BTN_HOME))  b |= WPAD_HOME;
    if (held & (1u << RT_BTN_NUNCHUK_C)) b |= WPAD_C;
    if (held & (1u << RT_BTN_NUNCHUK_Z)) b |= WPAD_Z;
    return b;
}

/* s32 KPADiRead(s32 chan, KPADStatus *buf, u32 length, s32 *err, BOOL ex) */
PPC_HLE(HLE_KPADiRead) {
    int chan = (int)ctx->r[3];
    uint32_t buf = ctx->r[4], length = ctx->r[5], errp = ctx->r[6];
    rt_stats.hle_hits++;
    if (chan < 0 || chan > 3 || !buf || !length) { ctx->r[3] = 0; return; }

    RtPlayerInput in;
    rt_input_get(chan, &in);
    if (!in.connected) {
        if (errp) st32(base, errp, (uint32_t)-1);          /* WPAD_ERR_NO_CONTROLLER */
        g_prev_hold[chan] = 0;
        ctx->r[3] = 0;
        return;
    }

    /* The SDK's WPAD layer never sees a Bluetooth connection (there is no BT
     * stack underneath), so its per-channel control block still says "no
     * controller". Game code that asks WPAD directly (WPADProbe from the remote
     * manager, extension checks) would wait forever, so present a connected
     * core remote: status (+0x8BC) = WPAD_ERR_NONE, device type (+0x8C1) =
     * WPAD_DEV_CORE, and the "ready for commands" word (+0x8DC) non-zero. */
    if (in.connected) {
        uint32_t cb = ld32(base, WPAD_CB_TABLE + 4u * (uint32_t)chan);
        if (cb >= 0x80000000u && cb < 0x81800000u) {
            if ((int32_t)ld32(base, cb + 0x8BC) != 0 || ld8(base, cb + 0x8C1) != 0 || ld32(base, cb + 0x8DC) == 0) {
                if (getenv("RT_WPAD_TRACE")) rt_log(0, "wpad: channel %d presented as a connected remote (cb %08X)", chan, cb);
                st32(base, cb + 0x8BC, 0);
                st8(base, cb + 0x8C1, 0);
                if (ld32(base, cb + 0x8DC) == 0) st32(base, cb + 0x8DC, 1);
            }
        }
    }
    uint32_t hold = wpad_buttons(in.held);
    uint32_t trig = hold & ~g_prev_hold[chan], rel = g_prev_hold[chan] & ~hold;
    g_prev_hold[chan] = hold;

    memset(ppc_host(base, buf), 0, KS_SIZE);
    st32(base, buf + KS_HOLD, hold);
    st32(base, buf + KS_TRIG, trig);
    st32(base, buf + KS_RELEASE, rel);

    /* Accelerometer (in g). Sideways and level: gravity along the remote's X
     * axis. Tilt rolls it; "shake" alternates a strong spike each frame. */
    float tilt = fmaxf(-1.0f, fminf(1.0f, in.tilt));
    float ang = tilt * 0.6f;
    float ax = -cosf(ang), ay = sinf(ang), az = 0.0f;
    if (in.held & (1u << RT_BTN_SHAKE)) {
        g_shake_phase[chan] ^= 1;
        float k = g_shake_phase[chan] ? 2.5f : -2.5f;
        ax += k; ay += k * 0.5f; az += k;
    }
    st32f(base, buf + KS_ACC + 0, ax);
    st32f(base, buf + KS_ACC + 4, ay);
    st32f(base, buf + KS_ACC + 8, az);
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    st32f(base, buf + KS_ACC_VALUE, mag);
    st32f(base, buf + KS_ACC_SPEED, mag - g_prev_acc_mag[chan]);
    g_prev_acc_mag[chan] = mag;

    st32f(base, buf + KS_HORIZON + 0, 1.0f);              /* level horizon */
    st32f(base, buf + KS_DIST, 1.0f);
    st8(base, buf + KS_DEV_TYPE, 0);                     /* WPAD_DEV_CORE */
    st8(base, buf + KS_WPAD_ERR, 0);                     /* WPAD_ERR_NONE */
    st8(base, buf + KS_DPD_VALID, 0);
    st8(base, buf + KS_DATA_FORMAT, 0);
    st32f(base, buf + KS_EX_STICK + 0, in.stick_x);
    st32f(base, buf + KS_EX_STICK + 4, in.stick_y);

    if (errp) st32(base, errp, 0);
    ctx->r[3] = 1;                                       /* one sample */
}

/* s32 WPADControlLed(s32 chan, u8 pattern, WPADCallback cb)
 * There is no Bluetooth link, so the remote "acknowledges" immediately. The
 * game sets the player LEDs when it assigns remotes to players and waits for
 * this callback before moving on (e.g. after choosing a save file). */
PPC_HLE(HLE_WPADControlLed) {
    int chan = (int)ctx->r[3];
    uint32_t cb = ctx->r[5];
    rt_stats.hle_hits++;
    RtPlayerInput in;
    rt_input_get(chan & 3, &in);
    int32_t result = in.connected ? 0 : -1;               /* WPAD_ERR_NONE / NO_CONTROLLER */
    if (getenv("RT_WPAD_TRACE")) rt_log(0, "wpad: ControlLed(chan %d, pattern %X, cb %08X) -> %d", chan, ctx->r[4] & 0xFF, cb, result);
    if (cb) {
        uint32_t lr = ctx->lr, r1 = ctx->r[1];
        ctx->r[3] = (uint32_t)chan;
        ctx->r[4] = (uint32_t)result;
        ppc_call(ctx, base, cb);
        ctx->lr = lr; ctx->r[1] = r1;
    }
    ctx->r[3] = (uint32_t)result;
}

/* ------------------------------------------------------------ remote speaker */
/* The speaker is reached over Bluetooth; there is none, so the commands
 * "succeed" at once and the stream data is accepted (and dropped - the sound
 * itself is mixed from the AX remote bus in hle_ax.c). Without this, sounds
 * meant for the remote never start or finish. */
static int g_speaker_on[4];

static void call_wpad_cb(PPCContext *ctx, uint8_t *base, uint32_t cb, int chan, int32_t result) {
    if (!cb) return;
    uint32_t lr = ctx->lr, r1 = ctx->r[1];
    ctx->r[3] = (uint32_t)chan;
    ctx->r[4] = (uint32_t)result;
    ppc_call(ctx, base, cb);
    ctx->lr = lr; ctx->r[1] = r1;
}

PPC_HLE(HLE_WPADControlSpeaker) {          /* s32 (s32 chan, u32 command, WPADCallback cb) */
    int chan = (int)(ctx->r[3] & 3);
    uint32_t cmd = ctx->r[4], cb = ctx->r[5];
    RtPlayerInput in; rt_input_get(chan, &in);
    int32_t res = in.connected ? 0 : -1;
    if (in.connected) g_speaker_on[chan] = cmd != 0;           /* WPAD_SPEAKER_OFF = 0 */
    if (getenv("RT_WPAD_TRACE")) rt_log(0, "wpad: ControlSpeaker(chan %d, cmd %u) -> %d", chan, cmd, res);
    call_wpad_cb(ctx, base, cb, chan, res);
    ctx->r[3] = (uint32_t)res;
}

PPC_HLE(HLE_WPADIsSpeakerEnabled) {
    ctx->r[3] = (uint32_t)g_speaker_on[ctx->r[3] & 3];
}

PPC_HLE(HLE_WPADCanSendStreamData) {
    RtPlayerInput in; rt_input_get((int)(ctx->r[3] & 3), &in);
    ctx->r[3] = in.connected ? 1u : 0u;
}

PPC_HLE(HLE_WPADSendStreamData) {          /* s32 (s32 chan, void *data, u16 len) */
    RtPlayerInput in; rt_input_get((int)(ctx->r[3] & 3), &in);
    ctx->r[3] = in.connected ? 0u : (uint32_t)-1;
}
