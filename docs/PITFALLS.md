# Pitfalls

Things that are true on every panel touched during this work, and the specific
failure each one produces. Most of these cost a session to find. None of them
announce themselves.

---

## The controller cannot see the glass

OLD RAM and NEW RAM are the driver's **only** model of the panel. Lie about OLD
— leave it stale, skip writing it, XOR it wrong — and the artefact lasts
forever, because `NEW == OLD` is classified as unchanged and the pixel is never
driven again.

There is no read-back path that tells you what the ink is actually doing. If
your model of the glass and the glass disagree, the glass wins and you will not
be told.

## `NEW == OLD` means no drive, which is where grey trails come from

A pixel that goes black-to-white and then sits still is **abandoned
mid-transition**. It parks grey. This is not ghosting from age and it is not a
slow fade — it is a stalled transition, and a short differential update can
never lift it, because as far as the driver is concerned nothing has changed
there.

The fix that costs nothing: **trail chasing.** Keep recently-changed pixels
asserted as changed in the OLD plane (`OLD = NEW ^ mask`, where mask is the OR
of the last N frames' changes), so each gets N drives instead of one. Same LUT,
same frame count, **zero waveform time** — the only cost is the extra OLD plane
write. On glass: trail visible for about 2 frames, then fully clear, white
maintained, no flash.

**The reset phase must be 0 whenever trail chasing is on.** A re-driven pixel
replays the *whole* LUT including the opposite-polarity kick, which flashes dark
on pixels that were already clean; a multi-frame history staggers those into a
pulse travelling behind the moving content. Dropping the kick also made every
mode 16 ms faster, so this is not a compromise.

## Driving to white, onto already-white, darkens

White only holds after a complete black-to-white round trip. One short drive of
a white sprite on a black field will not finish, so an inverted display cannot
use the fastest modes at all — a single-frame drive is unusable while inverted.

This is why "just invert the buffer for a dark theme" is not free on e-paper.

## Never write ink into the outer row and column

Edge ink walks horizontal and vertical ghost lines across the glass during
differential updates. Measured on one panel, and the same 1-pixel park is used
on the others as a precaution.

Park the rim **once**, after a full clear, in the rest colour, and then never
touch it. Do not scale the world down to fake a margin, and do not redraw a
decorative border every frame — a border you rewrite is ink at the edge, which
is the thing being avoided.

## The panel creeps dark, and VCOM is why

Idle pixels get a skipped LUT phase, which parks their source at VSS against a
negative common — a small DC field across the *whole* panel on every update.
It accumulates into a fuzzy grey background over a run.

Writing VCOM to −0.2 V (`0x2C = 0x08`) removes it, for free. Confirmed in both
directions: `0x78` (−3 V) makes it dramatically worse, which is the check worth
doing because it proves the mechanism rather than just the fix.

VCOM is a **DC-balance** control, not a contrast control. Reach for the source
rails for contrast.

It is not enough on its own for a long run with a static field. Letterbox
bars, a still left streaming, and high BB on dark slow content are in
[BURN-IN.md](BURN-IN.md).

## Bit 4 of `0x22` silently reloads the factory waveform

Covered in [WAVEFORM-LAW.md](WAVEFORM-LAW.md) §4, repeated here because it is
the most common way a working custom waveform stops working: any vendor helper
you call for a clear, a deghost or a full refresh probably sets b4, and after it
runs your table is gone. Re-arm.

## Windowed transfer saves SPI and never saves waveform time

The gate scan is always full-panel. Windowing cannot shorten BUSY on these
parts; measured on one controller at MUX 480 / 240 / 120 / 60 the frame period
was 9921 us every time.

Worse, on both panels where it was tried, windowing was **not trusted at the
end of the work**:

- On one, it measured a real win (SPI 101 to 25 ms) and rendered correctly
  *sometimes*, producing stripes and flashing at other times. The cause was
  never found.
- On the other, sub-windows landed scattered across the glass and the feature
  was disabled outright pending a RAM read-back to see where writes actually go.

A separate finding narrowed it: you can window the **NEW** plane freely, but
anything that needs OLD to differ from NEW must go full-plane. If you bring
windowing back, that is the constraint to design around, and read the RAM back
to verify before believing the addressing.

## Do not port a bit's meaning between parts

Bit 3 of `0x22` is Mode 1 vs Mode 2 on one controller and 3-colour vs
black/white on another. The LUT block polarity is inverted between two members
of the same family — on one part the default mapping drove the moving object
white and its trail black, which reads exactly like a picture-level inversion
bug and is not one.

The parts in the same family are similar enough to make this mistake and
different enough for it to cost a day.

## Set the SPI clock once, at boot

Rebuilding the SPI device while the panel is live wedges the controller:
transfers still complete, it stops acting on commands, `busy_ms` reads 0, and
only a full board reset clears it. That is a property of the mid-traffic
rebuild, not of the clock — two different speeds both wedged when switched into
at runtime and both were stable as the only speed the chip ever saw.

A related latent bug, worth checking in your own driver: a runtime clock change
that removes the SPI device **without releasing the bus first** will panic the
chip into a boot loop. If the bus was acquired for the device's lifetime, the
teardown order is not optional.

## Own the analog-rail state in the driver, not in the app

This one is the most expensive failure mode in the file, because its only
symptom is `busy_ms = 0` and frozen glass, which is indistinguishable from a
dead SPI path.

If the app tracks "rails are up" and the driver pulses RST on any path — a
re-init, a recovery from a BUSY timeout, a mode change — the flag is now a lie.
Every subsequent bare DISPLAY byte skips powering the rails and does exactly
nothing. Keep the flag in the driver and clear it in the one reset function that
every init path goes through.

## No generic Arduino e-paper stack

Not a style preference. The stock libraries for these parts:

- ship the OTP waveform, so they pay the vendor's frame count;
- ship EOPT at its POR value, so they pay a discharge scan on every update;
- ramp the analog rails per update, so they pay the fixed cost too;
- and in at least one case ship an activate byte as a documented "fast path"
  that is actually DISPLAY in 3-colour mode on a black-and-white panel.

Every measurement in this repository is against those defaults. That is where
the factor of 3 to 7 comes from.

## Deep sleep needs a hardware reset before the next use

And on one board, a vendor full refresh can glitch the button input pins — mute
inputs around it rather than debouncing harder.
