/*
 * addrs_smnp01.h - guest function addresses for New Super Mario Bros. Wii,
 * PAL (SMNP01), main.dol sha1 a5c5821adea919638fe431e79ba6bd63120431db.
 *
 * Found with tools/xref.py (string cross references) and by inspecting the
 * surrounding SDK code; re-verify if your main.dol hash differs (the tools
 * print it during extraction). Each define names the generated symbol that an
 * HLE implementation overrides.
 */
#pragma once

#define HLE_OSSaveContext        f_801ACEF0   /* stmw r13 into OSContext, returns 0 */
#define HLE_OSLoadContext        f_801ACF70   /* reloads OSContext, ends in rfi */
#define HLE_OSLink_Link          f_801B1D70   /* static Link(module, bss, fixed) */
#define HLE_OSLink               f_801B2060   /* li r5,0; b Link */
#define HLE_OSLinkFixed          f_801B2070
#define HLE_OSUnlink             f_801B2090   /* "OSUnlink: unknown relocation type" */
#define HLE_DSPAddTask           f_801D5950   /* queues a DSPTaskInfo, boots the DSP */
#define HLE_KPADiRead            f_801ED500   /* (chan, buf, len, *err, ex): behind KPADRead (801ED4E0) / KPADReadEx (801ED4F0) */
#define HLE_WUDIsBtReady         f_801FD070   /* returns BTA state == 5, polled by WUD init */

/* informational - candidates for further HLE work */
#define ADDR_DVDConvertEntrynumToPath 0x801CA7C0
#define ADDR_DVDReadAsync             0x801CAB70
#define ADDR_DVDRead                  0x801CAC60
#define ADDR_DVDLowInit               0x801D1370
#define ADDR_WPADInit                 0x801DF930
#define ADDR_WPADSetSamplingCallback  0x801E1110
#define ADDR_WPADSetConnectCallback   0x801E11A0

#define OS_MODULE_LIST_HEAD    0x800030C8
#define OS_MODULE_LIST_TAIL    0x800030CC
#define OS_CURRENT_CONTEXT     0x800000D4
#define OS_EXCEPTION_TABLE     0x80003000
#define OS_EXCEPTION_EXTERNAL  4

/* WPAD per-channel control block pointers (_wpdcb[4]) */
#define WPAD_CB_TABLE 0x8039E630u

/* AX output (audio frame pump). __AXOutAiCallback runs on every AI DMA
 * interrupt; with no DSP it would wait forever for the microcode to finish
 * the previous frame, so the HLE marks the DSP ready before each call. */
#define HLE_AXOutAiCallback      f_801A1CE0
#define AX_OUT_DSP_STATE_SDA     (-0x50F8)     /* r13-relative: 1 = DSP idle, ready for a new frame */

/* WPADControlLed(chan, pattern, callback): queues output report 0x11 for the
 * Bluetooth stack; the callback (chan, result) fires when the remote acks. */
#define HLE_WPADControlLed       f_801E1770
#define AX_DSP_PB_ARRAY_SDA      (-0x5098)     /* r13-relative: pointer to the DSP-side AXPB array (0x140 each) */
#define AX_NUM_VOICES_SDA        (-0x508C)     /* r13-relative: number of voices */
/* SndAudioMgr: "is a sound with this ID active" (iterates the sound player) */
#define HLE_SndIsSoundActive     f_80196740

/* OS scheduler: SelectThread (wrapped for idle-time thread switches) */
#define HLE_SelectThread         f_801B4FE0

/* AX aux buses (reverb etc.): callbacks + contexts (r13-relative), DPL2 mode flag */
#define AX_AUXA_CB_SDA           (-0x5160)
#define AX_AUXB_CB_SDA           (-0x5164)
#define AX_AUXC_CB_SDA           (-0x5168)
#define AX_AUXA_CTX_SDA          (-0x516C)
#define AX_AUXB_CTX_SDA          (-0x5170)
#define AX_AUXC_CTX_SDA          (-0x5174)
#define AX_OUTPUT_MODE_SDA       (-0x5138)     /* 2 = DPL2 (4 aux channels) */
#define AX_AUX_SCRATCH           0x80379020u   /* AX's own aux buffer area (unused without a DSP) */

/* Wii Remote speaker (WPAD) */
#define HLE_WPADControlSpeaker   f_801E1F60
/* void WPADControlMotor(s32 chan, u32 command): rumble on (1) / off (0) */
#define HLE_WPADControlMotor     f_801E1620
#define WPAD_CB_MOTOR            0x8D4   /* control block: motor running */
#define HLE_WPADIsSpeakerEnabled f_801E1F10
#define HLE_WPADCanSendStreamData f_801E3730
#define HLE_WPADSendStreamData   f_801E37C0
