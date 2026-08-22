# Start here

Pick the sentence that matches you.

### "I want my panel to update faster and I have an hour."

Read [WAVEFORM-LAW.md](WAVEFORM-LAW.md). Then run the three probes in
[PROBES.md](PROBES.md) — they cannot change what is on the glass, so they are
safe on hardware you do not want to damage. Then look at whichever port in
`../ports/` is closest to your controller.

If your panel is one of the three already here, skip the probes and use the
port. It has been measured.

### "I have a panel nobody here has seen."

[NEW-PANEL.md](NEW-PANEL.md) is the checklist, in order, with the items that must
be measured rather than copied marked as such.

Or hand [../prompts/PORT-TO-YOUR-PANEL.md](../prompts/PORT-TO-YOUR-PANEL.md) to a
coding agent and let it interview you about your wiring.

### "I want video on e-paper."

**Ink Flow** is the host: `python tools/ink_flow.py` (webcam, a file, or a
screen region). Flash the matching port first. The CLI is
`python tools/webcam.py COMx`.

[STREAMING.md](STREAMING.md) for the wire and the pipeline.
[PICTURE.md](PICTURE.md) for why the dither matrix is a hardware decision and
not a taste one. Both matter; the second one is the surprise.

### "I am going to leave a picture on the glass for a while."

[BURN-IN.md](BURN-IN.md). This driver is experimental and out of spec: one
minute of video is on the order of a five-year warranty's refreshes. Solid
letterbox bars, a still left in the stream loop, and BB redrive on dark slow
content stain the glass. Restore is hours of full black/white (`scrub` /
`blink` / Park), not one differential frame.

### "My picture looks wrong and I don't know why."

[PITFALLS.md](PITFALLS.md). Grey trails, whole-panel darkening, ghost lines at
the edges, a custom waveform that stops working the moment you call a vendor
helper, and frozen glass with `busy = 0` are all in there with their mechanisms.

### "I got a number and I want to know whether to believe it."

[MEASURING.md](MEASURING.md). Timing on these panels does not lie, because it is
a pin. Image quality lies constantly, and the five specific ways it lied during
this work are written down.

### "What is left to do?"

[OPEN-WORK.md](OPEN-WORK.md). The line-timing registers at the top of that list
are, on paper, the only remaining route to a faster waveform, and nothing here
has touched them.

---

## The reference documents

| file | contents |
|------|----------|
| [WAVEFORM-LAW.md](WAVEFORM-LAW.md) | The timing equation, the `0x22` bitfield, the LUT anatomy, where the time goes. The core. |
| [PROBES.md](PROBES.md) | `calib`, `seqscan`, `bytescan`, `probe`, and the bring-up ladder. |
| [PITFALLS.md](PITFALLS.md) | Failure modes and their mechanisms. |
| [BURN-IN.md](BURN-IN.md) | DC stain, letterbox bars, BB on stills / dark slow content. |
| [PICTURE.md](PICTURE.md) | Dither, tone, and native greyscale. |
| [STREAMING.md](STREAMING.md) | Host-to-device video: protocol, compression, pacing. |
| [MEASURING.md](MEASURING.md) | How to measure this without fooling yourself. |
| [NEW-PANEL.md](NEW-PANEL.md) | Bring-up checklist for unfamiliar hardware. |
| [OPEN-WORK.md](OPEN-WORK.md) | Unfinished, honestly. |
| [WINDOWED-TRANSFER.md](WINDOWED-TRANSFER.md) | Partial-window addressing: what was learned, and why it is off. |
| [INTEGRATION-ssd1683.md](INTEGRATION-ssd1683.md) | Copy-paste guide for dropping the SSD1683 driver into an application. |

### Per-panel measurement records

These are the raw findings, panel by panel, with every measurement that produced
a default. Read the one that matches your hardware; skim the others for the
places where two parts in the same family disagree, because those are the
expensive ones.

| file | panel |
|------|-------|
| [panels/SSD1683-crowpanel-5.79.md](panels/SSD1683-crowpanel-5.79.md) | 5.79", 792x272, **two** cascaded SSD1683 |
| [panels/SSD1683-video-player.md](panels/SSD1683-video-player.md) | the video path on that panel, mode by mode |
| [panels/SSD1677-seeed-4.26.md](panels/SSD1677-seeed-4.26.md) | 4.26", 800x480, SSD1677 |
| [panels/SSD1681-1.54.md](panels/SSD1681-1.54.md) | 1.54", 200x200, SSD1681 |

---

## The one-paragraph version, if you read nothing else

An update takes `frames x frame_period`, both of which you set, and **neither of
which depends on the picture**. The vendor waveform spends three to seven times
more frames than it needs to and additionally pays a fixed power-sequencing cost
on every single update, which you can remove with one register write and a
different activate byte. The source rails can go to the datasheet maximum for
free contrast at no time cost. Measure the frame period rather than assuming it,
measure the byte order by perturbing one byte at a time rather than by curve
fitting, and calibrate your optical metric before you trust anything it tells
you about contrast.
