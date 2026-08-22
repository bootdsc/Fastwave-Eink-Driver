# prompts

## [PORT-TO-YOUR-PANEL.md](PORT-TO-YOUR-PANEL.md)

Hand this to a coding agent working in an empty directory. It fetches the
current release, interviews you about your hardware, picks the closest existing
port, and works through the bring-up ladder in the order that keeps each step
diagnosable.

It is written to be safe on hardware you do not want to damage: the three
measurement passes it runs first cannot change what is on the glass, and it is
told to ask before flashing, before opening a serial port, and before touching
any panel voltage.

What it will ask you for: panel and controller part numbers, how many
controllers, the MCU and whether its USB is native, your six wiring pins, the
framework, what the display is for, whether you have datasheets, whether there
is working vendor code, whether the panel has a third pigment, whether the panel
is replaceable, and the serial port.

"Don't know" is an acceptable answer to most of those. Several are settled later
by measurement, and one is settled by looking at the panel from the side.

## Using it with a specific release

The prompt pulls the latest release. To pin a version, replace the download step
with a tag:

```
gh release download v1.0.0 --repo bootdsc/fastwave -D fastwave
```

## If you are writing your own

The three things worth copying out of it, whatever tooling you use:

1. **Read the reference material before touching hardware**, and treat every
   number in it as belonging to the panel it was measured on. The shape of the
   timing law transfers between controllers. The constants do not, and almost
   every expensive mistake in this project was somebody carrying a constant
   across a part boundary.
2. **Run the non-destructive probes first**, in order. They cost nothing and
   they turn "debug four things at once" into "debug one thing".
3. **Calibrate the optical metric before quoting a contrast figure.** An
   eyeballed camera crop scored an 8 ms drive and a 200 ms drive within seven
   points of each other, and did so convincingly.
