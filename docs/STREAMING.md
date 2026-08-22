# Streaming video to the panel

The clip cannot live on the device. 488 frames of packed 800x480 mono is 23 MB
against 8 MB of flash. So the host decodes, crops, tone-maps, dithers, packs and
delta-encodes, and the device only applies and displays.

Measured result: **12.4 fps on an 800x480 panel with no flashing between
frames**, and 13–34 fps on a 200x200 one depending on drive count.

---

## Three things had to be true to reach 12 fps

Each was found by measurement, not by reasoning, and each was invisible until
measured.

**1. The USB CDC receive buffer defaults to 256 bytes.** A 4096-byte chunk
arrived as 1205 bytes and the rest was **silently dropped** — no error, no
short-read indication. Set the RX buffer to 65536 *before* `begin()`.

**2. `Serial.readBytes` reads one byte at a time.** It is `Stream::readBytes`,
so a single frame cost 48000 queue operations. Use the bulk `read(buf, n)`.

**3. USB and the panel ran in series.** USB alone measured 4.9 fps, the panel
alone 8.6, and together exactly 3.1 — which is `1/(1/4.9 + 1/8.6)`, the
signature of two serialised stages. Receiving on a task pinned to the other
core, into one of two ping-pong buffers, makes the *slower* of the two set the
rate instead of their sum.

That third number is worth internalising as a diagnostic. If two stages measure
A and B alone and their combination lands on the harmonic sum, they are not
overlapping, whatever your code looks like.

---

## Compression: a changed-byte bitmap

Each frame is sent as a **bitmap of which bytes changed, plus the new values of
those bytes**.

The count of changed bytes is never transmitted — both ends derive it by
counting set bits in the bitmap. There is no length field to fall out of step
with the data, which removes an entire class of desync bug.

It works because the dither matrix is positionally fixed (see
[PICTURE.md](PICTURE.md)): a static area dithers to byte-identical output every
frame and drops out of the delta completely.

Measured on a real clip, alternatives included:

| encoding | bytes/frame | ratio | note |
|----------|------------:|------:|------|
| raw | 48000 | 1.00x | |
| dirty 64-byte blocks | 44921 | 1.07x | dither noise touches nearly every block |
| XOR + RLE | 18920 | 2.54x | |
| **changed-byte bitmap** | **17853** | **2.69x** | **trivial decoder** |
| XOR + deflate | 10094 | 4.76x | needs an inflate on-device |

**Deflate wins on ratio and is still the wrong choice.** With the bitmap the
link measures 18 fps against a 12.4 fps ceiling from the panel, so the wire is
no longer the bottleneck and a decompressor would buy nothing but risk. Stop
optimising a stage that is no longer the limit.

RLE of the *occupancy map* is still worth having even when USB is not the limit,
for a different reason: fewer USB interrupts and one round trip instead of two.

## The wire

```
1x ACK (0x06), then:
  u8   flags     bit0 = occupancy map is RLE (0x00 followed by a zero count)
  u16  n_lit     little-endian
  ...  occupancy raw PLANE/8 bytes, or RLE until that many are decoded
  ...  lit[n_lit]
0x18 in the flags position = abort.
```

The device scatters the literals onto the previous plane, writes NEW, and fires
the bare DISPLAY byte.

Two details that are not arbitrary:

- **The ACK token is `0x06`, not a printable character.** An earlier version
  used `'R'`, which also appears in the string `STREAM ready`. Choose a token
  that cannot occur in your own banner text.
- **The host encodes frame N+1 while frame N is on the wire and on BUSY.** This
  is what hides the host cost entirely; on one panel `chase 0` and `chase 2`
  measured identically over 30 seconds, proving the extra work was free.

## Pace it against the clock

Playback is paced to real time at `source_fps / step` by default.

Without a clock the host ships frames as fast as the panel will take them, so a
mode *faster* than the source needs plays sped up rather than correct — one mode
at 14.9 fps against a 12.5 fps requirement ran 19% quick. A mode *slower* than
the requirement simply falls behind, and no clock can fix that; do not pretend
otherwise in the tool's output.

Choose the drive mode as the **slowest** one that still clears
`source_fps / step`. Slower modes are darker and higher-contrast, so
overshooting costs picture quality while undershooting makes playback drag.

## Enter and leave cleanly

- **The first agreed image is white**, and the host does a deghost before the
  stream starts. Differential play cannot lift residue — that has to happen
  before you start, with the vendor sequence, and then you re-arm your own LUT.
  A deghost that works: three vendor full-refresh black/white cycles, then arm,
  ending on white.
- Use a single refresh to white for stream entry rather than a black/white
  flash, if the panel is already clean.
- The device cannot take commands mid-stream. If you want live knobs, leave the
  stream for a few seconds, apply, and rejoin.
- Force the outer 1 px white so live ink never reaches the rim
  ([PITFALLS.md](PITFALLS.md)).

## Halving the SPI, and what it costs

Writing only the NEW plane and leaving OLD at its last seed halves the SPI and
roughly doubled frame rate on one panel. The differential mode then drives every
non-white pixel.

The trade is real: whites that were previously black may smear until the next
deghost. And **after any frame where you did write OLD, you must write it again
on the next one**, or `NEW == OLD` goes stale and the following update re-drives
garbage.

## Host-side transport notes

- A native-USB device (CDC) reaches roughly 1 MB/s. A board whose USB-C port
  goes through a UART bridge is limited to that bridge — around 200 kB/s — which
  is a hardware ceiling, not a baud-rate setting you can raise. Check which one
  you have before optimising the protocol.
- On one native-USB module 115200 and 230400 both work and 921600 does not
  (host write timeouts, camera stalls). USB full-speed is 12 Mbps regardless;
  the CDC baud rate is nominal, but the failure at high nominal rates was
  reproducible.
- Match the firmware build to the glass. An image built for an 800x480 panel
  driving a 200x200 one cost a whole session of "waveform" debugging. The
  firmware here prints its panel identity in the status banner and in the stream
  banner, the host repacks to match it, and the flash tool has **no default
  environment** for exactly this reason.
