# NSMBW Recomp — static recompilation of *New Super Mario Bros. Wii* for macOS

This project turns **your own** NSMBW disc image into a native macOS application.
It translates the game's PowerPC machine code (main.dol plus the four REL modules) into C,
compiles that C with clang for Apple Silicon or Intel, and runs it on a runtime that stands in
for the Wii's hardware and operating system.

No game code or assets are included or distributed. Everything game-specific is generated
locally from the disc you supply.

> **Status: playable.** The recompiled game boots, plays the title and opening cutscene, reaches
> the world map and loads levels. Rendering runs on Metal (TEV uber-shader, indirect texturing,
> GPU-side EFB copies), audio is mixed from the game's AX voices (ADPCM/PCM, sample-rate
> conversion, envelopes, the game's own reverb on the aux buses, the Wii Remote speaker stream) and
> played through Core Audio, and save data persists in a virtual NAND.

## What's here

| Path | What it does |
|---|---|
| `tools/rvz.py` | Reads Dolphin **RVZ/WIA** images (zstd, LZMA, LZMA2, bzip2, RVZ junk packing). Rebuilds the decrypted Wii partition and extracts `sys/` (boot.bin, bi2, apploader, main.dol, fst.bin) and `files/`. No Wii keys needed. |
| `tools/nlz.py` | Nintendo LZ77 / LZ11 decompressor for the `rels/*.rel.LZ` modules. |
| `tools/image.py` | DOL + REL **static linker**. Places each module at a fixed address in an arena just above MEM1 (0x81800000, so every `bl` stays within ±32 MiB), applies all ~198k relocations and writes `image.bin` / `image.json`. |
| `tools/ppc.py` | Gekko/Broadway decoder: integer, FPU, paired singles, quantised loads/stores, branches, CR and SPR instructions. |
| `tools/recomp.py` | The **recompiler**. Finds functions without symbols (bl targets, vtables and callbacks in data, `lis`/`addi` address constants, fixed-point splitting) and recognises MWCC `switch` jump tables. Emits C: `goto` inside functions, guaranteed tail calls (`musttail`) between them, a dispatch table for indirect calls, interrupt polls on loop back-edges, and weak symbols so HLE code can replace any function. |
| `tools/xref.py` | String cross-references. This is how SDK functions were located in the symbol-less retail binary (OSLink, DVD, WPAD, …). |
| `runtime/` | C runtime: guest memory (1 GiB reservation with mirrors), MMIO for VI/PI/DSP/EXI/SI/Hollywood, **IOS IPC HLE** (`/dev/di` from the extracted files, NAND in a host folder, generated SYSCONF, `/dev/es`, `/dev/stm`), GX FIFO command decoder (CP/XF/BP state, vertex sizes from VCD/VAT, EFB→XFB copies), XFB→RGBA video output, settings and input state. |
| `runtime/src/hle/` | High-level replacements: `OSLink`/`OSUnlink` onto the pre-linked modules; `OSSaveContext`/`OSLoadContext` as **host-thread-per-guest-thread** with a run baton; interrupt delivery through the guest's own handler, returning via `longjmp`. |
| `platform/macos/` | Native **Cocoa + Metal** app: window and full screen, Metal presenter, keyboard + GameController input, Settings window. |
| `scripts/build.sh` | One command from RVZ to `.app`. |

## Build (macOS 11+)

```sh
xcode-select --install          # clang
brew install cmake zstd ninja   # ninja optional
bash scripts/build.sh "/path/to/New Super Mario Bros Wii (Europe).rvz"
open "dist/NSMBW Recomp.app"
```

The script extracts the disc to `game/`, links and recompiles into `build/`, compiles everything
(a few minutes on Apple Silicon; the generated C is about 80 MB) and copies the app to `dist/`.
Re-run with `--skip-extract` to reuse `game/`.

After the first build, double-click **`build_and_log.command`** in Finder to rebuild. It reuses
the generated C in `build/`, rebuilds the app, copies it to `dist/`, refreshes the app icon in
Finder and the Dock, writes `build_log.txt`, and runs a 30-second headless check
(`headless_log.txt`). If you move the project folder, CMake's cache no longer matches the new
location; the script notices, sets the old `build/cmake` aside as `build/cmake.old.<time>` and
starts a fresh (full) compile. You can delete the old folder afterwards.

If the Dock or Finder still shows an old icon after a rebuild, run `killall Dock Finder`.

A headless runner is built alongside the app, which is handy for working on the runtime:

```sh
build/cmake/nsmbw-headless build game -v -t 20 -o frame.ppm
```

Use `-vv` to trace every IPC request, context switch and interrupt. Hangs and crashes dump each
guest thread's registers and call stack (the guest back-chain). Because guest calls are host C
calls, `gdb`/`lldb` backtraces show the guest call chain directly as `f_XXXXXXXX` frames.

## Settings

**Graphics** (Settings → Graphics, or the View menu)

- Output filter: Nearest, Bilinear, Sharp bilinear (crisp pixels at non-integer scales), Scale2x (EPX), CRT (scanlines and aperture mask, adjustable strength), LCD grid
- Scaling: Fit (keep aspect), Integer, Stretch
- Aspect ratio: Auto (follows the game), 4:3, 16:9. **Widescreen 16:9** (default on) tells the game, through SYSCONF, to render in 16:9; turn it off for 4:3. Takes effect at the next start.
- Renderer: Metal (default) or the CPU software renderer
- Internal resolution 1×–8× (default 2×): the Metal renderer draws the game at N × 640×528 and presents that image directly. Render-to-texture effects are made at the same resolution. Scale2x, CRT and LCD filters work on native pixels, so they show the 1× image.
- Anti-aliasing: 2× / 4× MSAA. Anisotropic filtering 2×–16× (adds mipmaps to the game's textures). Texture filtering override (as the game sets it / nearest / linear).
- V-Sync, frame-rate limit (60 / 50 / 120 / unlimited), FPS counter, full screen (⌃⌘F)
- App icon: pick one of ten bundled designs; the Dock icon changes immediately

**Audio**: master volume (default 80 %).

**Input** (Settings → Input)

- Four players. Each gets a keyboard map and a game controller (Xbox, PlayStation, Switch Pro, MFi via Apple's GameController framework).
- Remap any Wii Remote/Nunchuk function. Click a field, then press a key or button; Delete clears it.
- Shake (spin jump), tilt left/right, optional left stick as D-pad, adjustable dead zone.
- Default keyboard: arrows = D-pad, **X = 2 (jump)**, **Z = 1 (run/fire)**, Space = shake, Return = +, Tab = −, Q/E = tilt.
- Rumble: when the game runs a remote's motor, the controller assigned to that player rumbles (controllers with haptics; can be turned off).

Settings are saved to `~/Library/Application Support/NSMBWRecomp/settings.ini`. The virtual
NAND (saves, SYSCONF) is in the `nand/` folder next to it.

## How it fits together

- **Recompiler** (`tools/`): RVZ extraction, REL linking, function discovery (call targets, data
  and code pointers, jump tables), PowerPC → C with interrupt checks on loop back-edges.
- **Runtime** (`runtime/`): memory map and MMIO (VI, PI, DSP/AI, PE, CP), IOS over IPC (`/dev/di`
  on the extracted disc, `/dev/fs` on a host folder, ES, STM), guest threads on host threads with
  the game's own scheduler, interrupts delivered through `__OSDispatchInterrupt`.
- **HLE** (`runtime/src/hle/`): OS context switching, WPAD/KPAD input (remotes reported as
  connected, speaker stream decoded from its Yamaha ADPCM, motor state passed on as rumble), DSP
  task start-up, and AX: the DSP's per-frame work (voice decoding and mixing, aux-bus hand-off to
  the game's effects) done in C.
- **Graphics**: a GX FIFO decoder feeds a front end that does vertex transform, lighting and
  texgen on the CPU; triangles go to Metal (`platform/macos/MetalGX.m`: one shader generated and
  compiled in the background per TEV setup, with a general "uber" shader drawing until it is ready;
  scaled EFB, MSAA, GPU-side
  EFB copies, full-resolution display copies handed to the presenter) or the portable software
  rasteriser (`runtime/src/gx_soft.c`, used by the headless runner).
- **Timing**: the VI clock (`runtime/src/rt_main.c`) locks to the display refresh reported by the
  app's display link; the AI clock paces audio DMA every 3 ms.

## App icons

`platform/macos/icons/` holds ten original icon designs (`AppIcon-<name>.icns`, plus PNG
previews and `variants_sheet.png` / `designs_sheet.png`): day, sunset, night, winter, autumn,
arcade_chip, **pixel_leap** (default), neon, paper, minimal. All of them are bundled into the
app. The one Finder shows for the `.app` file is set at configure time:

```sh
cmake -S . -B build/cmake -DNSMBW_ICON=neon
```

The Dock icon while the game runs follows Settings → Graphics → App icon. The icons are drawn by
`tools/make_icon_variants.py` and `tools/make_icon_designs.py` (Python + Pillow).

## Debugging

`build/cmake/nsmbw-headless build game -v -t <seconds>` runs the game without a window and prints
per-second status. Useful environment variables: `RT_PRESS="a@6,two@9,right@12-15"` (scripted
input), `RT_WAV=out.wav` (record the audio mix), `RT_SOFTWARE_RENDER` (app: use the CPU renderer),
`RT_CPU_EFB_COPIES`, `RT_XFB_READBACK` (app: write each frame to the guest XFB through a CPU readback, as before),
`RT_UBERSHADER_ONLY` (app: draw everything with the general TEV shader, no specialised shaders), `RT_NO_REVERB`, `RT_NO_REMOTE_SPEAKER`, `RT_NO_AUDIO`, `RT_INTERNAL_RES=<1-8>` (overrides the
setting), `RT_NO_VSYNC_LOCK` (free-running 60 Hz timer), `RT_SND_SAFETY` (report a sound finished after the game
has waited 3 s for it; a debugging aid for audio bugs). A hang prints every
guest thread's call stack.

## Known gaps / roadmap

1. Rumble and the decoded remote-speaker stream are written from the hardware specs and have not
   yet been checked against a real play session; the speaker falls back to the AX remote bus
   whenever no stream is coming in.
2. Internal resolution: XFB copies with vertical scaling are presented unscaled (NSMBW does not
   use them).
3. Only the Metal renderer has the enhancements; the CPU renderer stays at 1×.

### Recently fixed

- One-shot voices now end the way AXWii ends them: the voice stops and its play position jumps to
  the loop address. nw4r::snd points that at a silent buffer and checks for it to decide that a
  wave has finished. This was why some sounds (the "player decide" voice after choosing players)
  never finished, and why a 3-second safety net was needed. That net is now off.
- Frame pacing: on 60/120/180/240 Hz displays the game's VI clock follows the display's own
  refresh, half a refresh after each vsync. Every game frame is then on screen for exactly one
  refresh, with no doubled or skipped frames from two clocks drifting apart.

## How the recompiled code looks

```c
/* 80004050..800041C0 */
PPC_FUNC_IMPL(f_80004050) {
L_80004050:;
	ctx->lr = 0x80004054; f_800041C0(ctx, base);      /* bl __init_registers */
	ctx->lr = 0x80004058; f_80004300(ctx, base);      /* bl __init_hardware */
	ctx->r[0] = 0xFFFFFFFF;
	ea = ctx->r[1] + 0xFFFFFFF8; st32(base, ea, ctx->r[1]); ctx->r[1] = ea;
	...
```

## Legal

Only use this with a disc you own and dumped yourself (for example with CleanRip, then Dolphin
→ Convert to RVZ). Don't share generated `build/`, `game/` or app bundles that contain
`image.bin`; they contain the game's code.
