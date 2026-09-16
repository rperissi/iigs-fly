#!/bin/bash
# run_snap.sh - headless MAME check of FLY. Builds a throwaway 2mg where
# SYSTEM/START is FLY (so GS/OS launches it at boot) with the data files
# beside it, boots from CFFA in slot 7, and screenshots every few seconds.
#
# Env: SNAP_EVERY (s, default 4), SNAP_TOTAL (s, default 120),
#      SNAP_KEYS ("50= ,70= " to post keys at times), MAME, ROMP.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FLY="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$FLY/.." && pwd)"
JAVA="${JAVA:-/opt/homebrew/opt/openjdk/bin/java}"
AC="$ROOT/AppleCommander.jar"
MAME="${MAME:-$ROOT/mame-ample-cogs/mame-arm64}"
ROMP="${ROMP:-$HOME/Library/Application Support/Ample/roms}"
WORK="$(mktemp -d)"
DISK="$WORK/flytest.2mg"
SNAP="$WORK/snap"
mkdir -p "$SNAP"

cp "$FLY/gsfly.2mg" "$DISK"
chmod u+w "$DISK"
"$JAVA" -jar "$AC" -d "$DISK" SYSTEM/START
cat "$FLY/fly" | "$JAVA" -jar "$AC" -p "$DISK" SYSTEM/START S16 '$0000'
for f in FLYDATA.BIN:GSFLY.DATA FLYCARDS.BIN:GSFLY.CARDS; do
  cat "$FLY/${f%%:*}" | "$JAVA" -jar "$AC" -p "$DISK" "SYSTEM/${f##*:}" BIN '$0000'
done
cat "$FLY/FLYBOOT.shr" | "$JAVA" -jar "$AC" -p "$DISK" SYSTEM/GSFLY.SHR BIN '$0000'
# soundtrack: player + song beside the app (music.c looks in prefix 1 first)
cat "$FLY/NTPPLAYER" | "$JAVA" -jar "$AC" -p "$DISK" SYSTEM/NTPPLAYER BIN '$0000'
cat "$FLY/GSFLY.NTP" | "$JAVA" -jar "$AC" -p "$DISK" SYSTEM/GSFLY.NTP BIN '$0000'

NVDIR="$WORK/nv"; mkdir -p "$NVDIR/apple2gs"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
# MAME_EXTRA="-sl3 cogs" puts the virtual CoGS card in the machine.
"$MAME" apple2gs -rompath "$ROMP" -nvram_directory "$NVDIR" \
  -sl7 cffa2 -hard1 "$DISK" -snapshot_directory "$SNAP" ${MAME_EXTRA:-} \
  -nothrottle -skip_gameinfo -video none -sound none \
  -autoboot_script "$HERE/snap.lua" \
  -seconds_to_run "$(( ${SNAP_TOTAL:-120} + 5 ))" >"$WORK/out.log" 2>"$WORK/err.log" || true

rg 'SNAP:' "$WORK/out.log" "$WORK/err.log" | sed 's/^/  /' || true
OUTDIR="${OUTDIR:-/tmp/fly_snaps}"
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"
i=0
for f in $(ls "$SNAP"/apple2gs/*.png 2>/dev/null); do
  cp "$f" "$OUTDIR/$(printf '%03d' $i).png"; i=$((i+1))
done
echo "snapshots: $i in $OUTDIR"
rm -rf "$WORK"
