#!/bin/bash
# Put GSFLY + GSFLY.DATA + GSFLY.CARDS + GSFLY.SHR on:
#   gsfly.2mg    32MB GS/OS clone of cogsall (CFFA / Ample hard disk, not flop1)
#   gsfly800.2mg 800K floppy for the Apple/Sony 3.5 slot
# and copy the hard disk to cogs/gs/disk/cogsall-gsfly.2mg for the CFFA.
#
# AppleCommander rewrites a 2IMG as raw ProDOS and drops the 64-byte header.
# wrap_2mg.py puts it back so CFFA and Ample will identify the file.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
JAVA="${JAVA:-/opt/homebrew/opt/openjdk/bin/java}"
AC="$ROOT/AppleCommander.jar"
WRAP="$ROOT/scripts/wrap_2mg.py"
SRC="${SRC:-$ROOT/cogs/gs/disk/cogsall.2mg}"
OUT="$HERE/gsfly.2mg"
FLOP="$HERE/gsfly800.2mg"
[ -f "$HERE/fly" ] || { echo "run ./build.sh first"; exit 1; }
[ -f "$AC" ] || { echo "missing $AC"; exit 1; }
[ -f "$SRC" ] || { echo "missing template $SRC"; exit 1; }
[ -f "$WRAP" ] || { echo "missing $WRAP"; exit 1; }

put_fly() {
  local img="$1"
  # old names from earlier builds, then the current set
  for f in FLY FLYDATA.BIN FLY.SHR FLYCARDS.BIN FLYBOOT.SHR GSFLY GSFLY.DATA GSFLY.CARDS GSFLY.SHR GSFLY.NTP NTPPLAYER; do
    "$JAVA" -jar "$AC" -d "$img" "$f" 2>/dev/null || true
  done
  local d="$2"   # "" for the floppy root, "GSFLY/" for a folder on the boot volume
  cat "$HERE/fly" | "$JAVA" -jar "$AC" -p "$img" "${d}GSFLY" S16 '$0000'
  [ -f "$HERE/FLYDATA.BIN" ] && cat "$HERE/FLYDATA.BIN" | "$JAVA" -jar "$AC" -p "$img" "${d}GSFLY.DATA" BIN '$0000'
  [ -f "$HERE/FLYCARDS.BIN" ] && cat "$HERE/FLYCARDS.BIN" | "$JAVA" -jar "$AC" -p "$img" "${d}GSFLY.CARDS" BIN '$0000'
  [ -f "$HERE/FLYBOOT.shr" ] && cat "$HERE/FLYBOOT.shr" | "$JAVA" -jar "$AC" -p "$img" "${d}GSFLY.SHR" BIN '$0000'
  [ -f "$HERE/GSFLY.NTP" ] && cat "$HERE/GSFLY.NTP" | "$JAVA" -jar "$AC" -p "$img" "${d}GSFLY.NTP" BIN '$0000'
  [ -f "$HERE/NTPPLAYER" ] && cat "$HERE/NTPPLAYER" | "$JAVA" -jar "$AC" -p "$img" "${d}NTPPLAYER" BIN '$0000'
}

wrap_2img() {
  local img="$1"
  python3 "$WRAP" --force "$img" "$img.tmp"
  mv "$img.tmp" "$img"
}

rm -f "$HERE/fly.2mg" "$HERE/fly800.2mg" "$ROOT/cogs/gs/disk/cogsall-fly.2mg"
cp "$SRC" "$OUT"
chmod u+w "$OUT"
put_fly "$OUT" "GSFLY/"
"$JAVA" -jar "$AC" -n "$OUT" GSFLY
wrap_2img "$OUT"

rm -f "$HERE/gsfly800.po"
"$JAVA" -jar "$AC" -pro800 "$HERE/gsfly800.po" GSFLY800
put_fly "$HERE/gsfly800.po" ""
python3 "$WRAP" "$HERE/gsfly800.po" "$FLOP"
rm -f "$HERE/gsfly800.po"

echo "--- $OUT (32MB, CFFA / hard disk) ---"
"$JAVA" -jar "$AC" -l "$OUT" | egrep -i 'GSFLY|PRODOS|^/' | head -20
echo "--- $FLOP (800K floppy) ---"
"$JAVA" -jar "$AC" -l "$FLOP" | egrep -i 'GSFLY' | head -20
cp "$OUT" "$ROOT/cogs/gs/disk/cogsall-gsfly.2mg"
echo "hard disk: $OUT"
echo "CFFA copy: $ROOT/cogs/gs/disk/cogsall-gsfly.2mg"
echo "floppy:    $FLOP"
