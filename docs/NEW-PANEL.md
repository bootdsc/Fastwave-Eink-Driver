# Bringing up a panel this repo has never seen

A new panel is a new directory, a new build environment and its own board
header. Not an `#ifdef` inside an existing port.

That is a hard rule and it comes from experience: the sources in `ports/` look
similar enough to merge and behave differently enough that a merged file becomes
untestable. Copy the closest port, then change it until it is true.

---

## Identity — write this down before you power anything

```
Name, diagonal, resolution (native and visible, if they differ)
Controller part number, and HOW MANY of them
MCU, and whether USB is native CDC or a UART bridge
Pins: RST CS BUSY DC SCK MOSI, plus any panel power-enable
RAM layout: row- or column-major, and which bit value is white
Portrait mapping, if you intend to stand it up
Bytes per plane
```

Two of these have bitten hard:

- **How many controllers.** One panel here is two cascaded controllers, each
  400 wide, with 800 bytes of RAM behind 792 visible pixels and an 8-pixel seam
  in the middle. Every LUT write and every plane write goes to both.
- **Panel power-enable.** One board has a GPIO that must be driven high or the
  panel is simply off, with no other symptom.

---

## Measure these. Do not copy them from another port.

- [ ] **Frame period, per FR code** — run `calib`. Do not assume 8.11 or
      9.92 ms. One family member uses an entirely different FR encoding.
- [ ] **Fixed costs and the right activate byte** — run `seqscan`.
- [ ] **LUT size, and whether voltages live inside the LUT block** or in
      separate registers. 233 bytes, 153 bytes and 105 bytes have all been
      correct on different parts here.
- [ ] **Group byte order** — run `bytescan`. A clean linear fit under the wrong
      assumed order is a known trap.
- [ ] **What bit 3 of `0x22` selects.** Mode 1 / Mode 2 on one part, 3-colour /
      black-and-white on another. Getting this wrong looks like a broken
      waveform.
- [ ] **Waveform polarity.** Do the transition blocks map the same way as the
      port you copied? On one part the inherited mapping drove the moving object
      white and its trail black — waveform polarity, not a picture inversion.
- [ ] **Whether EOPT costs you time here.** It cost +40 ms on one panel and
      +17 ms *plus a visible flash* on another, so on one it is a required write
      and on the other it must stay out of the mask.
- [ ] **Maximum stable SPI clock.** Set it once at boot. Never rebuild the
      device while the panel is live.
- [ ] **Whether sub-windows land where you addressed them.** Verify by reading
      RAM back, not by looking at the glass.
- [ ] **Whether gate count changes BUSY.** It did not on any part here, but
      that is two data points.
- [ ] **The USB path.** Native CDC flags, or a bridge and a real baud rate.
- [ ] **Seams, dummy columns, defective gate lines, stuck edges.** Find these
      before you start blaming your waveform.

## Starting values, to be replaced by measurements

```
rim            1 px, parked once, never redrawn
dither         cluster 8x8, 45 degrees
tone (video)   gamma 1.30  contrast 1.40  positive polarity
source rails   datasheet maximum
serial         DTR/RTS deasserted on open, large RX buffer
update mode    differential, rails held up, bare DISPLAY byte
clear          vendor full-refresh cycle, then re-arm your own LUT
```

## Order of operations

1. Pins, power, BUSY polarity. Get the vendor full refresh working, unmodified.
   You need a known-good reference before you have anything to compare against.
2. `card` — the device renders its own test pattern with the vendor sequence.
3. `calib`, `seqscan`, `bytescan`. None of these can change the image.
4. A null LUT. Confirm it costs the full time and changes nothing.
5. Now build a waveform.
6. Calibrate your optical metric before trusting a single contrast number
   ([MEASURING.md](MEASURING.md)).
7. `frame`, then `stream`.

Steps 2–4 are cheap and they are the difference between debugging one thing and
debugging four at once.

## When you find something

Change the shared documentation in the same commit that changes the default.
Leaving the new number in a code comment while a doc still states the old one
creates a second source of truth, and the next person — quite reasonably —
believes the doc.
