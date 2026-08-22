# Getting a picture out of one bit

A fast waveform gets you a black-and-white panel updating quickly. Everything
that makes it look like an image happens on the host, and on e-paper the choices
are not the ones you would make for a screen.

---

## Dither is a waveform constraint, not a taste question

Measured on one panel with everything else held constant, streaming the same
video:

| dither | apparent levels | result on glass |
|--------|-----------------|-----------------|
| Bayer 8x8 | 64 | smooth gradients, **smears badly** |
| Bayer 4x4 | 16 | **structured banding** |
| cluster 4x4 | 16 | resolves, but a visible blocky 4x4 texture |
| **cluster 8x8, 45°** | **64** | **best** |

The mechanism is specific to this medium. **An isolated black pixel surrounded
by white is the hardest case e-paper has**: lateral field spread from its
neighbours fights it, and a short waveform never fully resolves it. Dispersed
(Bayer) matrices are nothing *but* isolated pixels, so they smear. A regular
lattice of half-resolved pixels reads as banding. Clustered matrices grow
compact blobs that do resolve — but an axis-aligned 4x4 grid is visible as
texture, hence the 45° screen, for the same reason print halftones are rotated.

So: **clustered, 8x8, rotated.** This also happens to be the right answer for
the wire (below), which is a coincidence worth taking.

## Ordered, and positionally fixed — never error-diffusion

Static areas must emit the **same packed bytes every frame** so they drop out of
the frame-to-frame delta entirely. A positionally fixed matrix does that: an
unchanging region dithers to byte-identical output and disappears from the wire.

Error diffusion (Floyd–Steinberg, Atkinson) perturbs every pixel on every frame.
It destroys compression, and it destroys differential updating, and it produces
exactly the isolated-pixel pattern the panel cannot resolve. Measured: the
changed-byte delta shrank a frame 2.7x with an ordered matrix. With error
diffusion there is nothing to remove.

This is the rule that makes the whole streaming path work, and it is invisible
until you break it.

## Content decides the drive count

Measured on one panel: a game with solid shapes is fine at **drive 2**, while
dithered video is unusable below **drive 4**. On a second panel, single-shot
transitions and continuous video diverged by 21 points of contrast at the fast
end and by almost nothing from drive 8 up.

A dithered photograph has no solid areas to carry it, and every pixel
transitions every frame. **Never carry a drive choice across content types.**
Ship a different default for graphics and for photographic video, and say which
is which:

- **drive 2** — high-contrast graphics only
- **drive 4** — solid-edged / graphic video
- **drive 8** — dithered photographic video

## Tone: ordered dither reads washed out, and the first fix is not gamma

Ordered dithering on a flat threshold ramp looks grey, because any source region
sitting above mid-grey turns most cells white. That is a real effect and it needs
correcting, but the order matters:

**First** raise the source rails and the drive count. **Then** touch tone. Gamma
above about 1.5 turned midtones into grey speckle on one webcam source and read
*more* washed out, not less, because speckle is exactly the isolated-pixel case
the panel cannot resolve.

Defaults that were judged on glass:

| use | gamma | contrast |
|-----|-------|----------|
| video | 1.30 | 1.40 |
| stills | 1.55 | 1.40 |
| grey | 1.20 | 1.40 |

A highlight compressor with a knee is the right control when bright points need
to come down but shadow detail is already correct. Gamma cannot do that — it
moves everything.

**Inverting the packed plane is not a contrast knob.** It inverts the picture.

---

## Native 4-level grey: a stills feature, and here is why

The controllers can do four levels natively. Both RAM planes become bitplanes —
one carries the low bit, one the high bit — and codes run 0 = black to 3 = white.

Note the surprise: on one part `0x24` carries the **low** bit and `0x26` the
**high** bit, which is the opposite of the obvious assumption.

Because both planes are now bitplanes, there is **no previous image** for the
controller to diff against. And to make levels absolute, the waveform starts by
driving every pixel to a common state (black) before pulling each code back
toward white by a different amount. That common reset **is** a full-panel
black/white cycle.

So native grey flashes on every single update. Fine for a still, unwatchable for
anything moving. Keep it as a separate command, not a mode of your streamer.

Sizing matters more than it looks: step values calibrated at 15 V saturate at
17 V, because a full black-to-white transition takes only ~12 frames there, so
codes 1, 2 and 3 all reach white and the image comes out nearly blank. Size
`3 x step` to exactly one transition.

### The flash-free version, and why it is parked

Removing the common reset removes the flash. Give each code a different amount
of black-ward drive and no reset at all, and you get **four levels, one pass,
178 ms, no flash**, measured, starting from a known white field.

It is not enough on its own: without a reset the levels are *relative* to
whatever is already on the glass, and a pixel that should hold its shade is
still driven every frame, so static areas creep darker.

The fix, designed and not built: keep a **shadow buffer** of what is actually on
the glass, and choose each pixel's 2-bit code from the **transition** rather
than from the target.

| code | LUT does | chosen when |
|------|----------|-------------|
| 0 | hold (VS = VSS) | pixel already at target |
| 1 | nudge darker | needs slightly darker |
| 2 | nudge lighter | needs slightly lighter |
| 3 | push hard | large change needed |

Pixels already correct get no drive at all, so static content stops drifting and
nothing is ever driven to a common state — no reset, no flash. Multiple frames
converge a pixel onto its target, which is how flash-free grey is done on
parallel-bus panels. The insight is that it does **not** require parallel
hardware: four LUTs is enough to encode four transition actions, and an SPI
controller will apply them per pixel.

Cost: one byte per pixel of shadow buffer (or 2 bits packed while levels stay at
4).

Open questions, honestly: how many frames a large transition takes to converge
and whether that reads as motion blur or as smearing; whether the nudge steps
need to differ by direction, since the panel is not symmetric; and whether four
levels with per-pixel transitions actually beats 1-bit with an 8x8 cluster
dither, which already gives 65 apparent levels with no flash and is working
today. The dither may simply win.

---

## Grey levels on the live path are spatial, and that is the honest description

An 8x8 cluster matrix is 64 thresholds and therefore 65 apparent steps. The
glass is still strictly black or white at every pixel. Say so in your
documentation, because "16 grey levels of video" is the kind of claim that makes
someone else waste a week looking for a native mode that is not there.
