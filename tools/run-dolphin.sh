#!/usr/bin/env bash
# Build GEMU for Wii and start it in Dolphin.
#
#   tools/run-dolphin.sh [options] [ROM...]
#
#   ROM...            copy these ROMs onto the emulated SD card first
#                     (sd/gemu/<machine>/, machine chosen by extension);
#                     everything in roms/ (or roms/<machine>/) is copied too
#   --no-build        don't run make
#   --gc              the GameCube build (gemu-gc.dol).  Dolphin has no GC SD
#                     adapter, so the staged SD tree's gemu/ is baked into the
#                     DOL instead (make EMBED=...); Dolphin runs it at the
#                     GameCube's clock, so speeds are the real GC's.
#   --hbc             boot the Homebrew Channel instead of gemu.dol, with GEMU
#                     on the SD card (sd/apps/gemu/).  The first time, your
#                     Dolphin NAND (which must have the HBC installed) is
#                     copied into .dolphin/Wii; your own NAND is not touched.
#   --autotest[=N]    unattended run: scripted input (tests/autotest_wii.c),
#                     no controllers, fixture ROMs from tests/roms/<machine>/; saves the
#                     last frame (and audio) to build-autotest/dolphin/. With N
#                     the script stops and the machine freezes at poll N.
#   --seconds=S       autotest run time (default 40)
#   --netplay=HOST:PORT  autotest with the netplay script (Wii): load the
#                     staged smb.nes, connect to that Kaillera server (put in
#                     netplay.ini as the recent server, name "wii"), create a
#                     game and start it a few seconds later - have a second
#                     player join meanwhile (tools/kaillera_test_server.py
#                     is a local server)
#
# Normal runs use a repo-local Dolphin profile (.dolphin/) whose controller
# mappings are copied from your own Dolphin config on every run, with the
# Wii Remote set to sideways; your own Dolphin settings are never changed.
# The emulated SD card is synced from sd/ (sd/apps/gemu/ is the packaged app).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOLPHIN=${DOLPHIN:-dolphin-emu}
USER_CFG=${XDG_CONFIG_HOME:-$HOME/.config}/dolphin-emu
USER_NAND=${XDG_DATA_HOME:-$HOME/.local/share}/dolphin-emu/Wii
HBC_TITLE=000100014C554C5A          # "LULZ", the Homebrew Channel
HBC_DIR=title/00010001/4c554c5a

build=1 autotest= seconds=40 hbc= gc= netplay= roms=()
for arg in "$@"; do
    case $arg in
        --no-build)    build= ;;
        --hbc)         hbc=1 ;;
        --gc)          gc=1 ;;
        --autotest)    autotest=1 ;;
        --autotest=*)  autotest=${arg#*=} ;;
        --seconds=*)   seconds=${arg#*=} ;;
        --netplay=*)   netplay=${arg#*=}; autotest=${autotest:-1} ;;
        -h|--help)     sed -n '2,30s/^# \{0,1\}//p' "$0"; exit 0 ;;
        -*)            echo "unknown option: $arg" >&2; exit 2 ;;
        *)             roms+=("$arg") ;;
    esac
done

command -v "$DOLPHIN" >/dev/null || { echo "$DOLPHIN not found (set DOLPHIN=...)" >&2; exit 1; }

machine_for() {   # ROM file -> machine folder name
    case ${1,,} in
        *.ch8|*.c8|*.chip8|*.8o) echo chip8 ;;
        *.a26) echo atari2600 ;;
        *.nes|*.nesstate) echo nes ;;
        *) return 1 ;;
    esac
}

# ── Build ────────────────────────────────────────────────────────────────────
if [ -z "${DEVKITPRO:-}" ] && [ -f /etc/profile.d/devkit-env.sh ]; then
    # shellcheck disable=SC1091
    source /etc/profile.d/devkit-env.sh
fi
sfx=${gc:+-gc}
[ -n "$gc" ] && [ -n "$hbc" ] && { echo "--hbc is Wii only" >&2; exit 2; }
if [ -n "$autotest" ]; then
    dol=$ROOT/gemu$sfx-autotest.dol profile=$ROOT/build$sfx-autotest/dolphin sd=$profile/sd
else
    dol=$ROOT/gemu$sfx.dol profile=$ROOT/.dolphin sd=$ROOT/sd
fi
build_dol() {   # GameCube: after staging, since the SD tree goes into the DOL
    [ -n "$build" ] || return 0
    local mk=(make -C "$ROOT" --no-print-directory)
    [ -n "$gc" ] && mk+=(PLATFORM=gc EMBED="$sd/gemu")
    [ -n "$netplay" ] && mk+=(AUTOTEST_NETPLAY=1)
    if [ -n "$autotest" ]; then
        rm -rf "$ROOT/build$sfx-autotest/core" "$ROOT/build$sfx-autotest/hardware" \
               "$ROOT/build$sfx-autotest/tests"   # AUTOTEST=N is baked in
        "${mk[@]}" AUTOTEST="$autotest"
    elif [ -n "$gc" ]; then
        "${mk[@]}"
    else
        "${mk[@]}" package
    fi
}
[ -z "$gc" ] && build_dol

# ── SD card ──────────────────────────────────────────────────────────────────
mkdir -p "$sd/apps/gemu" "$sd/gemu/chip8" "$sd/gemu/atari2600" "$sd/gemu/nes"
if [ -n "$autotest" ]; then              # fixtures: tests/roms/<machine>/
    rm -rf "$sd/gemu" && mkdir -p "$sd/gemu"
    for d in "$ROOT"/tests/roms/*/; do
        m=$(basename "$d"); mkdir -p "$sd/gemu/$m"; cp "$d"* "$sd/gemu/$m/" 2>/dev/null || true
    done
fi
[ -d "$ROOT/dist/apps/gemu" ] && cp "$ROOT"/dist/apps/gemu/* "$sd/apps/gemu/"
[ -n "$netplay" ] && printf 'user=wii\nconn=3\nserver=%s\n' "$netplay" > "$sd/gemu/netplay.ini"
for rom in "${roms[@]}"; do
    m=$(machine_for "$rom") || { echo "no machine for $rom" >&2; exit 1; }
    mkdir -p "$sd/gemu/$m" && cp "$rom" "$sd/gemu/$m/"
done
# Normal runs also stage the repo's roms/ folder (gitignored): files directly
# in roms/ go by extension, roms/<machine>/ as is.
if [ -z "$autotest" ] && [ -d "$ROOT/roms" ]; then
    while IFS= read -r -d '' rom; do
        rel=${rom#"$ROOT/roms/"}
        if [[ $rel == */* ]]; then m=${rel%%/*}; else m=$(machine_for "$rom") || continue; fi
        mkdir -p "$sd/gemu/$m" && cp "$rom" "$sd/gemu/$m/"
        staged=$((${staged:-0} + 1))
    done < <(find "$ROOT/roms" -type f ! -name '.*' -print0)
    [ -n "${staged:-}" ] && echo "Staged $staged ROM(s) from roms/"
fi
[ -n "$gc" ] && build_dol
[ -f "$dol" ] || { echo "$dol missing (build first)" >&2; exit 1; }

# ── Dolphin profile ──────────────────────────────────────────────────────────
mkdir -p "$profile/Config"
if [ -z "$autotest" ]; then
    for f in WiimoteNew.ini GCPadNew.ini; do
        [ -f "$USER_CFG/$f" ] && cp "$USER_CFG/$f" "$profile/Config/$f"
    done
fi

# set_ini FILE SECTION KEY VALUE... : update keys, keep everything else.
set_ini() {
    python3 -I - "$@" <<'EOF'
import configparser, sys
path, args = sys.argv[1], sys.argv[2:]
ini = configparser.ConfigParser(interpolation=None, strict=False)
ini.optionxform = str
ini.read(path)
for sec, key, val in zip(args[0::3], args[1::3], args[2::3]):
    if not ini.has_section(sec): ini.add_section(sec)
    ini.set(sec, key, val)
with open(path, "w") as f: ini.write(f)
EOF
}

cfg=$profile/Config
set_ini "$cfg/Dolphin.ini" \
    Core WiiSDCard True  Core WiiSDCardEnableFolderSync True \
    General WiiSDCardPath "$profile/WiiSD.raw" \
    General WiiSDCardSyncFolder "$sd/" \
    Interface ConfirmStop False  Interface ShowSplash False \
    AutoUpdate UpdateTrack ""

if [ -n "$autotest" ]; then
    # Scripted input only: no emulated controllers to pick up stray keys.
    set_ini "$cfg/Dolphin.ini" Core SIDevice0 0  Core SIDevice1 0 \
        Core SIDevice2 0  Core SIDevice3 0  Movie DumpFrames True  DSP DumpAudio True
    set_ini "$cfg/GFX.ini" Settings DumpFramesAsImages True
    set_ini "$cfg/WiimoteNew.ini" Wiimote1 Source 0  Wiimote2 Source 0 \
        Wiimote3 Source 0  Wiimote4 Source 0  BalanceBoard Source 0
else
    set_ini "$cfg/WiimoteNew.ini" Wiimote1 "Options/Sideways Wiimote" True
    # GameCube controller 1 plugged in (6 = standard controller)
    [ -n "$gc" ] && set_ini "$cfg/Dolphin.ini" Core SIDevice0 6
fi

# ── Run ──────────────────────────────────────────────────────────────────────
if [ -n "$hbc" ] && [ -z "$autotest" ]; then
    if [ ! -d "$profile/Wii/$HBC_DIR" ]; then
        [ -d "$USER_NAND/$HBC_DIR" ] || {
            echo "The Homebrew Channel isn't installed in $USER_NAND." >&2
            echo "Install it in Dolphin first (e.g. with the HackMii installer)." >&2
            exit 1; }
        echo "Copying your Dolphin NAND into $profile/Wii (once)..."
        mkdir -p "$profile/Wii" && cp -a "$USER_NAND/." "$profile/Wii/"
    fi
    echo "Starting the Homebrew Channel; GEMU is in sd/apps/gemu/"
    exec "$DOLPHIN" -u "$profile" -n "$HBC_TITLE"
fi
if [ -z "$autotest" ]; then
    echo "Starting $dol"
    echo "ROMs: $sd/gemu/<machine>/   Dolphin profile: $profile"
    exec "$DOLPHIN" -u "$profile" -e "$dol"
fi

rm -rf "$profile/Dump"
echo "Autotest${autotest:+ (AUTOTEST=$autotest)}: running Dolphin for ${seconds}s..."
timeout "$seconds" "$DOLPHIN" --batch -u "$profile" -e "$dol" >"$profile/dolphin.log" 2>&1 || true
# Dolphin finishes writing the dump while shutting down; give it a moment.
frame=
for _ in $(seq 20); do
    frame=$(ls -t "$profile"/Dump/Frames/*.png 2>/dev/null | head -1 || true)
    [ -n "$frame" ] && break
    sleep 0.5
done
[ -n "$frame" ] || { echo "no frame dumped; see $profile/dolphin.log" >&2; exit 1; }
cp "$frame" "$profile/last-frame.png"
audio=$(ls "$profile"/Dump/Audio/*_dspdump1.wav 2>/dev/null | head -1 || true)
[ -n "$audio" ] && cp "$audio" "$profile/audio.wav"
echo "frame: $profile/last-frame.png"
[ -n "$audio" ] && echo "audio: $profile/audio.wav"
exit 0
