# art

`ink-flow-glass.jpg` — still from the 1.54" port playing a clip. Bench photo,
not a render.

`ink-flow-app.png` — the Ink Flow host GUI.

`fastwave-hero.png` — grayscale test. 256x512, four grey levels, no dithering.
Black, two intermediate states, white: what an SSD16xx gives you natively
before any spatial screen is applied.

`fastwave-hero.ppix` — the same pixels as a PixelPerfect project, one layer,
five-entry palette (slot 0 is that editor's transparent slot and is unused, so
the four tones live at 1..4). Open it, edit it, export at any scale.

`make_hero.py` — draws the plate from primitives. Requires Pillow.

    python make_hero.py

It prints a tone histogram, which is the only quality control here: if one of
the four levels is barely represented, the composition is leaning on shading it
does not have. With four flat tones there is no shading, so silhouette,
negative space and rim light do all the work — which is the same reason
`docs/PICTURE.md` says what it says about dithering.
