# FLY: Apple IIgs 40th Anniversary Connectome Demo

Build spec. Target: a runnable GS/OS S16 app today, captured on video for a Facebook post marking the IIgs's 40th anniversary (announced Sept 15, 1986). The demo shows a IIgs pulling a block of real fruit fly neurons (MaleCNS v1.0, published Sept 3, 2026) through a CoGS card, then rotating and "analyzing" them in SHR with benchmark numbers on screen.

Priority order is at the bottom. Cut from the bottom if time runs out. The intro plus one rotating neuron is a shippable demo on its own.

## 1. What it does, in one paragraph

App opens on four title cards (the science date, the numbers, the GS date, the payoff line), fades to a riser sound, then cuts to an SHR "boot sequence" screen over a user-supplied backdrop image: console-style lines type in, a TLS link to a2cogs.com is established through the CoGS card, a neuron block downloads with a live progress bar, two sampled sounds punctuate the sequence. It then wipes to the viewer: one real neuron skeleton rotating in 3D, depth-shaded in a teal palette, with a plain-English label, real synapse counts, and a benchmark strip (nodes transformed per second, cumulative neurons and synapses processed, progress against 166,700, and a projected time to process the whole connectome at the current rate). Auto-advances through the neuron block with a palette shift per neuron. If no CoGS card is present, the same intro plays with a "no card found, loading cached block from disk" branch.

## 2. Files

- `FLY` (S16 application)
- `FLYDATA.BIN` (packed neuron block, produced by `pack_fly.py`, also hosted at `https://a2cogs.com/fly/FLYDATA.BIN`)
- `The Fly.shr` (raw `$C1/0000` screen, 32768 bytes, unpacked, no PackBytes. Blit straight to `$012000`: pixels at `$2000`, SCBs at `$9D00` (200 used, pad zero, high nibble 0, 320 mode), 16 palettes at `$9E00` as `$0RGB` LE. Palette 1 is unused by every SCB and its 32 bytes at `$9E20` are zero; text colors go there)
- `SND.RISE`, `SND.LINK`, `SND.DONE`, `SND.TICK` (raw 8-bit unsigned samples, see section 9)
- `pack_fly.py` (host side, already written; format in section 3)

## 3. Data format (FLYDATA.BIN, little-endian)

```
magic      4 bytes  'FLY1'
count      u16      number of neurons
per neuron:
  bodyId   u32
  type     16 bytes ASCII, null padded
  nNodes   u16
  pre      u32      presynaptic site count
  post     u32      postsynaptic site count
  nodes    nNodes * (int16 x, int16 y, int16 z)   centered on centroid, scaled to +/-30000
  parents  nNodes * u16                            index of parent node, 0xFFFF for root
```

Typical neuron is 600 nodes, about 4.8KB. A 20-neuron block is under 100KB. Load the whole file into a memory-manager block; keep pointers to each neuron's start. Nodes are a tree: draw one line per node from the node to its parent, skipping roots.

Host side: `pack_fly.py` fetches via neuprint-python (dataset `male-cns:v1.0`, free token from neuprint.janelia.org), downsamples with navis to 600 nodes max, packs. Cell types in the default list were picked for looks and for having a one-line story (section 7).

## 4. Transport

Two paths, selected at launch:

1. CoGS present: HTTPS GET `https://a2cogs.com/fly/FLYDATA.BIN` using the same card HTTP path the Config ROM uses for `cogs-volumes.json`. Stream into the memory block. Report bytes received to the intro screen as they arrive so the progress bar is real. Capture whatever TLS version and cipher the card reports for the peer line.
2. No card (or fetch fails): open `FLYDATA.BIN` from the app's directory via GS/OS. Fake nothing; the intro copy changes to the fallback branch (section 6).

Detect the card the way the ROM does (slot scan / signature). Do not hang on a missing card; the fallback must be automatic and fast.

## 5. Opening title cards

Four cards before the boot sequence. Black screen, palette 1 reserved for text, big type centered (QuickDraw II with a 16 or 20 point font, or a hand 8x16 font drawn double-height). Each card palette-fades in over 0.5s (lerp text entries from $000 to their target color), holds, fades out over 0.5s. Total run about 15 seconds. Any key skips to the boot sequence.

Text colors: headline lines in $CFF (white-green), body lines in $6EE (aqua). Same family as the viewer ramp so the whole piece is one palette.

### Copy (exact)

Card 1 (hold 4s):

```
September 3, 2026

Scientists published the first complete map
of a fruit fly's entire central nervous system:
brain and nerve cord, every neuron, every connection.
```

Card 2 (hold 3s):

```
166,700 neurons
125,000,000 synaptic connections
```

Card 3 (hold 3.5s):

```
September 15, 1986

Apple announced the Apple IIgs.
16-bit 65C816.  2.8 MHz.  4,096 colors.
```

Card 4 (hold 4s):

```
Forty years later, to the day,
this IIgs will download real neurons
from that map and process them.

Live. On the 65C816.
```

After card 4 fades to black: play `SND.RISE`, hold 300ms on black, then cut to the boot sequence screen (backdrop appears in one frame with the palette already set; the riser covers the cut).

Copy notes: neurons and synaptic connections are the two real numbers; do not add a third "connections" figure, synapses are the connections. "First complete map of the entire central nervous system" is the accurate first (a female brain-only map existed from 2024); keep that wording.

## 6. Boot sequence screen

SHR 320x200, 16-color mode. Backdrop image occupies the full screen; text draws over it in a region with its own SCB palette (rows ~100-199 use palette 1 so text colors don't fight the image's palette 0). Text via QuickDraw II DrawString or a hand font; either is fine. Monospace look preferred.

### Copy (exact)

Lines appear one at a time, typewriter effect (about 15ms per character), with a 350ms hold after each line. Dotted leaders fill left to right before the right-hand result appears. Block cursor blinks at the current line end.

CoGS present:

```
CoGS TLS link ......................... ESTABLISHED
Peer: a2cogs.com  TLS 1.3  {cipher}

Source: Fruit Fly Brain Map
The complete wiring of a fruit fly's nervous
system: 166,700 neurons, 125 million connections.
Published Sept 3, 2026.

Requesting neuron block 0x{blockid} ...... {count} neurons
Receiving {bytes} bytes

[############################.............]  {pct}%

Block received. Starting analysis on the 65C816.

Apple IIgs  Sept 15, 1986 - Sept 15, 2026
```

No card:

```
CoGS TLS link ......................... NO CARD FOUND
Loading cached neuron block from disk

(then identical from "Source:" onward)
```

Substitutions must be real values: `{cipher}` from the card, `{blockid}` is a 16-bit hash of the file (e.g. low 16 bits of a sum over the header) so it's stable per block, `{count}` and `{bytes}` from the file header and transfer, `{pct}` from bytes received over total.

### Progress bar

Text row, 40 characters wide, `[` and `]` fixed, `#` fills as bytes arrive. Bar fill color palette-cycles (section 8). On the disk path the read is near-instant, so throttle the bar to take about 1.5 seconds so it's visible on video.

### Timing and sounds

- "ESTABLISHED" (or "NO CARD FOUND") appears: play `SND.LINK`.
- Bar reaches 100%: play `SND.DONE`, hold 400ms, then type "Block received..." line.
- After the last line, hold 1.5 seconds, then wipe to the viewer (a top-to-bottom line-by-line clear to black over ~300ms is enough).

Any key skips the intro to the viewer.

## 7. Viewer screen

### Layout (320x200)

```
row   0-  9   title bar text: "Apple IIgs x Fruit Fly Brain Map"   (right: "40 years")
row  10-149   neuron viewport, 320x140, centered rotation
row 150-159   neuron label line 1: "Neuron {type}: {tag}"
row 160-169   label line 2: "ID {bodyId}   {nNodes} nodes   {pre} pre / {post} post synapses"
row 172-181   bench line 1: "{nps} nodes/sec   {fps} fps   neuron {i}/{count}"
row 182-191   bench line 2: "Processed: {N} neurons  {S} synapses   [{bar}] {pct}% of 166,700"
row 192-199   credit: "MaleCNS v1.0  HHMI Janelia / Google Research   CoGS a2cogs.com"
```

Text rows use their own SCB palette (palette 1). Viewport rows use palette 0 for the depth ramp.

### Plain-English tags

Lookup by type string, prefix match. Unknown types fall back to "brain neuron".

| type | tag |
|---|---|
| GF | giant fiber, the escape reflex |
| DNa01 | descending neuron, brain to nerve cord, steering |
| DNa02 | descending neuron, brain to nerve cord, turning |
| MBON01 | mushroom body output, memory readout |
| MBON03 | mushroom body output, memory readout |
| LC10 | visual neuron, tracks a mate during courtship |
| LC4 | visual neuron, detects looming threats |
| PPL101 | dopamine neuron, reward and punishment learning |
| aMe12 | circadian clock neuron |

### Rendering

- Rotation about the vertical (Y) axis only. 256-entry sin table in 8.8 fixed point. Angle advances by 1 or 2 per frame.
- For each node: `xr = (x*cos - z*sin) >> 8`, `zr = (x*sin + z*cos) >> 8`. Orthographic projection. Screen x = 160 + (xr >> 8), screen y = 80 + (y >> 9) (y uses >>9 to fit 140 rows; tweak so the largest neuron fills the viewport without clipping, or precompute a per-neuron scale from its bounding box on load).
- Color: map `zr` (range roughly +/-30000) to 16 palette entries, entry 1 far/dim through entry 15 near/bright. Entry 0 is background.
- Draw one line per node to its parent's projected point. Any Bresenham SHR line routine; QuickDraw II LineTo is acceptable for first pass.
- Erase: keep the previous frame's projected points and redraw those lines in color 0 before drawing the new frame. Cheaper than clearing 22KB per frame. Because the viewport background is pixel value 0 everywhere, the gradient background (section 8) lives entirely in per-scanline palette entry 0, so erasing with 0 restores it exactly.
- 600 lines per frame at 2.8MHz will land somewhere around 3-6 fps with a decent line routine. That's fine on video; it looks like the machine is working. Report real fps.

### Benchmark numbers

- `nps` = nodes transformed in the last second (count per frame, accumulate against the 60Hz tick counter).
- `fps` = frames in the last second.
- `N` = neurons completed so far this run. A neuron counts as processed after it has been displayed for its dwell time (12 seconds) or the user advances.
- `S` = sum of (pre + post) over processed neurons.
- `pct` = N / 166700, shown with one decimal.
- Bench line 3 (optional, rotate with line 2 every 5 seconds): "At this rate: full connectome in {days} days" where days = (166700 / N) * elapsed_seconds / 86400. Show hours if under a day. This is the line people quote; make sure it's on screen for a good stretch of the video.

### Controls

- Space: next neuron
- A: toggle auto-advance (default on, 12s dwell)
- P: cycle palette hue (section 8)
- Esc: quit to GS/OS from any screen (title cards, boot sequence, viewer), restore video and sound state

Without input the viewer runs indefinitely. When the block is exhausted, wrap to the first neuron but keep the cumulative counters (N, S, elapsed) growing, so the progress and "N days" figures keep converging the longer it runs.

### Per-neuron transition

On advance: erase current, play `SND.TICK`, shift the depth ramp hue (section 8), fade the new neuron in over ~10 frames by starting with only entries 1-4 lit and stepping the ramp up. Cheap and looks like data loading.

## 8. Palette and effects

Depth ramp (palette 0, viewport), teal set, values as SHR $0RGB:

```
 0  $000  background (overridden per scanline for the gradient)
 1  $021
 2  $032
 3  $043
 4  $054
 5  $065
 6  $076
 7  $087
 8  $098
 9  $0A9
10  $0BA
11  $0CB
12  $3DC
13  $6EE
14  $9FE
15  $CFF
```

Hue shift on advance: rotate which channel carries the ramp (teal, then blue, then magenta-ish, then amber) by permuting RGB nibbles in the 15 entries. Four variants is plenty.

Scanline gradient: for viewport rows, set each row's palette entry 0 from $000 at the top through $021 mid to $032 near the bottom of the viewport. Requires one palette per band of rows; SCBs allow 16 palettes, so use 3-4 bands rather than a true per-row ramp (palettes 0, 2, 3, 4 for the bands; palette 1 stays for text).

Palette cycling on the intro bar: rotate 4 entries in palette 1 every 4 frames.

Synapse dust (intro only, optional): 150 random single pixels in rows 100-199 using two dim entries; swap those two entries every 8 frames so they twinkle.

## 9. Sounds

Four short 8-bit samples played on the Ensoniq via the Sound Tool Set (FFStartSound) or direct DOC register writes, whichever the stack already has. Constraints: samples are unsigned 8-bit, and byte value $00 is a stop marker on the DOC, so clamp to 1..255. Keep total under 32KB so they fit in DOC RAM alongside nothing else. Playback rate around 13-15 kHz.

Generate them on the host with a few lines of Python (numpy, write WAV, then convert to raw 8-bit unsigned):

- `SND.RISE` (~1.2s): low riser, filtered noise plus a sine sweeping 80Hz to 600Hz, swelling then cut. Covers the title-to-boot transition.
- `SND.LINK` (~0.4s): rising sine sweep 400Hz to 1600Hz with a fast attack and a short tail. "Connection up."
- `SND.DONE` (~0.9s): two-note chime, 880Hz then 1320Hz, each a sine with a couple of harmonics and exponential decay, second note overlapping the first's tail. "Transfer complete." This is the one that needs to sound futuristic; a touch of vibrato on the second note helps.
- `SND.TICK` (~0.08s): short filtered click, 2kHz sine burst with instant decay. Per-neuron advance.

Suggested generator (host side, adjust to taste):

```python
import numpy as np, wave
R = 14000
def env(n, a=0.01, d=0.3):
    t = np.arange(n)/R
    return np.minimum(t/a, 1) * np.exp(-t/d)
def save(name, s):
    s = np.clip(s / np.abs(s).max(), -1, 1)
    b = np.clip((s*126 + 128).astype(np.uint8), 1, 255)
    open(name, "wb").write(b.tobytes())
t = np.arange(int(1.2*R))/R
rng = np.random.default_rng(1)
noise = rng.standard_normal(len(t)); noise = np.convolve(noise, np.ones(24)/24, "same")
save("SND.RISE", (np.sin(2*np.pi*(80*t + (520/1.2)*t*t/2)) + 0.6*noise) * np.minimum(t/1.1, 1)**2 * (t < 1.15))
t = np.arange(int(0.4*R))/R
save("SND.LINK", np.sin(2*np.pi*(400*t + (1200/0.4)*t*t/2)) * env(len(t), 0.005, 0.15))
t = np.arange(int(0.9*R))/R
n1 = (np.sin(2*np.pi*880*t) + 0.4*np.sin(2*np.pi*1760*t)) * env(len(t), 0.005, 0.25)
t2 = np.maximum(t-0.25, 0)
n2 = (np.sin(2*np.pi*1320*t2 * (1 + 0.01*np.sin(2*np.pi*6*t2))) + 0.3*np.sin(2*np.pi*2640*t2)) * env(len(t), 0.005, 0.35) * (t > 0.25)
save("SND.DONE", n1 + n2)
t = np.arange(int(0.08*R))/R
save("SND.TICK", np.sin(2*np.pi*2000*t) * env(len(t), 0.002, 0.02))
```

Play `SND.RISE`, `SND.LINK` and `SND.DONE` on one generator, `SND.TICK` on a second so it never cuts off a chime.

DOC RAM budget: keep all four samples under 32KB combined and load them into the top half of DOC RAM. A SoundSmith or similar tracker module may be added later and will need the rest; leave the player hook as a no-op call at viewer start so the music can drop in without touching the demo loop.

## 10. Build priorities (cut from the bottom)

1. Load `FLYDATA.BIN` from disk, rotate and draw one neuron in the viewport, single palette. This alone is a demo.
2. Label lines and benchmark strip with real numbers, including the "full connectome in N days" line.
3. Boot sequence screen with the exact copy, typewriter effect, throttled progress bar, disk branch.
4. Opening title cards with palette fades.
5. CoGS branch: real HTTPS fetch driving the bar, cipher line from the card.
6. Sounds at the four trigger points.
7. Auto-advance with per-neuron hue shift and fade-in.
8. Scanline gradient, intro palette cycling, synapse dust.
9. Music hook (SoundSmith module), later.

## 11. Video and post notes

- Film the real machine, not an emulator. Let the title cards and boot sequence run uncut, then hold on the viewer through at least one auto-advance so the counter and the "N days" line are visible.
- Caption hooks: announced 40 years ago today; the brain map it's rendering was published 12 days ago; The Fly hit theaters one month before the GS did in 1986; the CoGS card carrying the download ships in a few weeks and is open source.
- Credit line stays on screen the whole time: MaleCNS v1.0, HHMI Janelia / Google Research.
