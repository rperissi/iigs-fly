#!/bin/bash
# Build FLY.S16 (Golden Gate ORCA/C) and inject onto a GS/OS 2mg.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$HERE"
echo "== occ =="
# cogslib at -O0 (ORCA -O bit $20 miscompiles its STATUS poll; see cogs/gs/Makefile),
# fly.c at -O255 for the line drawing. Objects link together.
[ -f lib/cogs.a ] || (cd lib && occ -c -b -O0 -w255 cogs.c)
[ -f lib/cogs_io_slot.a ] || (cd lib && occ -c -b -O0 -w255 cogs_io_slot.c)
# NTP trampoline and loader at -O0 (inline JSL + handle setup)
occ -c -b -r -O0 -w255 ntpcall.c
occ -c -b -r -O0 -w255 music.c
occ -b -O223 -w255 -I lib fly.c ntpcall.a music.a lib/cogs.a lib/cogs_io_slot.a -o fly
iix chtyp -t s16 fly
ls -la fly
echo "built $HERE/fly"
