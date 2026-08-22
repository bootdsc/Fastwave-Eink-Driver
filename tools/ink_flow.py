#!/usr/bin/env python3
"""Ink Flow — live webcam / video / gif / image / screen to e-ink.

    python tools/ink_flow.py

Talks to the three fastwave ports. Does not flash.
"""
from __future__ import annotations

import json
import sys
import threading
import time
from argparse import Namespace
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, ttk

TOOLS = Path(__file__).resolve().parent
CFG = Path.home() / ".fastwave" / "inkflow.json"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import webcam as wv

BG = "#161616"
BG2 = "#242424"
FG = "#39ff6a"
DIM = "#1f8f42"
GRAY = "#9a9a9a"

PANELS = (
    ("auto", "Auto (USB id)"),
    ("xiao-426", "4.26\" Xiao 800×480"),
    ("xiao-154", "1.54\" Watchy 200×200"),
    ("crow-579", "5.79\" CrowPanel 792×272"),
)


def load_cfg() -> dict:
    d = {
        "port": "COM30",
        "camera": "1",
        "video": "",
        "panel": "auto",
        "drive": 3,
        "trail": 0,
        "chase": 0,
        "bb": 20,
        "fit": 0,
        "zoom": 100,
        "pad": -1,
        "dither": 4,
        "equalize_min": 15,
        "defaults_v": 2,
    }
    if CFG.is_file():
        try:
            d.update(json.loads(CFG.read_text(encoding="utf-8")))
        except Exception:
            pass
    if int(d.get("defaults_v") or 0) < 2:
        d["drive"] = 3
        d["trail"] = 0
        d["chase"] = 0
        d["bb"] = 20
        d["dither"] = 4
        d["defaults_v"] = 2
    return d


def save_cfg(d: dict) -> None:
    CFG.parent.mkdir(parents=True, exist_ok=True)
    CFG.write_text(json.dumps(d, indent=2), encoding="utf-8")


def com_ports() -> list:
    try:
        import serial.tools.list_ports as lp
        return [p.device for p in lp.comports() if p.vid]
    except Exception:
        return []


def fmt_time(frames, fps):
    if fps <= 0.05 or frames <= 0:
        sec = 0.0
    else:
        sec = frames / fps
    m, s = divmod(int(sec), 60)
    return "%d:%02d" % (m, s)


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Ink Flow")
        self.geometry("760x950")
        self.minsize(700, 760)
        self.configure(bg=BG)
        self.cfg = load_cfg()
        self.stream = None
        self._seek_user = False
        self._screen_box = None
        self._parking = False

        st = ttk.Style(self)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure(".", background=BG, foreground=FG, fieldbackground=BG2)
        st.configure("TFrame", background=BG)
        st.configure("TLabel", background=BG, foreground=FG)
        st.configure("TButton", background=BG2, foreground=FG,
                     bordercolor=BG2, darkcolor=BG2, lightcolor=BG2)
        st.map("TButton",
               background=[("disabled", "#1a1a1a"), ("pressed", "#111111"),
                           ("active", "#333333")],
               foreground=[("disabled", "#2f6a3f"), ("pressed", FG),
                           ("active", FG)])
        st.configure("TRadiobutton", background=BG, foreground=FG)
        st.configure("TCheckbutton", background=BG, foreground=FG)
        st.configure("TLabelframe", background=BG, foreground=DIM)
        st.configure("TLabelframe.Label", background=BG, foreground=DIM)
        st.configure("Horizontal.TScale", background=BG, troughcolor=BG2)
        st.configure("TEntry", fieldbackground=BG2, foreground=FG,
                     insertcolor=FG, bordercolor=BG2)
        st.configure("TCombobox",
                     fieldbackground=BG2, background=BG2, foreground=FG,
                     arrowcolor=FG, bordercolor=BG2,
                     darkcolor=BG2, lightcolor=BG2)
        st.map("TCombobox",
               fieldbackground=[("readonly", BG2), ("disabled", "#1a1a1a"),
                                ("!disabled", BG2)],
               background=[("readonly", BG2), ("disabled", "#1a1a1a")],
               foreground=[("readonly", FG), ("disabled", "#2f6a3f")],
               selectbackground=[("readonly", BG2), ("!disabled", BG2)],
               selectforeground=[("readonly", FG), ("!disabled", FG)],
               arrowcolor=[("disabled", "#2f6a3f"), ("!disabled", FG)])
        # The drop-down list is a tk Listbox, not ttk — style maps miss it.
        self.option_add("*TCombobox*Listbox.background", BG2)
        self.option_add("*TCombobox*Listbox.foreground", FG)
        self.option_add("*TCombobox*Listbox.selectBackground", DIM)
        self.option_add("*TCombobox*Listbox.selectForeground", FG)
        self.option_add("*TCombobox*Listbox.font", "Segoe UI 9")

        pad = ttk.Frame(self)
        pad.pack(fill="both", expand=True, padx=12, pady=10)

        ttk.Label(pad, text="INK FLOW", font=("Segoe UI", 14, "bold")).pack(
            anchor="w", pady=(0, 8)
        )

        row = ttk.Frame(pad)
        row.pack(fill="x", pady=2)
        ttk.Label(row, text="COM").pack(side="left")
        self.port = tk.StringVar(value=str(self.cfg.get("port") or ""))
        self.port_box = ttk.Combobox(row, textvariable=self.port, width=12)
        self.port_box.pack(side="left", padx=6)
        ttk.Button(row, text="Refresh", command=self.refresh_ports).pack(side="left")
        ttk.Label(row, text="Panel").pack(side="left", padx=(16, 4))
        self.panel = tk.StringVar()
        self.panel_box = ttk.Combobox(
            row, textvariable=self.panel, width=26, state="readonly",
            values=[lb for _k, lb in PANELS],
        )
        saved = str(self.cfg.get("panel") or "auto")
        self.panel.set(next((lb for k, lb in PANELS if k == saved), PANELS[0][1]))
        self.panel_box.pack(side="left")

        srcf = ttk.LabelFrame(pad, text="Source")
        srcf.pack(fill="x", pady=8)
        kind0 = "file" if self.cfg.get("video") else (
            "screen" if self.cfg.get("screen") else "camera")
        self.src_kind = tk.StringVar(value=kind0)
        r = ttk.Frame(srcf)
        r.pack(fill="x", pady=4)
        ttk.Radiobutton(r, text="Camera", variable=self.src_kind,
                        value="camera").pack(side="left")
        self.camera = tk.StringVar(value=str(self.cfg.get("camera") or "1"))
        ttk.Combobox(
            r, textvariable=self.camera, width=6, values=["0", "1", "2"],
            state="readonly",
        ).pack(side="left", padx=6)
        ttk.Label(r, text="0 desk · 1 laptop · 2 glass (do not stream 2)",
                  foreground=GRAY).pack(side="left")

        r = ttk.Frame(srcf)
        r.pack(fill="x", pady=4)
        ttk.Radiobutton(r, text="File (video / gif / image)",
                        variable=self.src_kind, value="file").pack(side="left")
        self.video = tk.StringVar(value=str(self.cfg.get("video") or ""))
        ttk.Entry(r, textvariable=self.video, width=42).pack(side="left", padx=6)
        ttk.Button(r, text="Browse…", command=self.browse).pack(side="left")

        r = ttk.Frame(srcf)
        r.pack(fill="x", pady=4)
        ttk.Radiobutton(r, text="Screen", variable=self.src_kind,
                        value="screen").pack(side="left")
        ttk.Button(r, text="Pick region…", command=self.pick_screen).pack(
            side="left", padx=6)
        self.screen_l = ttk.Label(r, text="drag a rectangle, or F = full desktop",
                                  foreground=GRAY)
        self.screen_l.pack(side="left")

        r = ttk.Frame(srcf)
        r.pack(fill="x", pady=4)
        ttk.Radiobutton(r, text="Burn-in equalizer", variable=self.src_kind,
                        value="equalize").pack(side="left")
        self.eq_min = tk.IntVar(value=int(self.cfg.get("equalize_min") or 15))
        self.eq_box = ttk.Combobox(r, width=28, state="readonly")
        self.eq_box.pack(side="left", padx=8)
        self._refresh_eq_labels()
        self.panel_box.bind("<<ComboboxSelected>>", lambda _e: self._refresh_eq_labels())
        ttk.Label(r, text="mode 12 · full rails · Stop to abort",
                  foreground=GRAY).pack(side="left")

        r = ttk.Frame(srcf)
        r.pack(fill="x", pady=6)
        self.seek = ttk.Scale(r, from_=0, to=1000, orient="horizontal",
                              command=self._on_seek)
        self.seek.pack(side="left", fill="x", expand=True)
        self.seek.bind("<ButtonPress-1>", lambda _e: setattr(self, "_seek_user", True))
        self.seek.bind("<ButtonRelease-1>", self._seek_release)
        self.time_l = ttk.Label(r, text="0:00 / 0:00", width=14)
        self.time_l.pack(side="left", padx=8)

        r = ttk.Frame(pad)
        r.pack(fill="x", pady=8)
        self.btn = ttk.Button(r, text="Send", command=self.toggle)
        self.btn.pack(side="left")
        ttk.Button(r, text="White", command=self.white).pack(side="left", padx=8)
        ttk.Button(r, text="Park white", command=lambda: self.park("white")).pack(
            side="left", padx=4)
        ttk.Button(r, text="Park black", command=lambda: self.park("black")).pack(
            side="left")
        self.pause_v = tk.BooleanVar(value=False)
        ttk.Checkbutton(r, text="Pause", variable=self.pause_v,
                        command=self._pause).pack(side="left", padx=8)
        self.status = tk.StringVar(value="idle — flash a port, then Send")
        ttk.Label(r, textvariable=self.status, foreground=DIM).pack(
            side="left", padx=16
        )

        r2 = ttk.Frame(pad)
        r2.pack(fill="x", pady=(0, 6))
        ttk.Label(r2, text="while paused", foreground=GRAY).pack(side="left")
        self.btn_hold_w = ttk.Button(
            r2, text="White out", command=lambda: self.hold_out("white"),
            state="disabled")
        self.btn_hold_w.pack(side="left", padx=8)
        self.btn_hold_b = ttk.Button(
            r2, text="Black out", command=lambda: self.hold_out("black"),
            state="disabled")
        self.btn_hold_b.pack(side="left")
        ttk.Label(r2, text="full clear at drive 12, restores on unpause",
                  foreground=GRAY).pack(side="left", padx=8)

        wf = ttk.LabelFrame(pad, text="Waveform (re-enters stream)")
        wf.pack(fill="x", pady=6)
        self.drive = self._slider(wf, "Drive / mode", 1, 12,
                                  int(self.cfg.get("drive", 3)), 3, self._rearm,
                                  live=False)
        self.trail = self._slider(wf, "Trail", 0, 12,
                                  int(self.cfg.get("trail", 0)), 0, self._rearm,
                                  live=False)
        self.chase = self._slider(wf, "Chase every (0=off)", 0, 8,
                                  int(self.cfg.get("chase", 0)), 0, self._rearm,
                                  live=False)
        # Costs no waveform time: the drive window is split, not extended.
        # It is DC on pixels that never change, so it burns in under a still
        # image. Fine on content that keeps sweeping the screen.
        # Overrides trail/chase — firmware will not combine them.
        self.bb = self._slider(wf, "BB redrive (burn-in risk)", 0, 20,
                               int(self.cfg.get("bb", 20)), 20, self._on_bb,
                               live=False)
        ttk.Label(
            wf,
            text="BB on: trail and chase are off. High BB on a still or a dark "
                 "slow scene stains — docs/BURN-IN.md.",
            foreground=GRAY,
        ).pack(anchor="w", pady=(0, 2))
        vr = ttk.Frame(wf)
        vr.pack(fill="x", pady=4)
        ttk.Label(vr, text="Volts").pack(side="left")
        self.volt = tk.IntVar(value=2)
        for i, lab in enumerate(("±15", "±16", "±17")):
            ttk.Radiobutton(vr, text=lab, variable=self.volt, value=i,
                            command=self._rearm).pack(side="left", padx=8)
        ttk.Label(vr, text="max ±17. ±18+ is fast permanent burn-in. Never.",
                  foreground=GRAY).pack(side="left", padx=12)

        ff = ttk.LabelFrame(pad, text="Framing (live)")
        ff.pack(fill="x", pady=6)
        fr1 = ttk.Frame(ff)
        fr1.pack(fill="x", pady=4)
        ttk.Label(fr1, text="Frame").pack(side="left")
        self.fitv = tk.IntVar(value=int(self.cfg.get("fit") or 0))
        ttk.Radiobutton(fr1, text="Fill (crop)", variable=self.fitv, value=0,
                        command=self._frame).pack(side="left", padx=8)
        ttk.Radiobutton(fr1, text="Fit (bars)", variable=self.fitv, value=1,
                        command=self._frame).pack(side="left", padx=8)
        ttk.Label(fr1, text="Bars").pack(side="left", padx=(16, 4))
        self.padv = tk.IntVar(value=int(self.cfg.get("pad", -1)))
        ttk.Radiobutton(fr1, text="extend", variable=self.padv, value=-1,
                        command=self._frame).pack(side="left", padx=6)
        ttk.Radiobutton(fr1, text="black", variable=self.padv, value=0,
                        command=self._frame).pack(side="left", padx=6)
        ttk.Radiobutton(fr1, text="white", variable=self.padv, value=255,
                        command=self._frame).pack(side="left", padx=6)
        ttk.Label(
            ff,
            text="extend repeats the picture edge (safe). solid white/black "
                 "reverse-drive ~0.25s each minute — an hour of idle white "
                 "is burn-in.",
            foreground=GRAY,
        ).pack(anchor="w", pady=(0, 4))
        self.zoom = self._slider(ff, "Zoom %", 100, 400,
                                 int(self.cfg.get("zoom") or 100), 100,
                                 self._frame)

        tf = ttk.LabelFrame(pad, text="Tone / gray (live)")
        tf.pack(fill="x", pady=6)
        dr = ttk.Frame(tf)
        dr.pack(fill="x", pady=4)
        ttk.Label(dr, text="Gray levels").pack(side="left")
        self.dither = tk.IntVar(value=int(self.cfg.get("dither", 4)))
        for n in (2, 4, 16, 64):
            ttk.Radiobutton(dr, text=str(n), variable=self.dither, value=n,
                            command=self._tone).pack(side="left", padx=8)
        self.bias = self._slider(tf, "Bias (black ← → white)", 1, 254, 64, 64, self._tone)
        self.gamma = self._slider(tf, "Gamma ×100", 50, 250, 130, 130, self._tone)
        self.contrast = self._slider(tf, "Contrast ×100", 40, 250, 140, 140, self._tone)

        cf = ttk.LabelFrame(pad, text="Serial console")
        cf.pack(fill="both", expand=True, pady=6)
        cr = ttk.Frame(cf)
        cr.pack(fill="x", pady=4)
        self.cmd = tk.StringVar()
        ent = ttk.Entry(cr, textvariable=self.cmd)
        ent.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ent.bind("<Return>", self._send_cmd)
        ttk.Button(cr, text="Send", command=self._send_cmd).pack(side="left")
        ttk.Button(cr, text="status", width=7,
                   command=lambda: self._send_cmd(text="status")).pack(
            side="left", padx=4)
        ttk.Button(cr, text="fast", width=6,
                   command=lambda: self._send_cmd(text="fast")).pack(side="left")
        ttk.Button(cr, text="Clear", width=6,
                   command=self._clear_con).pack(side="left", padx=4)
        # A command cannot go down the wire mid-frame, so it is applied by
        # briefly leaving the stream and re-entering. Say so, or a 1 s pause
        # looks like a fault.
        ttk.Label(cf, text="while streaming: leaves and re-enters the stream",
                  foreground=GRAY).pack(anchor="w")
        self.con = tk.Text(cf, height=9, bg=BG2, fg=FG, insertbackground=FG,
                           relief="flat", wrap="none",
                           font=("Consolas", 9))
        self.con.pack(fill="both", expand=True, pady=(4, 0))
        self.con.configure(state="disabled")
        self._con_n = 0

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.refresh_ports()
        self._sync_bb_lock()
        self.after(200, self._tick)

    def _slider(self, parent, label, lo, hi, val, default, cmd, live=True):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=2)
        ttk.Label(row, text=label, width=22).pack(side="left")
        var = tk.IntVar(value=int(val))
        shown = tk.StringVar(value=str(int(val)))

        def snap(_s=None):
            v = int(round(float(sc.get())))
            if int(var.get()) != v:
                var.set(v)
            shown.set(str(v))
            if live:
                cmd()

        def reset():
            var.set(int(default))
            shown.set(str(int(default)))
            cmd()

        sc = tk.Scale(
            row, from_=lo, to=hi, resolution=1, orient="horizontal",
            variable=var, showvalue=False, command=snap,
            bg=BG, fg=FG, troughcolor=BG2, highlightthickness=0,
            bd=0, sliderrelief="flat", activebackground=DIM,
        )
        sc.pack(side="left", fill="x", expand=True)
        if not live:
            sc.bind("<ButtonRelease-1>", lambda _e: cmd())
        ttk.Label(row, textvariable=shown, width=4).pack(side="left")
        btn = ttk.Button(row, text="def", width=4, command=reset)
        btn.pack(side="left", padx=4)
        var._scale = sc
        var._btn = btn
        return var

    def _slider_enabled(self, var, on) -> None:
        st = "normal" if on else "disabled"
        var._scale.configure(
            state=st,
            fg=FG if on else "#555555",
            troughcolor=BG2 if on else "#1a1a1a",
        )
        var._btn.configure(state=st)

    def _sync_bb_lock(self) -> None:
        on = int(self.bb.get()) > 0
        self._slider_enabled(self.trail, not on)
        self._slider_enabled(self.chase, not on)

    def _on_bb(self, *_a) -> None:
        self._sync_bb_lock()
        self._rearm()

    def _send_cmd(self, _e=None, text=None) -> None:
        line = (text if text is not None else self.cmd.get()).strip()
        if not line:
            return
        if text is None:
            self.cmd.set("")
        if self.stream is not None:
            self.stream.send(line)
        else:
            # Not streaming: talk to the port directly through a throwaway
            # LiveStream so the console works before Start is ever pressed.
            port = self.port.get().strip()
            if not port:
                self._con_write(["pick a COM port"])
                return
            tmp = wv.LiveStream(port, "", self.knobs(), panel=self.panel_key())
            tmp.send(line)
            self._con_write(list(tmp.conlog))

    def _con_write(self, lines) -> None:
        self.con.configure(state="normal")
        for ln in lines:
            self.con.insert("end", ln + "\n")
        self.con.see("end")
        self.con.configure(state="disabled")

    def _clear_con(self) -> None:
        self.con.configure(state="normal")
        self.con.delete("1.0", "end")
        self.con.configure(state="disabled")
        self._con_n = 0
        if self.stream is not None:
            self.stream.conlog.clear()

    def _pump_con(self) -> None:
        if self.stream is None:
            return
        log = self.stream.conlog
        if len(log) > self._con_n:
            new = list(log)[self._con_n:]
            self._con_write(new)
            self._con_n = len(log)

    def _refresh_eq_labels(self) -> None:
        panel = self.panel_key()
        cur = int(self.eq_min.get() or 15)
        if cur not in wv.EQUALIZE_MINS:
            cur = 15
            self.eq_min.set(15)
        labels = [wv.equalize_label(m, panel) for m in wv.EQUALIZE_MINS]
        self.eq_box["values"] = labels
        self.eq_box.set(wv.equalize_label(cur, panel))

    def eq_minutes(self) -> int:
        text = (self.eq_box.get() or "").strip()
        try:
            n = int(text.split()[0])
        except (ValueError, IndexError):
            n = int(self.eq_min.get() or 15)
        if n not in wv.EQUALIZE_MINS:
            n = 15
        self.eq_min.set(n)
        return n

    def panel_key(self) -> str:
        lab = self.panel.get()
        return next((k for k, lb in PANELS if lb == lab), "auto")

    def refresh_ports(self) -> None:
        ports = com_ports()
        self.port_box["values"] = ports
        if ports and self.port.get() not in ports:
            self.port.set(ports[0])

    def browse(self) -> None:
        path = filedialog.askopenfilename(
            parent=self,
            title="Video",
            filetypes=[
                ("Media",
                 "*.mp4 *.mkv *.mov *.avi *.webm *.m4v "
                 "*.gif *.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff"),
                ("Video", "*.mp4 *.mkv *.mov *.avi *.webm *.m4v"),
                ("Images / GIF",
                 "*.gif *.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff"),
                ("All", "*.*"),
            ],
        )
        if path:
            self.video.set(path)
            self.src_kind.set("file")

    def pick_screen(self) -> None:
        box = wv.pick_screen_region(parent=self)
        if box is None:
            self.status.set("region cancelled")
            return
        self._screen_box = box
        self.src_kind.set("screen")
        self.screen_l.configure(
            text="%d,%d  %d×%d" % (box[0], box[1], box[2] - box[0], box[3] - box[1])
        )

    def knobs(self):
        return wv.Knobs(Namespace(
            drive=int(self.drive.get()),
            gamma=max(0.5, int(self.gamma.get()) / 100.0),
            contrast=max(0.4, int(self.contrast.get()) / 100.0),
            highlights=255,
            knee=150.0,
            white=225,
            black=40,
            dither=int(self.dither.get()),
            bias=int(self.bias.get()),
            bb=int(self.bb.get()),
            ww=0,  # white re-drive turns the pixels black; see the handoff
            invert=False,
            no_mirror=True if self.panel_key() == "crow-579" else False,
            fit=bool(self.fitv.get()),
            zoom=int(self.zoom.get()),
            pad=int(self.padv.get()),
            landscape=False,
            test=False,
            panel=self.panel_key(),
            screen=self.src_kind.get() == "screen",
            region=self._screen_box,
            equalize_min=self.eq_minutes() if self.src_kind.get() == "equalize" else 0,
        ))

    def apply_knobs_extra(self, k):
        k.trail = int(self.trail.get())
        k.chase = int(self.chase.get())
        k.volt_i = int(self.volt.get())
        k.bb = int(self.bb.get())
        k.ww = 0
        k.paused = bool(self.pause_v.get())
        return k

    def toggle(self) -> None:
        if self.stream is not None and self.stream.running:
            self.stop()
        else:
            self.start()

    def start(self) -> None:
        port = self.port.get().strip()
        if not port:
            self.status.set("pick a COM port")
            return
        kind = self.src_kind.get()
        if kind == "file":
            src = self.video.get().strip().strip('"')
            if not src or not Path(src).is_file():
                self.status.set("pick a video, gif or image, or switch to Camera")
                return
        elif kind == "screen":
            src = "screen"
            if self._screen_box is None:
                self.pick_screen()
            if self._screen_box is None:
                self.status.set("pick a screen region")
                return
        elif kind == "equalize":
            src = "equalize"
        else:
            src = self.camera.get().strip() or "1"
        k = self.apply_knobs_extra(self.knobs())
        save_cfg({
            "port": port,
            "camera": self.camera.get(),
            "video": self.video.get().strip() if kind == "file" else "",
            "screen": kind == "screen",
            "panel": self.panel_key(),
            "drive": k.drive,
            "trail": k.trail,
            "chase": k.chase,
            "bb": k.bb,
            "fit": int(bool(k.fit)),
            "zoom": k.zoom,
            "pad": k.pad,
            "dither": k.dither,
            "equalize_min": self.eq_minutes(),
        })
        self.stream = wv.LiveStream(port, src, k, panel=self.panel_key())
        self.stream.start()
        self.btn.configure(text="Stop")
        self.status.set("sending…")
        self._pause_buttons()

    def stop(self) -> None:
        if self.stream is not None:
            self.stream.stop()
            t0 = time.time()
            while self.stream.running and time.time() - t0 < 10:
                self.update_idletasks()
                time.sleep(0.05)
        self.stream = None
        self.btn.configure(text="Send")
        self.status.set("stopped")
        self._pause_buttons()

    def white(self) -> None:
        if self.stream and self.stream.running:
            self.stream.request_white()

    def park(self, color: str) -> None:
        """8 full-refresh flashes at drive 12, then hold white or black."""
        port = self.port.get().strip()
        if not port:
            self.status.set("pick a COM port")
            return
        if self._parking:
            return
        if self.stream is not None:
            self.stop()
        self._parking = True
        self.status.set("parking %s — 8 flashes at drive 12…" % color)
        panel = self.panel_key()

        def work():
            def log(msg):
                self.after(0, lambda m=msg: self._con_write([m]))

            err = wv.park_panel(port, color=color, cycles=8,
                                panel=panel, log=log)

            def done():
                self._parking = False
                self.status.set(err or ("parked %s" % color))
                if err:
                    self._con_write([err])

            self.after(0, done)

        threading.Thread(target=work, daemon=True).start()

    def _rearm(self, *_a) -> None:
        if self.stream is None or not self.stream.running:
            return
        k = self.stream.knobs
        k.drive = int(self.drive.get())
        k.trail = int(self.trail.get())
        k.chase = int(self.chase.get())
        k.volt_i = int(self.volt.get())
        k.bb = int(self.bb.get())
        k.ww = 0
        self.stream.request_rearm()

    def _frame(self, *_a) -> None:
        """Framing is read per frame by the encoder, so no rearm is needed."""
        if self.stream is None or not self.stream.running:
            return
        k = self.stream.knobs
        k.fit = bool(self.fitv.get())
        k.pad = int(self.padv.get())
        k.zoom = int(self.zoom.get())

    def _tone(self, *_a) -> None:
        if self.stream is None or not self.stream.running:
            return
        k = self.stream.knobs
        k.dither = int(self.dither.get())
        k.bias = int(self.bias.get())
        k.gamma = max(0.5, int(self.gamma.get()) / 100.0)
        k.contrast = max(0.4, int(self.contrast.get()) / 100.0)
        self.stream.request_tone()

    def _pause_buttons(self) -> None:
        on = (self.stream is not None and self.stream.running
              and bool(self.pause_v.get()))
        st = "normal" if on else "disabled"
        self.btn_hold_w.configure(state=st)
        self.btn_hold_b.configure(state=st)

    def _pause(self) -> None:
        if self.stream and self.stream.knobs:
            self.stream.knobs.paused = bool(self.pause_v.get())
        self._pause_buttons()

    def hold_out(self, color: str) -> None:
        """Pause hold: vendor full-clear at drive 12, then white or black.

        Unpausing re-enters the stream (white seed + frozen frame) so the
        picture comes back instead of staying parked.
        """
        if self.stream is None or not self.stream.running:
            self.status.set("Send first, then Pause")
            return
        if not self.pause_v.get():
            self.pause_v.set(True)
            self._pause()
        self.stream.request_hold(color)
        self.status.set("holding %s…" % color)

    def _on_seek(self, _s) -> None:
        pass

    def _seek_release(self, _e) -> None:
        self._seek_user = False
        if self.stream is None:
            return
        self.stream.seek_frac(float(self.seek.get()) / 1000.0)

    def _tick(self) -> None:
        st = self.stream
        if st is not None:
            if st.error and not st.running:
                self.status.set(st.error)
                self.btn.configure(text="Send")
                self.stream = None
                self._pause_buttons()
            elif st.running:
                self.status.set(st.status or "streaming")
                pos, n = st.position()
                fps = st.cam.src_fps if st.cam else 0.0
                self.time_l.configure(
                    text="%s / %s" % (fmt_time(pos, fps), fmt_time(n, fps))
                )
                if n > 0 and not self._seek_user:
                    self.seek.set(1000.0 * pos / n)
                self._pause_buttons()
            elif not st.running and self.btn["text"] == "Stop":
                self.btn.configure(text="Send")
                self.status.set(st.status or "stopped")
                self.stream = None
                self._pause_buttons()
        self._pump_con()
        self.after(250, self._tick)

    def on_close(self) -> None:
        self.stop()
        self.destroy()


if __name__ == "__main__":
    wv.make_dpi_aware()
    App().mainloop()
