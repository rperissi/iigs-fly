# Cursor kickoff prompt: FLY demo

Paste the text below as the first message. Attach the files listed under "Attached files."

---

We're building a small Apple IIgs demo app today for the machine's 40th anniversary. It has to run on real hardware and be filmed within a few hours, so scope discipline matters more than polish. Read `fly-demo-spec.md` end to end before writing any code; it contains the design, exact on-screen copy, layout by pixel row, palette values, data format, sounds, and a cut-from-the-bottom priority list. Do not redesign anything in it. If something in the spec conflicts with how this repo's existing routines work, adapt the implementation, not the design, and tell me what you changed.

## Attached files

- `fly-demo-spec.md`: the full build spec. This is the source of truth.
- `pack_fly.py`: host-side Python that fetches real neuron skeletons from the MaleCNS connectome via neuPrint and packs `FLYDATA.BIN`. Requires `NEUPRINT_TOKEN` in the environment; I'll have the token ready. Run this first so we have real data to load. If neuprint-python's schema has drifted (column names, function signatures), fix the script rather than faking data.
- `The Fly.shr`: the intro backdrop, already converted. Source path on my machine: `/Users/robertperissi/Downloads/IIGS/Images/The Fly.shr`. It is a raw `$C1/0000` screen, 32768 bytes, unpacked. Do not run a `$C1` unpacker or PackBytes on it. Blit it straight to `$012000`. Layout: `$2000` pixels (32000 bytes, 4bpp, 320 mode), `$9D00` SCBs (256 bytes, 200 used, 56 pad `$00`, high nibble 0 so no fill and no interrupt), `$9E00` 16 palettes x 16 entries x `$0RGB` little-endian. Palettes land at `$9E00` with that blit; there is no CIM 32712 slide to account for. Palette 1 is free: no scanline SCB selects `$1` and the 32 bytes at `$9E20` are zero, so put the text colors there per the spec.
- `mock-boot-sequence.png`, `mock-viewer.png`, `mock-title-card-1.png`, `mock-title-card-4.png`: renders of what each screen should look like. Match these for layout and color. The font in the renders is a stand-in; use the 8-point system font or the repo's existing bitmap font.

## Use what's already here

This repo is my IIgs dev stack. Reuse its existing routines for SHR setup, line drawing, palette writes, keyboard polling, Ensoniq sample playback, GS/OS file loading, and CoGS HTTP transfer before writing anything new. If a routine is missing, write the smallest one that works and put it where the repo's conventions say it goes. Build with the toolchain and disk-image steps this repo already uses; the output is a GS/OS S16 app plus its data files on the disk image.

## Build order

Follow section 10 of the spec exactly, in order, and stop to show me a running build after each step:

1. Load `FLYDATA.BIN` from disk, rotate and draw one neuron in the viewport, single palette. Get this on real hardware before anything else.
2. Label lines and benchmark strip with real numbers, including the "full connectome in N days" line.
3. Boot sequence screen with the exact copy, typewriter effect, throttled progress bar, disk branch.
4. Opening title cards with palette fades.
5. CoGS branch: real HTTPS fetch driving the progress bar, cipher line from the card.
6. Sounds at the four trigger points. Generate the samples with the Python in section 9 of the spec.
7. Auto-advance with per-neuron hue shift and fade-in.
8. Scanline gradient on the intro, palette cycling, synapse dust.

Anything after step 4 is a bonus. If step 1 or 2 is fighting you, tell me immediately rather than working around it silently.

## Rules

- Every number on screen comes from real data: file header, transfer byte counts, tick counter. No constants standing in for measurements.
- Esc exits cleanly from every screen and restores video and sound state. Without input the viewer loops forever with counters climbing.
- Keep the render loop and the intro fully separate so the intro can be skipped with any key and the viewer can be launched directly for testing.
- Keep all four samples under 32KB combined in the top half of DOC RAM and leave a no-op music hook at viewer start (spec section 9).
- Commit after each step with a one-line message naming the step.

Start by reading the spec, then summarize in ten lines or fewer how you'll map its sections onto this repo's existing code, and what you'll need to write new. Then run `pack_fly.py` and begin step 1.
