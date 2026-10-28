#!/usr/bin/env python3
"""Build the native PC port of WipEout Fusion (NTSC-U, SLUS-204.62) from your own disc image.

  python3 build.py "/path/to/WipEout Fusion.iso"                    build for this machine
  python3 build.py "/path/to/WipEout Fusion.iso" --target windows   Windows .exe (from Linux/WSL)
  python3 build.py --game-dir /path/to/extracted/disc               use an already extracted disc

Everything goes into ./build (or --build-dir). Steps, each skipped when already done:
  1. extract the disc into build/game (about 2.4 GB)
  2. fetch PS2Recomp at the pinned commit and apply ps2recomp/patches/*.patch
  3. build the recompiler and translate SLUS_204.62 into C++
  4. rebuild the VU1 snapshots from SLUS_204.62 and translate the VU1 microcode
  5. translate the game's IOP modules (sound, files, pads, memory card, CD) into C++
  6. build a small static FFmpeg (only the video decoders the game needs)
  7. compile the game into dist/<target>/
The disc, the extracted files and everything generated from them stay on your machine.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG_TAG = 'n7.1'
ELF = 'SLUS_204.62'


def step(msg):
    print(f'\n==> {msg}', flush=True)


def run(cmd, cwd=None, env=None, log=None):
    print('   ', ' '.join(str(c) for c in cmd) if len(cmd) < 12 else f'{cmd[0]} ... ({len(cmd)} args)', flush=True)
    if log:
        with open(log, 'w') as f:
            r = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
        if r.returncode:
            with open(log, errors='replace') as f:
                print(''.join(f.readlines()[-30:]))
            sys.exit(f'failed (full log: {log})')
    else:
        r = subprocess.run(cmd, cwd=cwd, env=env)
        if r.returncode:
            sys.exit(f'failed: {cmd[0]} exited with {r.returncode}')


def need(*tools):
    missing = [t for t in tools if not shutil.which(t)]
    if missing:
        sys.exit('missing build tools: ' + ', '.join(missing) + '\nSee README.md, "Build requirements".')


def host_target():
    s = platform.system()
    if s == 'Windows' or s.startswith(('MINGW', 'MSYS')):
        return 'windows'
    if s == 'Darwin':
        return 'macos'
    return 'linux'


def is_game_dir(d):
    return os.path.isfile(os.path.join(d, ELF)) and os.path.isfile(os.path.join(d, 'WOF', 'FILESYS.WAD'))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def prepare_game(args, B):
    game = os.path.join(B, 'game')
    if args.game_dir:
        game = os.path.abspath(args.game_dir)
        if not is_game_dir(game):
            sys.exit(f'{game} has no {ELF} and WOF/FILESYS.WAD')
    elif not is_game_dir(game):
        if not args.iso or not os.path.isfile(args.iso):
            sys.exit('pass your WipEout Fusion (NTSC-U) disc image: python3 build.py "/path/to/WipEout Fusion.iso"')
        step('extracting the disc')
        tmp = game + '.tmp'
        shutil.rmtree(tmp, ignore_errors=True)
        run([sys.executable, os.path.join(HERE, 'tools', 'iso_extract.py'), args.iso, tmp],
            log=os.path.join(B, 'extract.log'))
        if not is_game_dir(tmp):
            sys.exit(f'{ELF} or WOF/FILESYS.WAD not found on the disc: this port needs the NTSC-U release (SLUS-20462).')
        shutil.rmtree(game, ignore_errors=True)
        os.replace(tmp, game)
    want = json.load(open(os.path.join(HERE, 'recomp', 'vu1dumps.json')))['elf_sha256']
    if sha256(os.path.join(game, ELF)) != want:
        sys.exit(f'{game}/{ELF} differs from the NTSC-U release this port was made for (SHA-256 {want}).')
    return game


def fetch_ps2recomp(B):
    P = os.path.join(B, 'PS2Recomp')
    commit = open(os.path.join(HERE, 'ps2recomp', 'COMMIT')).read().strip()
    patches = sorted(os.listdir(os.path.join(HERE, 'ps2recomp', 'patches')))
    stamp = os.path.join(P, '.wof-patched')
    want = commit + '\n' + '\n'.join(f'{p} {sha256(os.path.join(HERE, "ps2recomp", "patches", p))}' for p in patches)
    if os.path.isfile(stamp) and open(stamp).read() == want:
        return P
    step('fetching PS2Recomp')
    if not os.path.isdir(os.path.join(P, '.git')):
        run(['git', 'clone', 'https://github.com/ran-j/PS2Recomp', P])
    run(['git', '-C', P, 'fetch', '-q', 'origin', commit])
    run(['git', '-C', P, 'checkout', '-q', '-f', commit])
    run(['git', '-C', P, 'clean', '-q', '-fdx', '-e', 'out'])
    for p in patches:
        run(['git', '-C', P, 'apply', '--whitespace=nowarn', os.path.join(HERE, 'ps2recomp', 'patches', p)])
    for f in ('.wof-recomp.done',):
        if os.path.exists(os.path.join(B, f)):
            os.remove(os.path.join(B, f))
    open(stamp, 'w').write(want)
    return P


def cmake_build(src, bdir, target, defs, jobs, env=None, log=None):
    cmd = ['cmake', '-S', src, '-B', bdir, '-G', 'Ninja'] + [f'-D{k}={v}' for k, v in defs.items()]
    run(cmd, env=env, log=log and log + '.configure')
    run(['cmake', '--build', bdir, '--target', target, '-j', str(jobs)], env=env, log=log)


def translate(args, B, P, game):
    step('building the recompiler')
    host = os.path.join(P, 'out', 'host')
    cmake_build(P, host, 'ps2_recomp', {
        'CMAKE_BUILD_TYPE': 'Release', 'PS2X_BUILD_STUDIO': 'OFF', 'PS2X_BUILD_TEST': 'OFF',
        'PS2X_BUILD_RUNTIME': 'OFF', 'PS2X_BUILD_ANALYZER': 'OFF'}, args.jobs, log=os.path.join(B, 'recompiler.log'))
    exe = os.path.join(host, 'ps2xRecomp', 'ps2_recomp' + ('.exe' if host_target() == 'windows' else ''))

    done = os.path.join(B, '.wof-recomp.done')
    if not os.path.isfile(done):
        step('translating the game executable (SLUS_204.62 -> C++)')
        out = os.path.join(B, 'recomp-out')
        shutil.rmtree(out, ignore_errors=True)
        cfg = []
        for line in open(os.path.join(HERE, 'recomp', 'config.toml')):
            if line.startswith('input = '):
                line = 'input = ' + json.dumps(os.path.join(game, ELF).replace('\\', '/')) + '\n'
            elif line.startswith('output = '):
                line = 'output = ' + json.dumps(out.replace('\\', '/')) + '\n'
            cfg.append(line)
        open(os.path.join(B, 'config.toml'), 'w').writelines(cfg)
        run([exe, os.path.join(B, 'config.toml')], log=os.path.join(B, 'recomp.log'))
        runner = os.path.join(P, 'ps2xRuntime', 'src', 'runner')
        for f in os.listdir(runner):
            if f.endswith('.cpp'):
                os.remove(os.path.join(runner, f))
        for f in os.listdir(out):
            if f.endswith('.cpp'):
                shutil.copy(os.path.join(out, f), runner)
            elif f.startswith('ps2_recompiled_') and f.endswith('.h'):
                shutil.copy(os.path.join(out, f), os.path.join(P, 'ps2xRuntime', 'include'))
        open(done, 'w').write('ok\n')

    step('translating the VU1 microcode')
    dumps = os.path.join(B, 'vu1dumps')
    vu = os.path.join(B, 'vu1gen')
    shutil.rmtree(vu, ignore_errors=True)
    os.makedirs(vu)
    run([sys.executable, os.path.join(HERE, 'tools', 'vu1dumps.py'), 'build', os.path.join(game, ELF),
         os.path.join(HERE, 'recomp', 'vu1dumps.json'), dumps])
    env = dict(os.environ, VURECOMP_ENTRIES=os.path.join(HERE, 'recomp', 'vu1entries.txt'))
    run([sys.executable, os.path.join(HERE, 'tools', 'vurecomp.py'), os.path.join(game, ELF), vu, dumps],
        env=env, log=os.path.join(B, 'vurecomp.log'))

    step('translating the IOP modules (sound, files, pads, memory card, CD)')
    irx = os.path.join(game, 'WOF', 'IRX')
    mods = {'css': 'CSS', 'mcd': 'MCD', 'libsd': 'LIBSD', 'padman': 'PADMAN', 'sio2man': 'SIO2MAN',
            'mtapman': 'MTAPMAN', 'cdvdman': 'CDVDMAN', 'mcman': 'MCMAN', 'mcserv': 'MCSERV'}
    run([sys.executable, os.path.join(HERE, 'tools', 'iopxlat.py'),
         os.path.join(P, 'ps2xIOP', 'src', 'emulator', 'native', 'iop_aot_modules.cpp')] +
        [f'{k}={os.path.join(irx, v + ".IRX")}' for k, v in mods.items()], log=os.path.join(B, 'iopxlat.log'))

    dst = add_port_sources(P)
    for f in os.listdir(vu):
        shutil.copy(os.path.join(vu, f), dst)


def add_port_sources(P):
    step("adding the port's sources")
    dst = os.path.join(P, 'ps2xRuntime', 'src', 'game')
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    for d in ('runtime', os.path.join('runtime', 'render'), os.path.join('runtime', 'vu1'), 'game'):
        for f in os.listdir(os.path.join(HERE, d)):
            if f.endswith(('.cpp', '.h')):
                shutil.copy(os.path.join(HERE, d, f), dst)
    return dst


def build_ffmpeg(args, B, target, march):
    """A static FFmpeg with just the IPU and MPEG-1/2 video decoders (the game's movies)."""
    prefix = os.path.join(B, 'deps', f'ffmpeg-{target}')
    if os.path.isfile(os.path.join(prefix, 'lib', 'pkgconfig', 'libavcodec.pc')):
        return prefix
    step(f'building FFmpeg {FFMPEG_TAG} (static, video decoders only) for {target}')
    need('make')
    src = os.path.join(B, 'deps', 'ffmpeg-src')
    if not os.path.isdir(src):
        run(['git', 'clone', '-q', '--depth', '1', '-b', FFMPEG_TAG, 'https://github.com/FFmpeg/FFmpeg', src])
    bdir = os.path.join(B, 'deps', f'ffmpeg-build-{target}')
    shutil.rmtree(bdir, ignore_errors=True)
    os.makedirs(bdir)
    conf = ['sh', os.path.join(src, 'configure').replace('\\', '/'), f'--prefix={prefix}'.replace('\\', '/'),
            '--disable-programs', '--disable-doc', '--disable-everything', '--disable-network',
            '--disable-autodetect', '--disable-x86asm', '--enable-static', '--disable-shared',
            '--enable-decoder=ipu,mpeg2video,mpeg1video', '--enable-parser=mpegvideo',
            '--enable-swscale', '--enable-swresample', '--enable-avformat', '--pkg-config-flags=--static',
            f'--extra-cflags={march}']
    if target == 'windows' and host_target() != 'windows':
        conf += ['--enable-cross-compile', '--target-os=mingw32', '--arch=x86_64',
                 '--cross-prefix=x86_64-w64-mingw32-']
    if target == 'macos':
        conf += ['--arch=x86_64', '--cc=clang -arch x86_64', '--enable-cross-compile', '--target-os=darwin']
    run(conf, cwd=bdir, log=os.path.join(B, f'ffmpeg-{target}-configure.log'))
    run(['make', f'-j{args.jobs}'], cwd=bdir, log=os.path.join(B, f'ffmpeg-{target}-make.log'))
    run(['make', 'install'], cwd=bdir, log=os.path.join(B, f'ffmpeg-{target}-install.log'))
    return prefix


def game_config(args, B, target):
    """CMake definitions and environment for compiling the game (shared by the real build and --check)."""
    # Local builds are tuned to this CPU. Cross builds get x86-64-v3 (AVX2, any PC from about 2015 on),
    # macOS x86-64-v2 (Apple Silicon runs the x86-64 build under Rosetta 2).
    march = args.arch
    if not march:
        march = '-march=x86-64-v2' if target == 'macos' else '-march=native' if target == host_target() else '-march=x86-64-v3'
    env = dict(os.environ)
    defs = {
        'CMAKE_BUILD_TYPE': 'RelWithDebInfo', 'CMAKE_CXX_FLAGS': march, 'CMAKE_C_FLAGS': march,
        'CMAKE_CXX_FLAGS_RELWITHDEBINFO': '-O1 -DNDEBUG', 'PS2X_BUILD_RECOMP': 'OFF', 'PS2X_BUILD_ANALYZER': 'OFF',
        'PS2X_BUILD_STUDIO': 'OFF', 'PS2X_BUILD_TEST': 'OFF', 'PS2X_ENABLE_AGRESSIVE_LOGS': 'OFF',
        'PS2X_ENABLE_DEBUG_UI': 'OFF', 'PS2X_ENABLE_SCCACHE': 'OFF'}
    if not args.system_ffmpeg:
        ff = build_ffmpeg(args, B, target, march)
        env['PKG_CONFIG_LIBDIR'] = os.path.join(ff, 'lib', 'pkgconfig')
        env['PKG_CONFIG_PATH'] = ''
        defs['PS2X_WIN_DEPS_PREFIX'] = ff.replace('\\', '/')
    if target == 'windows' and host_target() != 'windows':
        defs['CMAKE_TOOLCHAIN_FILE'] = os.path.join(HERE, 'cmake', 'mingw-w64-x86_64.cmake')
    if target == 'macos':
        defs['CMAKE_OSX_ARCHITECTURES'] = 'x86_64'   # the runtime uses SSE; Apple Silicon runs it under Rosetta 2
        defs['CMAKE_OSX_DEPLOYMENT_TARGET'] = '11.0'
    return defs, env


def compile_game(args, B, P, target):
    defs, env = game_config(args, B, target)
    step(f'compiling the game for {target} (the generated code is large: expect 15-60 minutes)')
    bdir = os.path.join(P, 'out', target)
    cmake_build(P, bdir, 'ps2EntryRunner', defs, args.jobs, env=env, log=os.path.join(B, f'compile-{target}.log'))

    exe = 'ps2EntryRunner' + ('.exe' if target == 'windows' else '')
    dist = os.path.join(HERE, 'dist', target)
    os.makedirs(dist, exist_ok=True)
    name = 'WipEoutFusion.exe' if target == 'windows' else 'wipeout-fusion'
    shutil.copy2(os.path.join(bdir, 'ps2xRuntime', exe), os.path.join(dist, name))
    strip = shutil.which('x86_64-w64-mingw32-strip') if target == 'windows' and host_target() != 'windows' else shutil.which('strip')
    if strip and not args.keep_symbols:
        run([strip] + (['-x'] if target == 'macos' else []) + [os.path.join(dist, name)])
    shutil.copy2(os.path.join(HERE, 'docs', 'PLAYING.txt'), dist)
    return os.path.join(dist, name)


def check(args, B):
    """Without a disc: patch PS2Recomp, then compile its runtime and every source file of this port.
    This is what CI runs; the game itself cannot be built without the disc."""
    P = fetch_ps2recomp(B)
    step('building the recompiler')
    cmake_build(P, os.path.join(P, 'out', 'host'), 'ps2_recomp', {
        'CMAKE_BUILD_TYPE': 'Release', 'PS2X_BUILD_STUDIO': 'OFF', 'PS2X_BUILD_TEST': 'OFF',
        'PS2X_BUILD_RUNTIME': 'OFF', 'PS2X_BUILD_ANALYZER': 'OFF'}, args.jobs, log=os.path.join(B, 'recompiler.log'))
    # The IOP module translation needs the disc; an empty file lets CMake configure.
    iop = os.path.join(P, 'ps2xIOP', 'src', 'emulator', 'native', 'iop_aot_modules.cpp')
    if not os.path.isfile(iop):
        open(iop, 'w').write('// placeholder for build.py --check: the real file is generated from the disc\n')
    add_port_sources(P)
    defs, env = game_config(args, B, args.target)
    bdir = os.path.join(P, 'out', args.target + '-check')
    step(f'compiling the runtime and the port sources for {args.target}')
    cmake_build(P, bdir, 'ps2_runtime', defs, args.jobs, env=env, log=os.path.join(B, f'check-{args.target}.log'))
    ninja = subprocess.run(['ninja', '-C', bdir, '-t', 'targets', 'all'], capture_output=True, text=True, check=True)
    objs = sorted({line.split(':')[0] for line in ninja.stdout.splitlines()
                   if '/src/game/' in line.split(':')[0] and line.split(':')[0].endswith(('.o', '.obj'))})
    if not objs:
        sys.exit('no port sources found in the build')
    run(['ninja', '-C', bdir, '-j', str(args.jobs)] + objs, env=env, log=os.path.join(B, f'check-{args.target}-port.log'))
    print(f'\nOK: the PS2Recomp runtime and {len(objs)} port sources compile for {args.target}.')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('iso', nargs='?', help='your WipEout Fusion NTSC-U disc image')
    ap.add_argument('--game-dir', help='an already extracted disc (folder with SLUS_204.62 and WOF/)')
    ap.add_argument('--target', choices=['linux', 'windows', 'macos'], default=host_target())
    ap.add_argument('--arch', default=None, help='compiler CPU flags, e.g. -march=x86-64-v2 for older CPUs')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4, help='parallel compile jobs (about 1.5 GB RAM each)')
    ap.add_argument('--build-dir', default=os.path.join(HERE, 'build'))
    ap.add_argument('--keep-symbols', action='store_true', help='do not strip the binary (for debugging crashes)')
    ap.add_argument('--system-ffmpeg', action='store_true', help="use the system's FFmpeg (pkg-config) instead of building one")
    ap.add_argument('--check', action='store_true', help='no disc needed: only check that everything compiles (used by CI)')
    args = ap.parse_args()

    need('git', 'cmake', 'ninja')
    if args.target == 'windows' and host_target() != 'windows':
        need('x86_64-w64-mingw32-g++-posix' if shutil.which('x86_64-w64-mingw32-g++-posix') else 'x86_64-w64-mingw32-g++')
    B = os.path.abspath(args.build_dir)
    os.makedirs(B, exist_ok=True)
    if args.check:
        check(args, B)
        return
    game = prepare_game(args, B)
    P = fetch_ps2recomp(B)
    translate(args, B, P, game)
    out = compile_game(args, B, P, args.target)

    print(f'\nDone: {out}')
    if args.target == host_target():
        print(f'Play: "{out}" "{game}"   (or put the game folder next to it as ./game)')
    else:
        print(f'Copy {os.path.dirname(out)} to the {args.target} machine and drop your .iso onto {os.path.basename(out)}.')


if __name__ == '__main__':
    main()
