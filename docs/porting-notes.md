# Porting notes

The development log of this port, kept for contributors: what was broken, why, and where in the
game (EE/IOP addresses refer to the NTSC-U SLUS_204.62). Paths such as `t/drive.sh`, `~/wo` and
upload ids refer to the original build container and test harness, not to this repository.


State as of 2026-10-18 11:00 UTC. All fixes below are in the files in this folder (patches, config, vu1dumps, tarball). The build tree I worked in was in my session scratchpad and is NOT reachable from other threads; rebuild with `setup.sh`.

## Fixed in the current tarball / patches
- `config.toml`: removed the libm stubs (ceil, cos, floor, sin, tan, exp, log, log10, pow, sqrt, fabs, __ieee754_rem_pio2f, _sceVu0ecossin). The game's libm is soft-double (arg a0, result v0, 64-bit); the runtime stubs used the float ABI (f12->f0), so sin(x) returned x. That broke the sin table at 0x24d258 and caused the warping.
- `wipeout-runtime.patch` (on top of the Ruff patch) now also contains:
  - `ps2xRecomp/src/lib/control_flow_emitter.cpp`: BLTZ/BGEZ/BLEZ/BGTZ (and their likely/link forms) compare GPR_S64, not GPR_S32. The soft-float `_fpadd_parts` was wrong without this.
  - `vu_translator.cpp`: macro VCLIPw uses |w| and the correct bit order (+x is bit 0, -x is bit 1, and so on).
  - `code_generator.cpp` translateVUInstruction:
    - VU0 macro FMAC ops (vadd/vsub/vmul/vmadd/vmsub/vopmula/vopmsub) now set `ctx->vu0_mac_flags` via `ps2_vu0_macflags(res, dest)`. The game reads vi17 about 156 times.
    - Writes to vf0 are discarded.
  - `special_translator.cpp`: MOVZ/MOVN copy only the low 64 bits.
  - `ps2_runtime_macros.h`:
    - `ps2_vu0_macflags` helper.
    - CVT.W.S truncates and saturates (`ps2_fcvt_w`).
    - FPU compares use PS2 values.
  - `Stubs/VU.cpp` sceVu0CameraMatrix: normalize over xyz only, and w becomes 0 (as in Sony's sceVu0Normalize).
- `vurecomp-wipeout.patch`: the g_pendTarget fix carries a pending branch across code-block boundaries (this was the race-start hang).
- `vu1dumps/`: 26 dumps, including 9 new race code paths. `wipeout_override.cpp`: the RT_VU1MISS dump key is now a hash of the whole 16 KB micro memory, so overlay variants are saved.
- Result: on the GPU renderer the race scene draws correctly (HUD, track, 16/16).

## Open: race freezes at the countdown (clock stays 0:00.0)
Headless test runs on the software renderer, with race_pads timing; the race shows around 170–200 s.
- VU1 micro memory 0x2178..0x28d0 gets overwritten by vertex floats. This kills the matrix setup routine (L26c0) and then causes 1M+ interpreter fallbacks, with "reserved upper" instructions at pc 0x2258/0x2178.
- **Cause:** one VIF1 stream (about 9688 bytes, one contiguous RAM segment, phys about 0x010035f0 in my run) is already wrong in memory.
  - Every 160 bytes there is a vertex batch: STCYCL 0x0103, UNPACK V4-8 @2, STMASK, UNPACK V2-32 @3, STMASK, UNPACK V3-32 @1, MSCNT, STMASK, UNPACK V4-32 @0 n=1 (header 00008004 302e4000 ...).
  - At stream offset 6176 the header unpack holds `00008004 44e43580 44d42c50 c4612a38`, then floats follow where VIFcodes should be.
  - Those floats look like screen coordinates (~1700–1900).
  - The VIF parser then stalls on floats that have bit 31 set (treated as I-bit stalls), and later hits a "MPG dest=0x2178 n=236" made from data.
  - There were no VIF carries, so the bad bytes come from the EE side: either the packet builder or a buffer reused before its DMA was consumed. This still needs tracking down.
- A gdb watchpoint on that word was too noisy: the buffer is also written by memcpy during loading. Next idea: a watchpoint that arms only after the race has loaded, or log the EE function that builds these batches.

## Also still open
- The software renderer draws large flat olive ground triangles. The GPU renderer looks fine, so this is probably a software-rasteriser texture or large-triangle issue.
- Menu text overlap; the 3D ship and background are missing in menus.
- the tester asked for a graphics settings page inside the in-game Options menu, once races work.

## Debug env vars that helped
- RT_VU1MISS=dir: dumps VU1 code with no native version.
- RT_TRACE=addr:d with RT_TRACE_EXIT=1: prints 64-bit double args and results (this mode is new).
- RT_CAPTURE, RT_CAPTURE_AT, RT_CAPTURE_FRAMES: capture records; 0x10 is the VIF1 stream, 0x11 is PATH1.
- RT_SYNCLOG=1: VIF stall and resume log.

## Update 2026-10-18 23:55 UTC (second fixing thread)
- Race clock froze at 0:00.0 because config.toml stubbed `rand` with host rand() (15-bit); the weapon picker (0x11D348, called in a retry loop at 0x11E934) never got a different pick. Stub removed; the game's own LCG runs.
- `sceGsSetDefClear` (runtime stub was a no-op) and `sceGsPutDrawEnv` (stub applied 8 pairs, skipped the clear packet) are no longer stubbed; the game's own code builds the packet and DMAs it to GIF. Menu smear is gone.
- VU1 native XGKICK now streams one qword per 2 cycles (rt-wipeout.patch, vurecomp-wipeout.patch): fixed exploding race polygons.
- EE COP2: VMAX/VMINI use PS2 ordering, VFTOI saturates, VCLIPw uses integer magnitudes, LQ/SQ mask the low 4 address bits.
- Software GS: ZTE=0 now means no depth test and no depth write. RT_GSREGLOG=<secs> logs FRAME/ZBUF changes.
- Race FRAME is 512 wide (FBW 8) at FBP 0, Z32 at ZBP 0x70.
- `sceGsSetDefDBuff`, `sceGsSetDefDrawEnv`, `sceGsSetDefDispEnv`, `sceGszbufaddr` are no longer stubbed. The DBuff stub read args 5-7 from the stack (EE passes them in t0-t2), dropped the clear packet and put both buffers at FBP 0, so Z was never cleared: that hid the track.
- The game runs in interlaced field mode (SMODE2=3), 512x224 buffers. rt-wipeout.patch now also patches GsGpu present() to halve DH in field mode (DisplayRegs gained smode2; rufftrigger_gpu.cpp passes it). setup.sh copies render/*.{h,cpp} and rufftrigger_gpu.cpp into build/rtsrc before patching.
- Result on the GPU renderer: full races render correctly (track, stadium, ships, HUD, countdown). RT_DISPLOG=1 prints display env changes.
- Still open: IPU video not decoded (menu backgrounds, billboards); software renderer race view still wrong.

## Update 2026-10-19 02:05 UTC
- Prebuilt tarballs must be built with `-march=x86-64-v3` (`ARCH=-march=x86-64-v3 ./setup.sh`); the build container has AVX-512 and a native build crashed on the tester's CPU with "Illegal instruction".
- IPU videos (.IPF): header {"IPUF", u32 w, u32 h, u32 frames, u32 slot}; frame k at 20 + k*slot: u32 len, 60 pad, then len bytes of IPU bitstream ending 000001B0 000001B1. wipeout_override.cpp replaces the two players' IPU helpers (0x211618/16E8/1728/17A0 RGB32 front end, 0x212590/2660/26A0/2718 RGB16 billboards) and decodes the whole frame with FFmpeg's `ipu` decoder (16-pixel-wide column = macroblock order), then IPU CSC. RT_IPULOG=1 logs frames. Verified: main menu background plays.
- Sound (wipeout-runtime.patch, ps2xIOP): (1) WaitSema/DelayThread called outside any IOP thread (RPC server functions run directly) now run the other IOP threads until satisfied (IopKernel::setWaitPump); MCD.IRX's read API used to spin out its budget. (2) An RPC to a SID not yet registered runs the IOP up to 0.1 s for the server to appear (CSS.IRX's init call was dropped, so sceSdInit never ran). Race audio now records non-silent. Debug: IOP_SPU2LOG=1, IOP_PCWATCH=hex,hex.
- Pad: analog triggers give L2/R2 pressure (RT_TRIGGERS=l1,r1 etc. remaps); pad mode reports 0x79 after scePadEnterPressMode. The game reads pressure bytes 8..19 when data[1]>>4 == 7 (0x114250).
- Local test WAD: ~/wo/media IPFs injected into FILESYS.WAD via the WAC table (scratchpad inject.py). Music is still zeros locally.
- Open: late-race polygon explosion on the tester's machine (not reproduced in a 30 s race here at ~38% speed); possibly timing-related (VU1 XGKICK streaming fix was timing based).

## Update 2026-10-19 04:00 UTC
- Van Uber (and other non-Feisar) race load hung/crashed: deadlock. The EE blocks inside an RPC to MCD
  (close waits for pending reads) while MCD's worker thread waits on sceSifCheckStatRpc for an IOP->EE
  call the blocked EE can never run. Fix (ps2xIOP iop_rpc.cpp/.h): while the EE is inside handleRpc,
  IOP->EE sceSifCallRpc counts as done once queued (per-client seq guards the late completion).
- No menu music: libgraph's sceGsSyncVCallback handler got the vsync counter in a0; the game's handler
  (0x205758) only signals the FE music feeder thread when a0 == 2 (INTC_VBLANK_S). EeScheduler.cpp now
  passes 2. MUSICFE.PCM = interleaved 16-bit stereo PCM, fed 0xC80 bytes at a time via CSS fn=17.
- Race music (MUSIC%d.WAV, CSS streamer) verified against a real MUSIC0 head: plays at correct speed.
- iop_kernel.cpp prints "[iop] WaitSema(n) outside a thread timed out" (+ thread dump) when a nested wait fails.
- Local WAD: MUSIC*.WAV filled from the tester's real 4 MB head (looped); MUSICFE.PCM holds a 440/660 Hz tone.
- 10-minute Feisar race here showed no polygon explosions; still unreproduced locally.
- Packaging: after copying ps2EntryRunner into the package, run patchelf --set-rpath '$ORIGIN/../lib' bin/wipeout-fusion (forgot once, the tester got libavcodec.so.60 not found).

## Update 2026-10-20 12:05 UTC (third fixing thread)
- Florion Height exploded from the start: the game memcpys camera matrices to VU0 data memory
  (0x11004E10, caller 0x1EC980); ps2_stubs::memcpy/memset/memclr masked that into RAM 0x01004E10,
  on top of the track's VIF packets. LibC.cpp now maps 0x11000000-0x1100FFFF to the VU memories.
  Mandrashee raced fine here; the tester's crash there may be the same bug at another address.
- ps2_iop and ps2_runtime now build -O2 (the game code stays -O1). In a race the game thread
  spends about 25% in the IOP interpreter, nearly all of it CSS.IRX css_streamer_thread mixing
  PCM in software (about 190 wakes per guest second, about 16M IOP instructions per guest second).
  That is real work, not a spin. VU1 programs take about 26% and processVIF1Data about 9%.
- Test harness (scratchpad, lost with the session): Xvfb :9 1280x960, RT_GPU_SCALE=1,
  RT_PADFILE=<file> (new: hex PS2 button bits held while the file exists), menu path from boot:
  ~25 s, X (no memcard), Start, X arcade, X Feisar, X pilot, Right×n venue (0 Florion, 1
  Mandrashee, 2 Cubiss Float), X, X course. Local WAD rebuilt with recomp/wof_rebuild_wad.py
  from out.zip plus the IPF zip; IRX files are the uploads named by module.
- New debug env: RT_VIFTRACE=1 (dump VIF1 command ring and DMA tags at the first invalid VIFcode),
  RT_MEMW=<hex> now actually hooks memcpy/memset (0x22B088/0x22B138), RT_GSREGLOG also logs XYOFFSET.
- Open: the tester reports rough fps and jitter; XYOFFSET and DISPLAY are constant in races (no
  per-field offset), so this needs the tester's RT_FPSLOG output and monitor refresh rate.

## Update 2026-10-20 13:00 UTC (third fixing thread, round 2)
- Ship stats all 0 on the pilot screen: Ps2VarArgCursor::nextU64 (Support.h) read 64-bit varargs
  as register pairs, o32-style. The EE uses EABI, so each vararg (soft doubles from fptodp 0x227CD8
  included) is one full 64-bit GPR. Fixed; stats now print (1058 kph, 7150 GFU ...).
- Frame pacing: publishHostPresent's display lock accepted any 15-18.5 ms present interval, so a
  165 Hz monitor (3 presents = 18.2 ms) locked the guest to 92% speed and audio
  ran off-rate. Lock now only for 16.2-17.2 ms; logs "[run] display lock on/off (present interval X ms)".
  Unverified on the tester's hardware; likely also the music speeding up/slowing down.
- New wipeout_settings.cpp (setup.sh copies it): F10 or L3+R3 opens a PC SETTINGS overlay
  (resolution 1-8x/auto via GpuSetScale, window/full screen, 4:3/16:9/stretch, vsync, FPS counter,
  restore defaults), saved to graphics.ini in the cwd (RT_SETTINGS overrides). DEBUG row "Unlock
  everything" replays cheat 8's writes (0x168AD0..0x168B74, $gp = 0x2891F0) from the pad hook,
  which runs on the game thread once a frame; the game then shows its "new weapon unlocked" screens.
  It must be installed last in ApplyWipeoutFusion because it chains the RT_PADFILE pad hook.
  Note the INTC VBLANK handlers (0x18B708, 0x214008) are effectively never called, so don't hook them.
  Test env: RT_GFXMENU_OPEN=<row> starts with the menu open, RT_UNLOCKALL=<n> unlocks after n pad reads.

## Update 2026-10-20 14:10 UTC (third fixing thread, round 3)
- Fast loading (wipeout_settings.cpp + EeScheduler/ps2_capture_hook.h g_turbo): while g_turbo is
  set the EE scheduler ignores host deadlines (vblanks fire as soon as the guest reaches them).
  Loading is paced by guest vblanks, not CPU: race load took 26 s here, 10.6 s with turbo.
  Detector: the game's file read 0x201CB8 (a3 = 0x20 for menu-movie streaming reads, 0x4000
  otherwise) runs <= 4/s in menus and races but 30-150/s while loading; turbo turns on at 12
  non-streaming reads within 30 vblanks, off 30 vblanks after the last, then the SPU2 output ring
  is flushed. Menu row "Fast loading" (graphics.ini fastloading=). RT_TURBOLOG=1 logs switches.
- Music: SPU2 output rate verified at 48 kHz against wall clock (no bursts); FE music (MUSICFE.PCM)
  plays a 600 Hz test tone at exactly 600 Hz. CSS mixes everything in software into core-0 ADMA
  (feed = consumption, queue ~1500 samples). The FE feeder 0x205758 is the gs vsync callback,
  called once per vblank. Local race recordings contain no music at all (same output with a
  48 kHz or 24 kHz tone in MUSIC*.WAV), so race music is unverified; CSS has upsamplemixs for
  non-48 kHz files. The INTC VBLANK handlers are removed by the game at boot (0x213ca0, 0x18be9c).
- Display: SMODE2 = 3 (interlaced field mode), DISPFB alternates FBP 0 / 0x38 (512x224 each);
  XYOFFSET/DISPLAY constant; no frame-to-frame vertical shift measured. Flicker cause unknown.
- Local WAD music entries now hold test tones (1000 Hz race, 600 Hz FE), not the tester's audio.

## Update 2026-10-20 14:40 UTC (round 4)
- the tester: 240 Hz monitor, RTX 4090. RT_FPSLOG showed 100% speed with frequent 160-630% bursts
  (fast loading firing on short menu loads, with the sound still on) and the display lock
  flapping (x4 multiplied present jitter). Now: fast loading needs a full second of busy reading
  before it starts and mutes audio while on (ring disabled); the display lock only applies on
  60/120 Hz (n <= 2) displays. Race speed on the tester's PC dips to 70-92% (CPU-bound).
- Deinterlace (rt-wipeout.patch, GsGpu.cpp/h + rufftrigger_gpu.cpp): present() renders each field
  into fieldTex[2] ping-pong, then a kDeintFs pass into displayTex: 0 off, 1 blend, 2/3 bob
  TFF/BFF (+-0.25 line shift by DisplayRegs.field = vsyncTick & 1), 4/5 weave TFF/BFF.
  F10 menu row "Deinterlace", graphics.ini deinterlace=off|blend|bob-tff|bob-bff|weave-tff|weave-bff.
- Local race has no music at all: the IOP only streams billboard IPFs from the WAD during races;
  the EE never starts MUSIC%d.WAV here (unknown why), so race music can't be checked locally.

## Update 2026-10-20 15:50 UTC (round 5)
- Z-fighting (GsGpu.cpp, rt-wipeout.patch also covers gl_api.h now): the game draws with a
  32-bit Z buffer but keeps Z tiny (max ~540k of 2^32; far track ~2^11..2^15). The old
  `z * 2 - 1` clip depth landed next to -1 where a float step is ~128 Z units, so coplanar
  layers fought at a distance. replay() now switches glClipControl(LOWER_LEFT, ZERO_TO_ONE)
  (uniform uZ01) and restores the default afterwards (raylib needs [-1,1]); falls back if the
  driver lacks GL 4.5 clip control. Log line "[gsgpu] depth: ...". The inverted-depth path
  (1 - z) still loses precision, but Wipeout never uses it.
- GPU stalls: loadClut() read the frame buffer back (glReadPixels) ~5 times a frame. The game
  sets CLD on 32-bit textures and loads a dummy PSMT8 CLUT from CBP=0 (its frame buffer) to reset
  the CLUT cache. Now non-indexed TEX0 loads nothing and indexed loads are deferred to the first
  indexed texture (flushClut in getTexture, and before a VRAM transfer overlapping the source);
  a later full 256-entry load replaces a pending one. RT_FPSLOG readbacks went to 0.00.
- IOP: servicePendingDmaInterrupts walked a std::map every IOP instruction while any DMA was
  pending (always, with SPU2 streams); now gated on nextDmaDue (iop_emulator.cpp). ~6% of the
  game thread. Remaining game-thread profile: VU1 programs ~28%, VIF1 ~12%, IOP interpreter ~20%
  (CSS software mixer at IOP 0x97xxx-0x9Axxx, real work, no idle loop).
- Fun mod "FUN: 2-player weapons" (graphics.ini mpweapons=): weapon pad picker 0x11D348 builds
  the allowed mask in $s1; mode word 0x3A4F58 < 12 clears 0x05E00000 (ids 21 Hunter Missile,
  22 Turbo Enforcer, 23 Stealth, 24 Global Drain, 26 Control Jammer); >= 12 adds them when
  $s5 (human ship) != 0. Weights at 0x3A4988 (27 ints), total -> 0x2826C8, count -> 0x285120.
  The mod wraps rand 0x22BD80: when called from the picker (ra 0x11D528) it ORs the bits back
  and redoes total/count. Tested: Stealth, Control Jammer and Hunter Missile picked up and fired
  in an arcade race. Weapon ids follow the name table at 0x2482F8.

## Update 2026-10-20 21:30 UTC (round 6)
- the tester: Bob (bottom first) fixes the flicker. Deinterlace is now fixed to gsgpu mode 3
  (kDeinterlaceMode in wipeout_settings.cpp, GsGpu g_deinterlace default 3); menu row and
  graphics.ini deinterlace= key removed.
- AppImage: WipEout_Fusion-x86_64.AppImage (appimagetool continuous + type2 runtime, AppDir =
  usr/bin/wipeout-fusion, usr/lib/* from the tarball, usr/share/wipeout-fusion/iso_extract.py).
  AppRun mirrors play.sh but uses the folder containing $APPIMAGE (game/ next to it). Built in
  /tmp/claude-0/ai; tested with APPIMAGE_EXTRACT_AND_RUN=1 (no FUSE in the container).
- Music "a bit fast randomly" still open (race music can't be heard locally). Added to RT_FPSLOG:
  "[audio] produced N/s, queued ms, dropped, underruns" (iop_audio.h counters). Menus here:
  produced 48000/s, 0 dropped. If the tester's log shows produced > 48000 or drops, the guest is
  outrunning the output; if not, the speed-up is inside the guest's music stream.

## Update 2026-10-20 23:00 UTC (round 7)
- Music fast: the tester's RT_FPSLOG showed "produced ~60000/s" in races. RPC handling and kernel
  wait pumps ran IOP time on top of the EE-driven slice, so the IOP ran ~25% ahead. Fix in
  iop_emulator.cpp runEeCycles: track eeClock (IOP cycle the EE has reached) and only run the
  IOP up to it. Local race now ~48000/s. Boot got slower, so test scripts use BOOTWAIT=38.
- Flamethrower / rockets: DualShock 2 pressure bytes 16..19 are L1, R1, L2, R2; Pad.cpp and
  ps2_pad.cpp had L1, L2, R1, R2, so R1 (default fire) pressure read as 0. Ship byte +0xD91 =
  fire pressure (written by the pad mapper at 0x1155D8/0x115864). The flamer update 0x1C0678
  only emits and burns fuel while it is nonzero; rockets get launch speed from it (spawn
  0x1B7168 f12 -> projectile +0x138, update 0x1CDC20 adds +0x138 * 30). Other refs: fire
  handler 0x11C330 (jump table 0x257A10 by weapon id), absorb 0x11C110 (ship +0xD84 bit 0x4000),
  fire bit 0x800, projectile pool [gp-0x5778] x 0x180, update fn table 0x271F60 (id+1).
- Test pad bits: 0x4000 cross (thrust), 0x0800 R1 (fire), 0x8000 square (absorb in default
  config). /tmp/claude-0/ws.r7dbg has the weapon/projectile trace hooks (not shipped).
- >60 fps: game logic is a fixed step per frame (projectile timers count 1 per frame,
  positions += velocity per frame, no delta time), so a higher vblank rate just speeds the game
  up. A real high-refresh mode would need render interpolation, which this port can't do.

## Update 2026-10-20 23:45 UTC (round 8)
- the tester: round-7 build hangs on a dim screen after the first load, where the intro FMV would
  be. Not reproduced here (AppImage and build binary reach the title with/without save, vsync
  off, fast loading off). Awaiting their log.txt. Hardening guess: runEeCycles in
  iop_emulator.cpp now caps eeClock lag at kIopClockHz/30, so after a long load the EE can't
  owe the IOP seconds of catch-up (which froze the IOP and anything waiting on it).
- "FUN: all super weapons" (graphics.ini superweapons=1): in RandWrap (picker rand, ra
  0x11D528) every super bit (mask 0x29F80: ids 7,8,9,10,11,12,15,17) whose use count
  (0x364F08) is under its limit (0x249A28, 2 per race) is ORed into the pad mask 0x3A4978;
  zero-weight additions get the average weight, total 0x2826C8 / count 0x285120 redone, s1
  rewritten. Applies to all ships. RT_WEAPONLOG=1 prints the mask per pick and the 16 ships'
  held weapons every 60 vblanks. Verified: ships held all eight supers in one race.

## Update 2026-10-21 02:20 UTC (round 9)
- Intro FMV freeze (rounds 7-8): the front-end player 0x211410 plays WOF.IPS / WOFAC3.IPS (OUTRO.IPS
  too) after the save check. Header "IPUS" (byte 3 'S' -> sound flag gp-0x4350); slot = {u32 videoLen,
  u32 audioLen, pad to 0x40, video, audio}. Audio is pushed per frame with 0x1FFD00 -> CSS RPC sid
  0x54321 fn 17, retried until the IOP stream buffer has room. Local WAD has zeros for all .IPS, so
  the movie never ran here; ~/wo/game2 is a copy whose WOF/WOFAC3.IPS hold FE.IPF as IPUS with a
  synthetic audio chunk (/tmp/claude-0/setaud.py N sets chunk size).
- Cause: round-7 eeClock pacing. Each retry RPC (~1483 IOP cycles, server code only) advanced
  totalCycles, the EE charged ~nothing, so the IOP stayed "ahead", runEeCycles never ran the threads
  that drain the stream, guest went to 1% speed. Fix in runEeCycles/handleRpc: short (<1/2000 s),
  back-to-back repeats of the same sid/fn, minus pumped-thread time, count as rpcStarved; when
  starved >= 1/240 s and RPC time >= 4x EE time, run the threads for it and reset eeClock. Long
  calls (CSS fn 9 mixes 140-174k cycles per call when the host is slow) don't count; an earlier
  broader rule made race music 30% fast on a slow host. Verified: test movie plays to the title;
  races at 100% and at ~40% (llvmpipe) produce 48000 x speed samples/s, 0 triggers.
  Debug: RT_STARVELOG=1, RT_RPCLOG=1, RT_IOPPACE=0 (pre-round-7 unpaced IOP).
- Movie frames go to the GS as ~1200 small uploads+draws per frame; llvmpipe runs it at ~25-40%.

## Update 2026-10-21 03:35 UTC (round 10)
- 16:9 race HUD (rt-wipeout.patch GsGpu.cpp/h + gl_api.h BufferSubData; wipeout_settings.cpp):
  F10 row "Race HUD (16:9)" = Screen edges (default) / Stretched, graphics.ini racehud=edges|stretched.
  Active only with Picture shape 16:9 (gsgpu::SetEdgeHud from ApplyAspect) and while the race gauge
  update 0x17F4F8 has run in the last 0.5 s (wrapped -> gsgpu::NoteRaceHud).
  In 512-wide main targets: untextured no-Z flat meshes start the HUD (Target::hudSeen), textured flat
  draws after them in the frame are its text/icons, any non-flat draw ends it. Vertices are remapped on
  the CPU (BufferSubData over the draw's range): per row (y<112 top) spans anchored left/centre/right
  with x*3/4 proportions (top {0-160 L, 164-367 C, 370-512 R}, bottom {0-262 L, 312-512 R}), linear
  stretch between spans; panels per vertex, textured runs of touching triangles <128 wide move rigidly.
  HUD files: DATA/HUD/*.HUD in the WAD, 512x224 float verts; ARCADE_1PLAYER element boxes guided spans.
  Known: at 1x internal resolution thin glyphs lose columns (fine at 2x+); the start countdown
  sprites are drawn before the HUD and stay stretched; 2-player split HUDs untested.
- Speed (wipeout-runtime.patch): IOP import-stub decode cache (iop_imports), inline RAM fast paths
  for IOP read8/16/32 (iop_memory), VIF masked-unpack fast path (ps2_vif1_interpreter; RT_VIFCHECK=1
  compares against the old path: 0 mismatches over 39M unpacks; RT_VIFSTAT, RT_VIFTIME debug).
  Gains not measurable here (llvmpipe is GPU-bound); profile: VU1 ~30%, IOP interp ~18%, VIF ~10%.

## Update 2026-10-21 04:00 UTC (round 11: weapon tuning, wipeout_settings.cpp)
- F10 rows (graphics.ini): "FUN: weapon damage" damage=0..3 (Half/Normal/Double/Triple),
  "FUN: weapon uses" weaponuses=1..3, "FUN: per-race limits" nolimits=0/1. Run by
  WeaponTuningTick after the race HUD gauge update 0x17F4F8 (game thread, race frames only).
- Ships 16 x 0xE90 from 0x2DBD20; physics block at ship+0x40 (re/ship-fields.md offsets are
  relative to it): weaponID +0xCA4, iLastWeapon +0x244. Ammo ship+0xDF8 (pickup 0x11E9F0: 25 proton
  cannon, 5 mines, 99 flamer, else 0). Pilot record ptr ship+0xE00: byte +0xC = shield (0-100).
  0x193620 per frame: slow drain (byte 0x2833E0 = 2 every 20 units, timer pilot+0x54 reset to 0),
  regen (byte 0x2833E8). Rocket hit 0x10BCB8: 50 (table 0x244F38) * 0.04 (0x2833D4). Ship-ship
  collision 0x108A90: byte 0x2833D8 (5). Per-race limits 0x249A28 (26 ints).
- Damage scales per-frame shield losses (except the slow drain); never lifts a ship from 0.
  Uses: when weaponID goes to -1 the weapon and its ammo come back, N-1 times per pickup.
  Debug RT_SHIPLOG=1 (shield/weapon/ammo per ship every 30 race frames, restores, scaled hits).

## Update 2026-10-21 05:55 UTC (round 12: widescreen camera, 2-player)
- Rebuild without an ISO (fresh container, ~10 min on 4 cores): ruff-trigger-linux is the upload
  4ced6306 (tar.gz). Make build/game by hand: SLUS_204.62 = f1c947fa, SYSTEM.CNF = 6aef4d6f,
  WOF/FILESYS.WAC = 1a03a397, WOF/IRX/IOPRP242.IMG = 0b38ce8f, IRX by .iopmod name: cdvd_driver=CDVDMAN
  1ef640ef, mcserv=MCSERV 33444f18, multitap_manager=MTAPMAN 5aa74685, MCD 6b9c24da, mcman=MCMAN 7c50d98e,
  sio2man=SIO2MAN 7c91b98b, Sound_Device_Library=LIBSD 806b1985, css=CSS aa63622c, padman=PADMAN c579fcbf.
  WAD: recomp/wof_rebuild_wad.py WAC WAD out/wof_core.zip out/wof_tracks.zip (from out.zip), then write
  the IPF zip b04ea9d2 entries at their WAC offsets. Then `ARCH=-march=x86-64-v3 ./setup.sh <any file> <ruff dir>`
  (step 1 is skipped when build/game exists). apt: libx11/xrandr/xinerama/xcursor/xi/gl1-mesa/asound2/
  pulse/avcodec/avformat/swscale/swresample -dev, patchelf. Note src/game/wipeout_*.cpp are copies
  (edit wo/*.cpp and cp them in), render files are symlinks into build/rtsrc.
- Test pads: RT_PADFILE (port 0) and new RT_PADFILE1 (port 1). Menu input lags several seconds on
  llvmpipe: hold 1 s, wait 3-8 s between presses. Don't `pkill -f ps2EntryRunner` from a shell
  whose command line contains that word (it kills itself); use pkill -x.
- The F10 16:9 picture never turned on the game's 3D widescreen (Options > Preferences >
  Widescreen, prefs word 0x3965CC), so the whole race view was stretched 4/3 (the "stretched
  countdown icons" were the gantry signs, ordinary 3D). wipeout_settings.cpp ViewSetupWrap wraps
  0x1EC160 (view setup; race call ra 0x19E408, a0 = mode 0 4:3, 1 16:9, 2 2P side by side,
  3 2P top/bottom; multiplayer prefs 0x396620 = 1 -> mode 2). Viewport table 0x24DA60, 0x20 per
  entry, aspect at +0x14. 16:9 forces mode 1 and sets the split aspects (0x24DAB4/D4/F4, 0x24DB14)
  to 16/9; 4:3 restores 4/3. Applies from the next race. Front-end calls are untouched.
  (wipeout_override.cpp's ProjWide/SceneWide are Ruff Trigger leftovers and are never installed.)
- Pad ports (wipeout-runtime.patch, ps2_pad.cpp): port 1 used to mirror port 0. Now port 1 =
  host gamepad 1; with one gamepad the keyboard is port 1; with none, keyboard = port 0. The
  RT_PADFILE hook only feeds port 0. 2P team select won't accept player 1's team.
- 2P HUD (GsGpu.cpp): gsgpu::SetHudLayout(0 1P, 1 top/bottom, 2 side by side) from ViewSetupWrap.
  Layouts: 1P as before; H views 512x112, rows split at y 45, spans {0-200 L, 430-512 R} /
  {0-160 L, 380-512 R}; V views 256x224, {0-164 L, 175-256 R} / {0-100 L, 130-256 R}, player 2's
  HUD is drawn 13 px left of player 1's position in its view (P2 spans looked up at x+13).
  RT_HUDLOG=1 logs each edge-mapped draw (x,y>new x). RT_SPLIT=2|3 forces the multiplayer split
  option (tests). Checked here: both splits, 1P race. Not checked: other HUD modes (time trial, zone,
  challenge) use the 1P spans as before.

## Update 2026-10-21 06:15 UTC (round 13: weapons toggle)
- F10 DEBUG "Weapons toggle" opens a second page (g_page 1): Custom set on/off, All on, All off,
  26 tick boxes (kPadWeapons, ids 0-26 without 20 SHIELD_RAIDER, limit 0), Back.
  graphics.ini weaponset=0/1, weapons=<hex mask by weapon id>. CustomWeaponPick (in RandWrap at
  the picker, ra 0x11D528) replaces $s1 with the mask for every ship, gives zero-weight weapons
  the average weight, redoes total/count; it takes precedence over the 2-player/super weapon mods.
  While a non-empty custom set is on, WeaponTuningTick raises the per-race limits (QUAKE's is 1).
  Tested: weapons=20 (quake only) gave 25 quakes in one race, no hang. Test env:
  RT_GFXMENU_OPEN=<row> RT_WEAPONPAGE=<row> opens the page; xdotool keys don't reach the window
  under bare Xvfb.

## Update 2026-10-21 07:10 UTC (round 14: FOV slider; IOP native work started)
- Rebuild from scratch in a fresh container worked as in round 12 (note: `bash ./setup.sh <existing file> <ruff dir>`,
  the ISO arg must be an existing file even when build/game is prepared by hand; setup.sh lost its x bit on the mount).
  Packaging without appimagetool: extract the old AppImage (--appimage-extract), swap usr/bin/wipeout-fusion,
  patchelf rpath, mksquashfs -comp zstd, prepend the first --appimage-offset bytes of the old AppImage (runtime).
- FOV: the viewport table 0x24DA60 (+0x10 = 110) is NOT used by the race camera. The race projection is rebuilt
  every frame by 0x1EDF00 (a0 = view, called from 0x1400dc/0x17bc68) from view+0x44 (projection distance d, set
  by the camera code to 240), +0x3C width 512, +0x40 height, +0x4C near, +0x50 far; horizontal FOV =
  2 atan(256/240) = 93.7 deg. 0x1ED820.. temporarily sets d = 240 for a sub-render and restores it.
  wipeout_settings.cpp ProjectionWrap wraps 0x1EDF00: in race views (g_raceView from ViewSetupWrap) it widens
  d by g_set.fov degrees for that call only and restores it. F10 row "Field of view" -30..+40 step 5,
  graphics.ini fov=. Verified +40 (93.7 -> 133.7 deg) at the race start. RT_FOVLOG=1 logs it.
- IOP profile (IOP_PROF=1, new, prints the top 64-byte buckets per 64M IOP instructions with module+offset):
  in a race CSS.IRX ~65% (UpdateListeners 0x57A4 3D sound ~40%, css_streamer_thread 0x314C ~25%: it inlines
  clip+copy of the two 512-entry int32 accumulators base+0x3C000/0x2BB80 and the mixers, loops 300 stream slots
  (stride 0x158 from base+0xA2D8) per 256-sample block), MCD.IRX ~10% (fill_cache, worker_thread).
  CSS and MCD have full symbol tables (readelf -s). The mixer leaf functions (mixs .. clipandcopy) are NOT
  called in races (inlined); ps2xIOP native/iop_native.cpp has hand ports of them plus IOP_NATIVECHECK
  (run native on a RAM copy, compare after the interpreter returns), disabled unless IOP_NATIVE=1.
- Test harness this round: /home/claude/wo/t/race.sh (lost with the session): Xvfb :9, RT_PADFILE presses
  4000 (x6 with 6-8 s gaps, after a 40 s boot) reach the Florion arcade race start. RT_AUDIO_WAV=<file> is
  needed for the sound driver to run (no ALSA device here: RT_AUDIO default produced 0/s).

## Update 2026-10-21 07:30 UTC (round 15: IOP sound + file drivers native)
- New iopxlat.py (setup.sh runs it after the VU step): translates every FUNC symbol of CSS.IRX (106 functions)
  and MCD.IRX (41) into ps2xIOP/src/emulator/native/iop_aot_modules.cpp (generated from the disc, not in the
  patch). Runtime: native/iop_aot.{h,cpp} (in wipeout-runtime.patch). Modules are matched by an FNV hash of
  .text with relocatable fields masked; relocated immediates are read from the loaded code, so any load address
  works. Execution enters native code at labels (entries, branch/jump-table targets, after every jump/branch,
  after anything unsupported) from IopEmulator::step() and leaves on calls (dispatcher re-enters translated
  callees, imports go to the interpreter), returns, jr, syscall/break/COP. Each block adds its exact guest
  instruction count, so IOP timing is unchanged; backward branches return after 4096 instructions.
  The old hand-written mixer hooks (iop_native.*) are gone.
- IOP_AOTCHECK=1: every native slice runs, is undone via an IopMemory write log, then the interpreter runs the
  same instruction count; registers, pc, hi/lo and all written bytes are compared (mismatches print
  "[iop-aot] MISMATCH", interpreter result kept). Boot + menus + Florion race: 8.45M slices, 0 mismatches.
  IOP_AOT=0 turns the native code off.
- perf (game thread only): IOP was 19% of the game thread with the interpreter, 3% native (rest is the SPU2
  voice emulation). Speed here is llvmpipe-bound, so no fps change measurable locally.
- Not exercised locally: intro FMV audio (IPS files are zeros here), real music streams (zeros), memory card
  saving (MCMAN/MCSERV stay interpreted; MCD file reads are native).
- Tools this round (lost with the session): /home/claude/wo/package.sh (tarball + AppImage repack),
  mkpatch.sh (regenerates wipeout-runtime.patch against a clean worktree of the pinned commit + ruff patch,
  skipping iop_aot_modules.cpp), iopshare.sh (perf report: IOP share of the GameThread).

## Update 2026-10-21 08:35 UTC (round 16: all IOP drivers native, VU1 speedup)
- iopxlat.py now covers all 9 IRX the game loads (css, mcd, libsd, padman, sio2man, mtapman, cdvdman, mcman,
  mcserv); regions = FUNC symbols plus every gap outside import/export tables, so symbol-less modules work.
  IOP_AOTCHECK run (boot, menus, race): 6.15M slices, 3 mismatches, all LIBSD reading SPU2 status registers
  twice (read side effects in check mode, not translation errors).
- Pads (scePad*) and memory card (sceMc* -> host files in mc0/) were already HLE'd on the EE side; PADMAN/
  SIO2MAN/MCMAN/MCSERV are loaded but idle. SPU2 voice emulation was already native C++ (~1% game thread).
- VU1 (vurecomp-wipeout.patch, rt-wipeout.patch, vu1entries.txt, setup.sh passes VURECOMP_ENTRIES):
  * RT_VU1ENTRIES=file logs every distinct (program id, entry pc) the dispatcher starts (vu1_fast.cpp).
  * Programs with recorded entries get switch cases only for those + chunk starts + branch targets + BAL/JALR
    return points + E-bit resume points. A full-entry copy prog_<id>_all (vu1_<id>_all.cpp) is registered right
    after it; Dispatcher::run retries the next covering program when one leaves at once at its entry.
  * static_scoreboard() in vurecomp.py: inside straight-line traces, ready-cycle checks against writes >= L
    pairs earlier are dropped, closer ones use locals; vfR/accR/viR stores are dropped when overwritten before
    control can leave or when it can only leave >= 4 pairs later (all latencies <= 4).
  * Hottest program: checks 759 -> 231, scoreboard stores 528 -> 107. perf samples per million VU1 pairs
    6.42 -> 5.5..5.8 (10-15%). RT_VU1VERIFY (native vs interpreter) mismatch pattern identical to the old
    build (pre-existing 1-ulp float diffs at the same pcs, no kick or vi mismatches).
  * Remaining VU1 cost is register-file memory traffic (c.vf loads/stores); next step would be promoting vf
    registers to locals per trace.
- IOP_PROF interval restored to 64M instructions.
- Next per the tester: native renderer (graphics), VIF/data packets later.

## Update 2026-10-21 09:30 UTC (round 17: native renderer started)
- Plan doc: https://claude.ai/artifact/TzbP1TFDhxv7x163hu8CRC (milestones, census, risks).
- wipeout_override.cpp (debug only, no behaviour change; the shipped 08:35 build does not have it):
  * RT_GFXCENSUS=secs: parses every GIF packet, counts vertices/prims/image bytes per source (PATH1 keyed by
    VU1 program name + entry pc, PATH2, PATH3). Florion race: ~1.0M vertices/s; g3841391935 48%,
    d1604436274 14%, g1331843391 12%, d939320750 8.5%, g1920671007 7.7%, g1920694623 5.9%, d1159117218 4.7%,
    EE direct (PATH2/3) 0.2% of vertices (PATH2 = ~6 MB/s texture uploads + ~110k A+D writes/s).
  * RT_NATIVECHECK=1: for g3841391935 entry 0x1410 (strip loop L1278..L1418): header at TOP (GIF tag, NLOOP
    = vertex count, .w = mode), then per vertex XYZ, RGBA, STQ qwords; transform p = vf1*x+vf2*y+vf3*z+vf4
    (vf1-4 built by L1500 from VU mem 0..3 (object) x 0xc..0xf; vf13-16 from 0x10..0x13 = clip matrix),
    Q = 1/p.w, XYZ = ftoi4(p*Q), STQ = stq*Q, RGBA copied; output tag + (STQ, RGBA, XYZ) per vertex,
    XGKICK at 0x13f8. Guard-band clipper subroutines L1fa8/L1628/L20f8 (scratch at 0xf1..0x123).
    Result over 10.9M on-screen vertices: 98.4% bit-exact XY, 99.96% within 2/16 px, RGBA always equal,
    STQ equal; 5% of batches go through the clipper (skipped by the checker).
- Next: milestone 2 = submit these triangles straight to gsgpu::Recorder in GIF order (needs a hook in the
  runtime GS/GIF arbiter so native draws keep their place between PATH2 state writes).

## Update 2026-10-21 09:30 UTC (round 17b: native renderer milestone 2 shipped)
- Build shipped (same two file names). Native strip drawing is ON by default; RT_NATIVEDRAW=0 turns it off.
- wipeout_override.cpp NativeStrip(): for g3841391935 entry 0x1410 the batch is computed natively instead of
  on the VU, under the VU's float environment (MXCSR round-toward-zero + FTZ/DAZ, fp-contract off, same
  operation order as the MADD chain). Tag must be PACKED, NREG 3, regs 0x512 (STQ RGBAQ XYZ2) or 0x412
  (STQ RGBAQ XYZF2: z = (ftoi4 z >> 4) & 0xFFFFFF, fog = ftoi4(w*Q) >> 4). ADC rule (verified): header.w&3==3
  = lists, tested vertices k%3==2, else k>=2; clip flags from vf13..16 (clipw: +x,-x,+y,-y,+z,-z vs |w|);
  ADC when the last 3 vertices share an outside plane; any other outside vertex -> VU clipper -> fall back
  to the VU for the whole batch (~3.5% of batches). XY wrap to 16 bits like the GS write.
- Output = one native vertex packet through PATH1 (magic "RNVVERTX" qword, count | xyzf<<32, original tag,
  32 bytes/vertex: x,y float, z u32, rgba | s,t,q, fog, adc). GS::tryProcessNativeVertexPacket
  (gs_frontend.cpp/.h, in wipeout-runtime.patch; mkpatch.sh now also lists gs_frontend.h) feeds vertexKick.
- RT_NATIVECHECK=1 compares the native packet with the VU's kick field by field: 3.0M vertices, 0 differences
  in xy, z, fog, ADC, RGBA, STQ; 96.5% of batches accepted. Screenshots of a Florion race look the same.
- perf: prog_3841391935 2.55% of samples -> 0.54% (its VU fallbacks) + NativeStrip 0.47% + native GS 0.15%.
- RT_NATIVEPRECISE=1: unquantized float x/y for on-screen vertices (not default; looks slightly different).
- Next: milestone 3 = same for d1604436274, g1331843391, d939320750, g1920671007 (disassemble from
  build/vu1gen/vu1_<id>.cpp comments; RT_GFXCENSUS shows entry pcs); milestone 4 batching per draw state.

## Update 2026-10-21 13:30 UTC (round 18: native renderer milestone 3 shipped)
- Build shipped (same two file names). Five VU1 drawing programs now run as native C++, all on by default:
  g3841391935 @0x1410 (clipped strip), g1920671007 @0x2540 (lit clipped strip), d1604436274 @0x38/0x158
  (plain strip), g1331843391 @0xd58/0xee0 (lit strip, 3 lights, vf26 fed back), d939320750 @0x3218
  (particles: R random drift, size table, Z/S flag culls, one sprite kick per particle).
- New files vu1_native_draw.{h,cpp} (setup.sh copies them into src/game; vu1_* so built with
  -ffp-contract=off). Each function returns the exact GIF packet(s) the VU would kick, using the VU float
  helpers (vn, f_mul/f_madd/f_add/f_sub, 4-lane fast path like the recompiler, ftoi, RZ+FTZ/DAZ). Register
  side effects written back: vf26 (1331), R/CLIP/I (939). VU memory side effects: lit colours (1920) and the
  particle output buffers (939). Flag tests in 939 = MAC/status of the op exactly 4 cycles earlier.
- wipeout_override.cpp: table g_ndProgs {id, entry pc, resume pc, kind}; NdRun -> NdSubmit (STQ/RGBA/XYZ
  packets become RNVVERTX native vertex packets, others such as the particle sprites go as GIF packets).
  RT_NATIVEDRAW=0 all off; RT_NATIVEDRAW_MASK=<hex> picks table rows. Old NativeStrip() removed.
- RT_NATIVECHECK=1: runs native and VU for every batch of these programs and compares byte for byte, plus the
  written-back registers. Result over a Florion race: 0 differences in all five (47M + 7.8M + 7.5M + 6.7M
  vertices, 9.2M particle corners). Only clipped-strip batches needing the VU's clipper fall back (~8%).
- RT_NATIVETIME=1 prints TSC ticks per batch (native, or with RT_NATIVEDRAW=0 the VU incl. its GIF kick):
  clip-strip 2.5K vs 9.8K, lit-clip 6.2K vs 9.8K, strip 1.7K vs 8.2K, lit-strip 3.9K vs 9.7K,
  particles 52K vs 186K. Census: 70% of PATH1 vertices now arrive as native vertex packets.
- Left on the VU: g1920694623 (5.9%), d1159117218 (4.7%), the clipper paths, setup entries.
- RT_NATIVEPRECISE is gone (positions now always come from the exact FTOI4 packet). Next: milestone 4 (batch per draw state) or port g1920694623 / d1159117218.

## Update 2026-10-21 14:20 UTC (round 19: phase 2 "PC port" steps 1-3)
- the tester: "go for those three" = direct GPU path, native data path, PC formats. Plan in the renderer doc.
- Step 1 (gs_frontend.cpp, gs_backend.h, rufftrigger_gpu.cpp, GsGpu.*): RNVVERTX packets of triangles/sprites
  are collected per packet (vertexKick logic inline, no per-primitive submit) and handed to the new
  GSRasterBackend::SubmitList (default = one Submit each). GpuBackend::SubmitList converts the state once
  and calls the new Recorder::triangles bulk insert. RT_GSLIST=0 = old path; RT_GSLISTCHECK=1 sends each
  list both ways and compares recorder vertices: 6.2M lists, 0 differ. GS-side samples -21%.
- Step 2 (ps2_vif1_interpreter.cpp unpackMaskedFast): SSE4.1 select path for wl=1 V2/V3/V4-32 and V4-8
  with row/col/keep masks (the game's 4 vertex stream formats, ~4 vectors per UNPACK). RT_VIFCHECK=1:
  54M unpacks, 0 mismatches. RT_VIFTIME 28.1 -> 22.0 ns/vector (incl. timer overhead). Remaining cost
  is per-VIF-command overhead (690K UNPACKs/s); a deeper "read EE memory directly" path was judged not
  worth it (VU memory must stay coherent for VU-run programs).
- Step 3 (GsGpu.cpp, gl_api.h + GenerateMipmap): texture packs. Name = hash of decoded level-0 RGBA
  (%016llx.png). RT_TEXDUMP=dir dumps (1701 textures in one Florion race). RT_TEXPACK=dir (default
  ./textures = game folder) replaces at any size: one shared GL texture per replacement (texPackGl),
  GenerateMipmap, LINEAR filtering, shader lod bias uLod.z = log2(scale), max level = its mips.
  Tested with a 4x red/blue-swapped pack of all race textures: correct mapping, normal speed.
- Patches regenerated: /home/claude/wo/mkrtpatch.sh (new) rebuilds rt-wipeout.patch; mkpatch.sh now
  also lists gs_backend.h. README in the tarball documents texture packs.

## Update 2026-10-21 16:00 UTC (round 20: last VU1 programs native, draw merging)
- Rebuild in a fresh container (~12 min on 4 cores) as in round 12; uploads now sit in /mnt/project-files/uploads/hearth/.
  The game folder recipe and test harness are saved in recomp/wipeout/devtools/ (prep_game.sh, build.sh, game.sh,
  press.sh, shot.sh, race.sh, wait.sh, mkrtpatch.sh, package.sh; paths assume /home/claude/wo). race.sh LOG SECS [env..]
  boots on Xvfb :$DISP, presses Start, Cross, Start, Cross x7 (8 s apart) to reach the Florion arcade race, waits
  SECS, screenshots LOG.png. Foreground `sleep` is blocked here: use the scripts or background runs.
- vu1_native_draw.cpp: three more native programs, on by default (g_ndProgs kinds 5-7):
  * g1920694623 @0x3058 Sprite: one sprite per batch (centre TOP+1, colour TOP+2, UV rect TOP+3, size VU mem
    0xa3 * centre.w). Culled when THIS batch's CLIPw has any flag (FCAND sees the new flags; verified over 82K
    batches). Output TOP+0x96..0x9c. The game clamps only the second corner to 4095 (both MINIi are on vf23).
  * g1920694623 @0x31a0 Passthrough: XGKICK of the EE-built packet at TOP as is (~5% of race vertices).
  * d1159117218 @0x380/0x5d8 EnvStrip: environment-mapped clipped strip (stride 4: XYZ RGBA STQ normal). Pass 1 is
    software pipelined and writes T = ((L.n).xy + 1 + vf08.xy) * vf08.z over STQ.xy, plus the incoming vf30 at
    TOP-5 and the incoming vf21's T at TOP-1. Pass 2 = ClippedStrip rules; output at TOP+0x98; VU mem 0xa8 =
    header with y from mem[24 + header.w]. Clipper batches fall back (~0.01%).
- WriteBack has whole-register masks (vfMask/viMask), ACC and Q; the new kinds write back every register they
  change (vf28.w is shared with other programs). RT_NATIVECHECK also compares those, CLIP, I, Q, ACC and (kinds
  >= 4) all 16 KB of VU memory. Florion race: 0 differences in all 8 rows (sprite 215K, kick 2.5M, env-strip 7.1M
  vertices; others as before). Particles: wb.i only when a sprite was drawn (the new check found batches where the
  VU never ran LOI).
- RT_NATIVETIME per batch, native vs VU: sprite 1.0K vs 2.9K ticks, env-strip 3.9K vs 6.2K.
- Draw merging (rufftrigger_gpu.cpp convert): gsgpu::State no longer carries prim type and IIP (the renderer never
  read them; flat colour is already in the vertices). RT_RUNSTATS=1 (GsGpu.cpp) showed 99.8% of draw splits were
  only those fields. Now 0.3% of submits start a new draw instead of 7.3%; [fps] draws per present 10,300 -> 4,100.
  RT_GSMERGE=0 restores the old split. llvmpipe here is fill-bound, so no fps change is measurable locally.
  EdgeHudMap groups triangles per draw: the 16:9 race HUD looked the same in a side-by-side check.
- Still on the VU: g3841391935 entries 0x1268 (1.2%) and 0x10c0 (0.3%), g1920671007 @0x2268 (0.3%), and the
  clipper fallbacks of 0x1410 (4.6% of race vertices). Next candidates: the guard-band clipper (L1fa8/L1628/L20f8,
  shared by 3841 and 1159) and those entries.

## Update 2026-10-21 19:40 UTC (round 23: full rewrite phase 1 + packet cache)
- the tester chose "Go for the full rewrite" (native data path + sound). Phase 1 = map the mesh submission:
  RT_DMASITES=1 (wipeout_override.cpp) hooks MMIO CHCR stores (g_storeHook) and sceDmaSend/N (0x216ed0/0x216fa8)
  and walks each kicked chain: tags by id, REF sources, VIF commands (UNPACK formats, DIRECT, MPG, MSCAL entries),
  printed every 10 s. RT_DMADUMP_PC=<hex pc> RT_DMADUMP_AT=<s> writes one chain as text to dmadump.txt (game dir).
- Findings: one VIF1 chain per frame, kicked at 0x1edcd8 (inside sub_001EC8E8). Per object: an EE-built CNT block
  (STROW/STCOL, matrices/lights via UNPACK V4-32, MSCAL setup entry such as 0xef0) then a CALL into the model's
  prebuilt packet in RAM (0xB00000-0x1E0000 region), terminated by RET: FLUSH + DIRECT 7 (GIF state), then per
  strip STMASK (0x40404040 / 0x90909090: w from ROW, z/w from ROW/COL), UNPACK V4-32 header (+TOPS), V4-8 colour,
  V2-32 STQ, V3-32 position, V3-32 normal, MSCNT; STCYCL 0x104/0x103 (stride 4). One sampled frame: 515 calls of
  416 distinct packets, ~35K of ~35.7K position vertices from those packets. GIF chain at 0x18bc84; 2D/HUD VIF
  chain (DIRECT only) at 0x1e2bc8; MPG upload at 0x1fc204 (once).
- Packet cache (ps2_vif1_interpreter.cpp, ps2_memory.cpp walkVif1Chain, ps2_memory.h): RET/REF/REFS/REFE tag data
  from RAM >= 16 qw is compiled the second time it is seen with the same bytes: the interpreter runs it twice
  against probe VU memories filled 0 / ~0 (m_vif1Rec recorder mode: callbacks and GIF submits become events,
  MPG/MARK/MSKPATH3/STMOD!=0/DIRECTHL/irq/carry make it uncacheable); lanes equal in both runs are the writes of
  the segment before each event. Replay = memcpy runs + masked stores, GIF submits, MSCAL/MSCNT callbacks with
  TOP/ITOP/TOPS/DBF as recorded, final VIF regs. Key: address + 64-bit hash of tte+payload + entry VIF state.
  Slots whose hash changes 16 times without a replay are marked dynamic. RT_VIFCACHE=0 off, RT_VIFCACHECHECK=1
  (interpreter on a shadow VU memory alongside each replay, compares memory at every event, events, final regs):
  459K + 164K replays, 0 differences. RT_VIFCACHESTATS=1, RT_VIFCACHETIME=1 (ticks per replay: hash ~4.7K,
  writes ~4.5K for ~5 KB, memory bound here). Game thread VIF/DMA share 10.4% -> 7.6%.
- gdb stack sampling (t/pmp.sh, t/pmprun.sh): the GameThread spends ~half its time in gsgpu::Recorder::throttle,
  i.e. waiting for the llvmpipe render thread; the profiler's "?outside" libc share is that wait, not overhead.
- Sound census (IOP_SPU2STAT=1, iop_spu2.cpp): races use one AutoDMA PCM stream (CSS's software mix) plus up to
  ~6 hardware voices; ATTR effect bit off, EVOL 0, VMIXEL/R 0, so reverb (not emulated) is unused. Sound is already
  native: CSS/LIBSD AOT-translated, SPU2 voices/mix in C++ (~1%).
- Decision card pending: GPU-side transform (non bit-exact) vs stay on CPU vs both switchable (recommended).

## Update 2026-10-21 21:05 UTC (round 24: GPU model transform, build 3)
- Working on the recommended "both, switchable" option while the decision card is open.
- GPU model path: a native VU1 batch that comes from a cached packet (ps2x_capture::g_vif1BatchId =
  serial<<16 | event+1) is sent as an RNVMODEL packet (GSModelPacket in gs_backend.h: raw input vertices, vf1..4
  screen matrix, vf13..16 clip matrix, vf5..7 lights, vf8 ambient, VU mem 8..11 light colours) instead of being run
  by NdRun (NdModel in wipeout_override.cpp). gs_frontend tryProcessNativeModelPacket applies PRE/PRIM and calls
  backend->SubmitModel; rufftrigger_gpu.cpp records Op::Model; GsGpu.cpp keeps meshes in a 64 MB arena VBO keyed by
  batch id (resent when the generation changes) and draws with a model vertex shader that does the VU maths
  (FTOI4 truncation, z per ZPSM, lighting, clip distances from the clip matrix, depth clamp, flat/smooth).
  Varyings are multiplied by w and divided back with gl_FragCoord.w: GS screen-linear interpolation that survives
  GPU clipping (noperspective varyings gave white wedges near the camera).
- Kinds on the GPU: 0 clipped strip, 1 lit clipped strip, 2 plain strip, 3 lit strip, 8 stride-4 clipped strip
  (RT_GPUMODELKINDS hex mask, default 0x10F). Kind 3 only when vf5..7.w == 0 (the carried vf26*l.w term vanishes);
  vf26 is written back as the last vertex's light. Still on CPU: 7 env-mapped strip (register/memory
  writebacks), particles, sprites, anything outside a cached packet.
- Switch: F10 "Model transform" (GPU / CPU PS2-exact), graphics.ini gputransform=0/1 -> RT_GPUMODELS.
  g_gsModelCapable (renderer can) vs g_gsModelSupport (in use); live toggle, from the next model.
- RT_GPUCENSUS=1: batches/CPU vertices by kind, inside/outside cached packets (GPU batches count 0 CPU vertices).
  Debug: RT_GPUMODELDBG, RT_GPUMODELPLANES (hex), RT_GPUMODELZCLAMP, RT_GPUMODELNOCLIP.
- Patches: wipeout-runtime-r23.patch = runtime changes since round 22 (cache, model packet, frontend, SPU2 stats);
  rt-wipeout.patch regenerated (GsGpu model renderer, gpu backend).

## Update 2026-10-22 02:15 UTC (round 25: GPU transform coverage, batched model draws, build 4)
- Fresh container rebuild as in round 12/20 (~15 min). setup.sh now applies wipeout-runtime.patch, then -r21, -r23, -r25
  (the earlier setup.sh only applied the first one). The game folder recipe is devtools/prep_game.sh; drive.sh gained
  PRE (s before holding accelerate, default 10) and GAP (s between shots): `PRE=55 GAP=8 drive.sh LOG 6` gets a moving race.
- Env-mapped strips (kind 7, d1159117218) on the GPU: GSModelEnv (8) in GSModelPacket.flags / ModelDraw flag 16; the model
  shader's uMf.x == 2 computes STQ.xy = ((uL * n).xy + 1 + uAmb.xy) * uAmb.z (uL = vf5..7, uAmb = vf08). The CPU side
  runs vu1native::EnvStripTail: the register writeback and the stores at 0xa8, TOP-5, TOP-1 from the last vertices only.
  RT_ENVTAILCHECK=1 runs the full EnvStrip on a copy and compares: 1.05M batches, 0 differences (registers incl. vi11, memory
  outside the input STQs and the output block).
- Batches outside cached packets (k0 "other", ~7% of clipped-strip batches) now go to the GPU too, as transient meshes: key bit 63
  + serial, sent with every draw (rufftrigger_gpu.cpp skips meshSent for them), stored in an 8 MB ring at the end of the 64 MB
  mesh buffer (kTransientBytes per replay at most). RT_GPUMODELTRANSIENT=0 keeps them on the CPU. RT_GPUCENSUS: all strip kinds
  0 CPU vertices now. Left on the CPU: particles (k4, ~24K ticks/batch, ~2% of the game thread), sprites (k5, 0.5K ticks),
  the passthrough kick (k6, a copy), setup entries.
- Found: build 3's GPU path issued one GL draw per model batch (47K draws per present vs 2.8K in CPU mode; per-draw uniforms
  and attribute setup). Now batched (GsGpu.cpp, needs GL 4.3, RT_GPUMODELMDI=0 = old path): replay() first uploads every mesh
  of the batch and the per-model parameters into a buffer texture (kMdTexels = 20 RGBA32F per model: m, cm, l, amb, lc, ofs,
  mf); a run of consecutive Op::Model commands with the same state index, stride and shading is one MultiDrawElementsIndirect
  with baseInstance = model index (instanced int attribute 4 from an iota buffer, MDI shader variant reads uMd). Strips and
  fans are drawn as GL_TRIANGLES through fixed index patterns (list / strip (k,k+1,k+2) / fan (0,k+1,k+2) for up to 1024
  vertices, baseVertex = mesh offset / stride; mesh offsets are now multiples of 192 bytes). Manual-blend runs draw per
  triangle with DrawArraysInstancedBaseInstance. Draws per present 47K -> 6.5K; RT_MDISTATS=1: 9.4 models per run, runs end
  almost only at a non-model command in between.
- llvmpipe here does the GPU transform on the same 4 CPU cores, so local fps cannot rank GPU vs CPU mode (CPU mode is faster
  here: 70% vs ~50% speed); on a real GPU the draw count was the problem. Screenshots of a moving race look right.
- Patches: wipeout-runtime-r25.patch (gs_backend.h: GSModelEnv), rt-wipeout.patch regenerated (GsGpu.*, gl_api.h,
  rufftrigger_gpu.cpp). Build 4 shipped under the same two file names.

## Update 2026-10-22 04:00 UTC (round 26: build 5, batched-draw crash, lock-on reticle)
- the tester: Build 4 crashed at race start; RT_GPUMODELMDI=0 avoided it (so the multi-draw indirect path).
  Found: the transient mesh ring started at kCachedBytes = 56 MB, not a multiple of 48, so every stride-3
  transient mesh (hundreds of thousands per race) got a baseVertex 32 bytes off (garbage vertices). Fixed
  (GsGpu.cpp: ring start rounded down to 192; mdFirst rejects offsets that are not whole vertices). Not
  confirmed on the tester's GPU yet: ask whether Build 5 still crashes without RT_GPUMODELMDI=0, and for the
  "[gsgpu] OpenGL" line and last lines of the terminal if so.
- Lock-on reticle (the tester: off centre in 16:9, slows the game, no sound when fully locked):
  * 0x18C058 draws the three arcs of DATA/HUD/LOCKON_CIRCLE.HUD round (a0, a1) = target centre (12.4 px);
    f13 = lock progress (1 -> 0 locked), f12 = rotation. Caller 0x18C470 (per target slot: state at
    obj+0xCF8, 60-frame counter at obj+0xD04) is driven by 0x18D500 (targets from ship+0xCEC).
  * The edge-HUD layout took the arcs (untextured, flat) for HUD panels and moved each to a screen edge,
    and set hudSeen early so later flat textured draws were edge-mapped too. Now ReticleWrap
    (wipeout_settings.cpp) writes A+D packets to "GS register" 0x7E (bit 0 on, centre x in bits 16-31)
    before and after the call into the player's HUD packet (write pointer 0x358C50 + 0x34*player + 0x10,
    as 0x184C18 uses it). Runtime GS keeps it as GSDrawState::portMark (wipeout-runtime-r26.patch),
    rufftrigger_gpu maps it to State::notHud/notHudX, GsGpu squashes those draws about their own centre.
  * Sound: 0x18D790 each frame sends sound 0x72 (acquiring) / 0x73 (locked) on or off from the flags at
    gp-0x3F2C / gp-0x3F28. Locally (IOP_KONLOG=1) the locked tone (SPU 0x31B00, looped) keys on at lock and
    plays at full level (peak ~28K, mixed) until the lock is lost. Asked the tester to recheck in Build 5.
  * Slowdown: reticle code is ~150 us/call here; its GL draws are plain blends. Likely the early hudSeen
    (per-draw vertex re-uploads); unconfirmed.
- Debug switches added: RT_GIVEWEAPON=<id> (refill every ship's weapon; with PRE=0 drive.sh a lock-on
  happens in the first seconds), RT_RETLOG=1 (reticle calls, progress, packet pointer, time), RT_RAMDUMP=s,...
  (EE RAM to ramdump_<s>.bin), IOP_KONLOG=1 (SPU2 key on/off with SPU sample clock, sample flags, voice
  peak output). t/dis.sh ADDR prints a function's MIPS from recomp-out comments (devtools/dis.sh).
- In-game Graphics page (paused): Options item table 0x2457A0 {textId, flag}; item 93 = "screen alignment"
  (screen id 0x64, object 0x28D610, manager+0x4B8). Plan: second prefs-class object (ctor 0x13BDD0, init like
  0x147B00) at free RAM 0x40000 (0x13000-0x76000 never written), registered as screen 0x64, with our rows.
  Option widget: choices at +0x48 + 12*i {textId,..}, count +0x9C, value +0xD8; label id +0x88.
- Patches: wipeout-runtime-r26.patch (runtime GS marker register, SPU2 key-on log), setup.sh applies it.

## Update 2026-10-22 04:35 UTC (build 6: newer GL context)
- the tester's log (RTX 4090, NVIDIA 615.78): raylib's 3.3 core request gets exactly "3.3.0 NVIDIA", so the
  batched (MDI, GL 4.3) path never ran there: the Build 4 race-start crash was not the MDI path (cause still
  unknown; it did not recur in Build 5). the tester also runs at internal scale 8x (window on a 3840x2160 display).
- Build 6: the link wraps glfwCreateWindow (-Wl,--wrap, ps2xRuntime/CMakeLists.txt in r26) and
  __wrap_glfwCreateWindow (rufftrigger_gpu.cpp) asks for 4.6, then 4.3, then raylib's 3.3. RT_GL33=1 keeps
  3.3. MDI on NVIDIA is untested: RT_GPUMODELMDI=0 is the fallback.
- Lock-on "acquiring" sound silent in the tester's second race: not reproduced. With t/race2.sh (pause, restart
  race: keys 8 40 4000 80 4000) the acquiring tone (SPU 0x31920) keys on in the restarted race too.
- Smoke slowdown: waiting for RT_FPSLOG=1 lines (env var must precede the AppImage path).

## Update 2026-10-22 06:00 UTC (build 7: in-game Graphics page)
- Options menu: "screen alignment" is now "graphics" and opens a Graphics page in the game's own style
  (screenshot graphics-page-build7.png): resolution, display, picture shape, race hud (16:9), field of view,
  vsync, show fps, fast loading, model transform. Left/right change a row live and save graphics.ini (same
  Change() as the F10 menu, run on the render thread via a queue). Accept and back both leave the page.
  The F10 overlay stays for the fun/debug options. RT_NOGFXPAGE=1 restores the original screen.
- How (wipeout_settings.cpp, "In-game Graphics page"):
  * A second Preferences-class object at 0x40000 (ctor 0x13BDD0, set-up 0x147B00(obj, s2 of 0x1398D0)),
    registered by RegisterScreenWrap (0x1397E8, id 0x64): the screen table entry is
    R32(manager+4) + id*12 + 8; 0x1397E8 itself only sets input flags.
  * Guest calls from hooks must not be native nested calls: an EE checkpoint inside them unwinds the
    stack and the rest runs later from the scheduler, so the hook "returns" early (the first try left the
    page half built). StartSeq/CallOrigThen run calls in continuation style: ra = kStubSeq (0x1398CC, padding
    after 0x1397E8's jr ra) whose handler starts the next call or the final continuation.
  * Text: 0x185740 replaced in C (TextLookup). Ids 0x7000+ are strings at 0x41000 (0x40 bytes each);
    id 93 maps to "graphics". Row label = text element at widget+0x84, its id halfword at +0x88 (negative =
    inline string at +0x38); choice values = widget+0x48+12*i (count +0x9C, index +0x94/+0xD8; drawn by
    0x12CF58 every frame); page title = obj+0x1C. The game font has a-z, 0-9 and " !$%&'(),-./:?@_" only.
  * Rows rest on index 1 with every choice showing the value text; a press moves the index (right counts
    down) and PageUpdateWrap (0x148580, before the original) turns it into Change(row, +-1). Row 0x94C rests
    on 0 (the game forces it to 0 when 0x2DB6E4 == 0). Sliders hidden with widget+0x94 = 0 (alpha).
    DBC is kept equal to the 0x26C row so the page never changes sound quality.
  * Up/down (0x148920/0x148998, repeats 0x148A10/0x148AA0): cursor kept to rows 2-10; the autosave pref
    0x39662C is set to 0 around the call so row 6 is reachable. Accept 0x148C48 / back 0x148B30 replaced
    for this page: sound 0x205090(0x48 / 0x42), then 0x140108(0x3A / 0x3B, 0).
- Test helper: devtools/menu2.sh gained tN = short tap (0.3 s) of pad bits N; back is triangle (1000).
  Path to the page: "4000 8 w6 10 w2 4000 w5 40 40 4000 w4". RT_PAGELOG=1 logs row presses and moves.
