// CrowPanel 5.79" — circle motion / plane-diff / custom-waveform bench
//
// Part 1 (modes 1-4, 8-10): plane strategy and the 0x22 sequence byte only.
// Vendor OTP waveforms throughout, so a bad mode cannot brick or mis-seed the
// panel — worst case is an ugly frame.
//
// Part 2 (modes 5-7): custom waveforms written to LUT register 0x32 / 0xB2.
// Volatile register state only — never OTP burn (0x30). Any 0x22 byte carrying
// a Load-LUT bit (0xDC / 0xF7 / 0xFF / 0x9x / 0xBx) throws a custom LUT away and
// restores the factory one, so recovery is always one mode change away.
//
// Why the vendor partial costs 336 ms, from the SSD1683 datasheet (see
// waveform.h for the full decode): BUSY = frames x frame period. The frame count
// lives in the LUT's TP/SR/RP fields and the period comes from LUT byte 224 (FR),
// capped at 125 Hz = 8 ms. Nothing the host does to RAM can shorten it — which is
// exactly what the "BUSY is content-independent, min == med == max" result said.
// So the only lever is the LUT itself, and `calib` measures the law directly.
//
// Two circles animate together so fill and thin-line rendering can be compared
// under the same waveform — thin outlines are where fast waveforms fail first.
//
// Controls (buttons around the jog wheel):
//   RIGHT (GPIO2) = run        LEFT (GPIO1) = stop
//   SEL click (GPIO5) = next mode
//   wheel UP (GPIO6) / DOWN (GPIO4) = prev / next mode
//   Buttons are sampled by a core-0 task so presses during BUSY are never lost.
//
// Serial 115200: `help`.
//
// Build: python -m platformio run -d . -e wavelab

#include <Arduino.h>
#include <string.h>

#include "EPD.h"
#include "EPD_Init.h"
#include "spi.h"
#include "waveform.h"

// ---- board ------------------------------------------------------------------
#define PIN_PWR 7
#define BTN_BACK 1  // left
#define BTN_RUN 2   // right
#define BTN_DOWN 4
#define BTN_SEL 5
#define BTN_UP 6

// Portrait logical canvas (Rotation 90): x = 0..271, y = 0..791
#define PW 272
#define PH 792

// ---- animation geometry -----------------------------------------------------
// Ms Pac-Man box size in pixels; the sprite is drawn to fill it. `box N` changes
// it live so the render cost can be measured against fps at several sizes.
#define CIRCLE_R_DEFAULT 16  // 16 -> a 32x32 box
// One solid disc. The outline circle was there to show thin-line rendering, but
// two shapes moving at once makes it impossible to attribute an artefact to the
// waveform rather than to the other disc having just passed.
#define NBALL 1

static int g_radius = CIRCLE_R_DEFAULT;
// PERCENT of one shape-length per frame, not pixels per axis. 100 = the half
// disc advances by exactly its own extent along the direction it is travelling.
static int g_speed = 100;

// Bar-sweep height, as a PERCENT of the sprite box. The step always equals the bar
// height, so coverage stays exactly-once at any setting.
//
// 80 for now, on glass: at 100 the swept background came out too dark. The likely
// mechanism is not the height itself but the pairing — more drive frames make the
// bar's black DEEPER, so the single white pass that follows recovers less of it and
// parks darker. That predicts the needed height falls as drive rises, which matches
// what was seen. `bar N` to characterise it.
static int g_bar_pct = 80;

// Two balls free-roaming the whole panel, text included. Velocity ratios are
// deliberately coprime-ish so they do not fall into lockstep and keep sweeping
// fresh ground over the text block.
static const int kRatioX[NBALL] = {4};
static const int kRatioY[NBALL] = {10};

static int bx[NBALL], by[NBALL];
static int bsx[NBALL], bsy[NBALL];  // direction signs

// Extent along the direction of travel, in pixels.
//
// The starfish fits inside its bounding circle, so 2R is a conservative depth: it
// guarantees no overlap between consecutive positions whatever angle the arms
// happen to be at. The true along-travel extent is ~1.8R (a tip leads, and the
// two rear arms sit 36 degrees off dead astern), so this leaves a small gap rather
// than a small overlap — the harmless direction to err in here.
static int shape_depth(void) { return 2 * g_radius; }

// Per-frame displacement MAGNITUDE, in pixels.
//
// At speed 100 this is exactly shape_depth() — the sprite's extent along its own
// direction of travel — so consecutive positions abut and NEVER overlap. Every
// frame therefore paints entirely fresh pixels, and no pixel is asked to hold the
// same colour two frames running.
//
// Derived from the shape rather than tuned by hand, so the property survives
// `radius N`.
static int step_mag(void) {
  int m = (shape_depth() * g_speed + 99) / 100;  // round up, see step_axis
  return m < 1 ? 1 : m;
}

// Split the magnitude across the two axes so the RESULTANT is step_mag(). The old
// code set the dominant axis directly, which made the true displacement
// sqrt(x^2 + y^2) — 27.9 px when it read 26 — and that is the number that has to
// match the shape, not either component.
//
// Rounded UP: overshooting leaves a hair of white between successive positions,
// undershooting overlaps them. Only one of those two failures is harmless here.
static int step_axis(int ratio, int other) {
  float h = sqrtf((float)(ratio * ratio + other * other));
  if (h <= 0.0f) return 1;
  int v = (int)ceilf((float)step_mag() * (float)abs(ratio) / h);
  return v < 1 ? 1 : v;
}
static int step_x(int i) { return step_axis(kRatioX[i], kRatioY[i]); }
static int step_y(int i) { return step_axis(kRatioY[i], kRatioX[i]); }

// ---- modes ------------------------------------------------------------------
// old_plane: 0 = leave OLD alone (LongTris baseline)
//            1 = OLD = previous frame  (true differential)
//            2 = OLD = ~current frame  (false diff / flash-free full repaint)
enum : uint8_t { OLD_NONE = 0, OLD_PREV = 1, OLD_INV = 2, OLD_TRAIL = 3 };

// Runtime override for the mode's OLD-plane strategy; -1 = use the mode's own.
//
// Exists to test whether the OLD write is needed AT ALL. Both planes are 27200
// bytes and we send both every frame, which is the whole 13 ms of SPI. If the
// controller promotes NEW to OLD itself when DISPLAY completes, then in plain
// true-diff the OLD write is redundant and the transfer halves. The SSD1677
// sibling has no plane-copy and must rewrite it; whether this part behaves the
// same was never checked. `old 0` answers it — a wrong guess shows up instantly
// as inversion or garbage on the glass, not as a subtle artefact.
static int8_t g_old_override = -1;

// ---- trail chasing ----------------------------------------------------------
//
// A pixel the disc has moved off is a B->W transition. It gets `drive` frames of
// the to-white rail, lands somewhere short of white, and is then abandoned:
// NEW == OLD from the next frame on, so the driver classifies it unchanged and
// never touches it again. That is why every mode leaves a ~50% grey trail no
// matter how long you wait — it is not a slow fade, it is a stalled transition.
//
// Fix: remember which pixels changed over the last N frames and keep asserting
// them as "changed" in the OLD plane. Each one then gets N drives instead of one.
// Costs NO waveform time — same LUT, same frame count, same 8.11 ms/frame. The
// only cost is the OLD-plane write, which a true differential already pays.
#define TRAIL_MAX 8
#define PLANE_BYTES ((uint32_t)EPD_H * FB_WBYTES)

static uint8_t *g_hist[TRAIL_MAX];  // ring of per-frame change masks (PSRAM)
// These two are read byte-by-byte inside the SPI pump, so they live in INTERNAL
// RAM. Holding them in PSRAM cost ~14 ms per frame (95 -> 109 ms of SPI) purely
// in bus stalls. The history ring stays in PSRAM: it is only touched during the
// once-per-frame sequential rebuild, where the slower bus does not show.
static uint8_t g_trail_mask_buf[PLANE_BYTES];  // OR of the last g_trail frames
static uint8_t g_span_mask_buf[PLANE_BYTES];   // OR of the whole ring
static uint8_t *g_trail_mask = g_trail_mask_buf;
static uint8_t *g_span_mask = g_span_mask_buf;
static uint8_t g_hist_pos = 0;
static uint8_t g_trail = 4;         // frames to keep chasing; 0 = plain true-diff

// ---- windowed transfer ------------------------------------------------------
//
// BUSY does not care about the window — the gate scan is always full-panel, so
// the waveform costs the same. What shrinks is the SPI: currently two whole
// planes, 2 x 27200 bytes, 95 ms of bit-bang for a disc that touches a fraction
// of the glass.
//
// The window is a FIXED SIZE that moves, never one that grows to fit. A window
// sized to the content gives a frame time that changes with the content, and for
// a game an occasional slow frame is worse than a consistently slower one. Size
// it once for the largest thing that moves and the cost is then constant.
//
// Safety condition: the window must cover every pixel whose OLD still disagrees
// with NEW, or that pixel keeps being re-driven forever with no way to settle it.
// That is why the window is centred on the span of the ENTIRE history ring, not
// just the g_trail frames currently being chased — pixels stay covered for
// longer than they stay hot.
// Works, and cuts the transfer from 54400 bytes to ~14000 (SPI 101 -> ~20 ms).
// Off at boot only so a reset always lands on the known-good full path.
//
// IMPORTANT: after reflashing, POWER-CYCLE the panel before judging this. An
// esptool reset restarts the ESP32 but does not drop the panel's supply, so the
// COG keeps analog/LUT state from the previous firmware and the first windowed
// frames render as a correct rectangle surrounded by glitching. That is stale
// COG state, not a window bug — it cost several rounds of chasing a fault that
// was not in the code. setup() now forces the rails down before init to make
// this self-correcting.
// 0 = off (full transfer), 1 = SAFE band, 2 = TIGHT rectangle.
//
// SAFE is the configuration that actually worked on glass: keep every row and
// run the columns from the content's leading edge out to the panel edge. That
// band always contains the HUD (which lives in the high columns) and everything
// between it and the moving content, so no pixel can be left hot outside the
// window — which is the failure mode TIGHT keeps hitting. It saves less, and it
// varies with content position, but it is correct.
//
// TIGHT is the fixed-size rectangle. Nearly twice as fast again and still
// unexplained — see docs §7d.
static uint8_t g_win_mode = 0;
#define g_win_on (g_win_mode != 0)
// 36 byte columns = 288 px. Sized from what the span actually needs: the disc
// travels ~3.25 byte columns per frame and the history ring is 8 deep, so the
// span reaches ~26 columns of travel plus ~9 of disc. Anything smaller and the
// window grows on its own, which is exactly the variable frame time this was
// meant to avoid. For a real app: piece footprint + travel x (trail + 2).
static uint8_t g_win_cols = 36;
static uint16_t g_win_rows = 208;  // gate rows
// The HUD lives in a fixed strip of native byte columns (portrait y maps to
// native x, so the text at the top of the screen sits at the high column end).
// It is not part of the moving content, so the disc window never covers it.
#define HUD_C0 84
#define HUD_C1 (FB_WBYTES - 1)
// Frames for which the HUD strip must keep being transferred after its text
// changes. Must outlast the history ring, or the pixels it left mid-transition
// are never given OLD = NEW and re-drive forever — which is exactly the
// flashing seen whenever the window was small enough to be worth having.
static uint8_t g_hud_frames = 0;

// Every Nth windowed frame writes the WHOLE panel instead of a window.
//
// Reasoning straight off the glass: a region that gets written comes out correct
// and stays correct — junk only ever sits in regions nothing has written lately,
// and sweeping the moving content over it cures it for good. So rather than keep
// hunting why the band manufactures stale pixels at its edges, guarantee that no
// region can go unwritten for longer than N frames. Amortised cost is one full
// transfer spread over N frames: at N=8 that is ~13 ms/frame on bit-bang, ~6 ms
// on hardware SPI.
static uint8_t g_win_sync = 8;
static uint8_t g_win_sync_ctr = 0;

// "Fake crossing": frames during which the WHOLE panel is driven as if every
// pixel had just changed (OLD = ~NEW, bb_epaper's PLANE_FALSE_DIFF).
//
// A plain full write only refreshes RAM — it does not drive anything, because
// pixels that match are classified unchanged. That is why the periodic sync
// removed the junk but left a dark grey ghost: those pixels were corrected in
// RAM but never actually pushed to their target. The moving disc clears them
// properly only because trail chasing drives each pixel several frames running.
// So do the same thing deliberately: N consecutive full-panel drives. No GC
// flash — this is the differential waveform doing a complete repaint.
static uint8_t g_flush_frames = 0;

static uint32_t g_win_bytes = 0;   // last frame's transferred byte count
static uint8_t g_win_c0 = 0, g_win_c1 = 0;      // last window, for the log
static uint16_t g_win_r0 = 0, g_win_r1 = 0;

// Which waveform is in the LUT registers while a mode runs.
enum : uint8_t {
  WV_OTP3 = 0,   // factory LUT, 3-color bank (0x91) — what the vendor code loads
  WV_OTPBW = 1,  // factory LUT, black/white bank (0x99)
  WV_CUSTOM = 2,  // parametric table from WaveParams
  WV_GD = 3,      // Good Display 26-frame partial table
  WV_PROBE = 4,   // one LUT block only — identifies the transition mapping
};

struct ModeInfo {
  const char *name;
  uint8_t seq22;     // 0xDC vendor partial, 0xCF documented b/w display, 0xC7 3-color
  uint8_t old_plane;
  uint8_t wave;      // WV_*
  uint8_t passes;    // repeat write+update N times (black saturation)
  bool auto_clean;   // periodic FALSEDIFF cleanup during animation
  bool manual;       // step-driven, does not free-run
  const char *note;
};

// The datasheet 0x22 table finally explains the dead 0xC7 modes measured on
// 2026-08-06: 0xC7 is "Display (3-color mode)" and 0xCF is "Display (b/w mode)".
// 0xC7 was never a fast path — it was the wrong colour mode on a b/w panel.
// Those four modes are dropped; 0xCF replaces them as the real comparison.
static const ModeInfo kModes[] = {
    {"PART_DC",      0xDC, OLD_NONE, WV_OTP3,  1, false, false, "baseline: LongTris play path"},
    {"TRUEDIFF_DC",  0xDC, OLD_PREV, WV_OTP3,  1, false, false, "best known — sharpest edges"},
    {"OTP3_CF",      0xCF, OLD_PREV, WV_OTP3,  1, false, false, "same LUT, documented b/w display byte"},
    {"OTPBW_CF",     0xCF, OLD_PREV, WV_OTPBW, 1, false, false, "factory B/W LUT bank (0x99) — untried"},
    // 0x0C on the custom-LUT modes: rails held up, no ramp, no fixed cost. It is
    // a property of the mode, not something to type in — a manual global did not
    // survive a reset, silently cost 157 ms, and worse, followed the user into an
    // OTP mode where it hangs the panel outright.
    // OLD_NONE, not OLD_TRAIL: the SSD1683 promotes NEW to OLD itself when DISPLAY
    // completes, so the OLD write is redundant in true-diff — 8 ms of the 13 ms of
    // SPI, for nothing. `old 3` restores trail chasing at runtime.
    {"CUSTOM_CF",    0x0C, OLD_NONE, WV_CUSTOM, 1, false, false, "parametric LUT — see `wave`"},
    {"GD26_CF",      0x0C, OLD_PREV, WV_GD,    1, false, false, "Good Display 26-frame, both chips"},
    {"PROBE_CF",     0x0C, OLD_PREV, WV_PROBE, 1, false, false, "drive one LUT block — see `probe`"},
    {"FALSEDIFF_DC", 0xDC, OLD_INV,  WV_OTP3,  1, false, false, "OLD=~new, 1 pass, no GC"},
    {"TD_CLEAN",     0xDC, OLD_PREV, WV_OTP3,  1, true,  false, "truediff + periodic falsediff clean"},
    {"HALFSPLIT",    0xDC, OLD_NONE, WV_OTP3,  1, false, true,  "per-chip activate 0xA2/0xA0 test"},
};
static const uint8_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
static uint8_t g_mode = 0;

static const ModeInfo &mode_info(void) { return kModes[g_mode]; }

// ---- waveform state ---------------------------------------------------------
static WaveParams g_wp;
static uint8_t g_ws[WAVE_BYTES];
// Both cascade chips must hold the same LUT. Writing the master only leaves the
// two halves on different waveforms, which is the most likely reason the earlier
// "GD phases only" attempt came back dark and soft.
static bool g_dual = true;
static uint8_t g_gd_fr = 0x05;    // GD ships 0x08, which is not a defined FR code
// 0x22 is a bitfield, not the opaque menu the datasheet prints it as. Decoding
// the documented combinations gives:
//   b7 enable clock   b6 enable analog   b5 load temperature   b4 load LUT
//   b3 colour mode (0 = 3-color, 1 = b/w)   b2 DISPLAY
//   b1 disable analog b0 disable clock
// which explains every measurement so far: 0xC7 is DISPLAY in 3-color mode (the
// "fast" byte was never fast, it was the wrong colour mode), 0xCF is the same in
// b/w, and the vendor's 0xDC is 0xCF without the two power-down bits — it leaves
// the rails up between partials. Note b4 is CLEAR in 0xDC, so contrary to the
// old note in the handoff it should NOT reload the OTP LUT.
//
// calib puts a number on why that matters: a 0xCF update costs ~91 ms of fixed
// analog ramp before a single waveform frame runs. Anything under ~130 ms is
// unreachable while every frame pays that ramp. g_seq lets the activate byte be
// chosen at runtime so the ramp can be taken out of the loop.
static uint8_t g_seq = 0;  // 0 = use the mode's byte
// Rail state now lives in the DRIVER (EPD_AnalogIsOn / EPD_AnalogPower). It was
// a local bool here, and nothing cleared it when the busy-timeout recovery path
// re-inited the panel — so the flag said "up" while the rails were down and
// every following 0x0C silently did nothing. Only the driver sees every reset.
static uint8_t eff_seq(void);   // defined with the rest of the activate handling
static void trail_reset(void);  // defined with the trail-chasing helpers
static void fps_reset(void);    // defined with the FPS meter

static uint8_t g_probe_block = WB_LUTWB;
static uint8_t g_probe_level = VS_VSL;
static uint8_t g_probe_tp = 14;
static bool g_wave_loaded = false;

// ---- framebuffers -----------------------------------------------------------
static uint8_t ImageBW[EPD_H * FB_WBYTES];    // current frame
static uint8_t ImagePrev[EPD_H * FB_WBYTES];  // previous frame (TRUEDIFF)

// ---- state ------------------------------------------------------------------
static bool g_running = false;
static bool g_step_once = false;
static uint32_t g_frame = 0;
static uint32_t g_last_spi_us = 0;
static uint32_t g_last_trail_us = 0;  // trail-mask rebuild, was hidden inside spi_us
static uint32_t g_last_busy_us = 0;
static uint32_t g_last_tot_us = 0;
static bool g_last_to = false;
static uint32_t g_mode_frames = 0;
static uint64_t g_mode_busy_sum = 0;
static bool g_last_clean = false;
static uint16_t g_clean_every = 16;  // TD_CLEAN: scrub every N frames (0 = off)
// Print one RESULT line every N frames (0 = silent).
//
// 32 by default, not 1. Measured at mode 1: printing every frame costs 10.3 ms of
// a 62 ms frame — 16.1 fps against 19.3 with it throttled — because the line is
// ~170 chars and 115200 baud only moves 11.5 KB/s. At 32 the amortised cost is
// 0.3 ms, which is indistinguishable from off, and the log stays usable.
// `result 0` for true silence, `result 1` to reproduce the slow baseline.
static uint16_t g_result_every = 32;
static uint8_t g_clean_passes = 2;   // 1 pass leaves black unsaturated

static char g_ser_line[96];
static uint8_t g_ser_len = 0;

// Queued UI actions — set by the core-0 poll task, drained in loop().
enum Act : uint8_t { ACT_NONE = 0, ACT_RUN, ACT_STOP, ACT_MODE_DN, ACT_MODE_UP, ACT_WIN };
static volatile uint8_t g_act = ACT_NONE;

static void queue_act(uint8_t a) {
  if (a == ACT_NONE) return;
  // Stop always wins — it is the "get me out of here" button.
  if (g_act == ACT_STOP) return;
  g_act = a;
}

// ---- buttons ----------------------------------------------------------------
static bool btn_down(uint8_t pin) { return digitalRead(pin) == LOW; }

// Poll on core 0 every 5 ms so a press during a ~336 ms BUSY is still caught.
static void btn_task(void *) {
  const uint8_t pins[5] = {BTN_BACK, BTN_RUN, BTN_DOWN, BTN_SEL, BTN_UP};
  bool prev[5] = {false, false, false, false, false};
  uint32_t last_ms[5] = {0, 0, 0, 0, 0};

  for (;;) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < 5; i++) {
      // TWO consecutive lows, not one. A GC clear ramps the panel's high-voltage
      // rails hard enough to glitch these pins low for a single 5 ms sample, and a
      // spurious BTN_DOWN is ACT_STOP — which silently halted the animation right
      // after every mode change and looked exactly like a firmware crash.
      bool now_down = btn_down(pins[i]);
      if (now_down) {
        delayMicroseconds(300);
        now_down = btn_down(pins[i]);
      }
      if (now_down && !prev[i] && (now - last_ms[i]) > 180) {
        last_ms[i] = now;
        switch (pins[i]) {
          // The two SIDE buttons cycle the mode, because that is the control
          // being exercised constantly while judging waveforms and the side
          // buttons are the ones you can find without looking.
          case BTN_BACK: queue_act(ACT_MODE_DN); break;  // left  — mode down
          case BTN_RUN:  queue_act(ACT_MODE_UP); break;  // right — mode up
          // Run/stop moved onto the toggle, which had two duplicate stops.
          case BTN_UP:   queue_act(ACT_RUN);  break;
          case BTN_DOWN: queue_act(ACT_STOP); break;
          // SEL toggles windowed transfer. Without it the windowed path can only
          // be reached over serial, which makes it untestable from the bench.
          // The total line shows "W" while it is on, so the regime is never a
          // guess.
          case BTN_SEL:  queue_act(ACT_WIN);  break;
        }
      }
      prev[i] = now_down;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---- paint ------------------------------------------------------------------
// Point the Paint layer at a buffer WITHOUT clearing it. Needed because the maze
// template is rasterised into its own buffer and then blitted, so the target has to
// be switched back and forth without destroying either side.
static void paint_bind(uint8_t *buf) {
  Paint_NewImage(buf, EPD_W, EPD_H, Rotation, WHITE);
}

static void paint_begin(void) {
  paint_bind(ImageBW);
  Paint_Clear(WHITE);
}

// What is on the glass RIGHT NOW, in 24 px. A sweep changes the waveform every
// few frames, so without this there is no way to tell from the panel which
// setting you are actually looking at.
static char g_label[40] = "";

// Exact, not estimated: frames x frame period, both read straight back out of
// the loaded table. Known BEFORE the update fires, unlike g_last_busy_us, which
// always shows the previous frame's cost and so lags a sweep by one step.
static uint32_t predicted_busy_ms(void) {
  if (!g_wave_loaded) return 0;
  return (uint32_t)((uint64_t)wave_frames(g_ws) * wave_fr_period_us(g_ws[224]) / 1000ULL);
}

// A fix comparison owns the banner: while one is running the label names the
// candidate, not the drive value, so there is never any doubt about what is on
// the glass. Empty override = normal behaviour.
static char g_label_override[40] = "";
static char g_note[48] = "";
static char g_note2[48] = "";

// Waveform time is only part of the frame. The plane writes happen first and
// cost 86 ms of bit-bang SPI regardless of the waveform, so at MODE 10 the
// panel updates every ~191 ms, not every 105 ms. Showing only the waveform
// number overstates the speed by nearly half.
static uint32_t total_frame_ms(uint32_t wave_ms) {
  return wave_ms + (uint32_t)(g_last_spi_us / 1000UL);
}

// The banner must describe the mode you are actually in. `drive` only means
// something to the parametric waveform: showing "DRIVE 14" while GD26 or an OTP
// bank is on the glass is a leftover from the previous mode and a lie.
static void set_label(void) {
  if (g_label_override[0]) {
    snprintf(g_label, sizeof(g_label), "%s", g_label_override);
    return;
  }
  unsigned long ms = (unsigned long)predicted_busy_ms();
  switch (mode_info().wave) {
    case WV_CUSTOM:
      // The drive frame count IS the mode number. Nothing else differs between
      // them — same LUT shape, same activate byte, same voltages, same rate.
      snprintf(g_label, sizeof(g_label), "MODE %u", (unsigned)g_wp.drive_tp);
      break;
    case WV_GD:
      snprintf(g_label, sizeof(g_label), "GD26");
      break;
    case WV_PROBE:
      snprintf(g_label, sizeof(g_label), "PROBE blk%u", (unsigned)g_probe_block);
      break;
    default:  // OTP banks — frame count lives in ROM, so there is nothing to predict
      snprintf(g_label, sizeof(g_label), "%s", mode_info().name);
      ms = (unsigned long)(g_last_busy_us / 1000UL);
      break;
  }
  char prev_label[sizeof(g_label)], prev_note[sizeof(g_note)], prev_note2[sizeof(g_note2)];
  memcpy(prev_label, g_label, sizeof(prev_label));
  memcpy(prev_note, g_note, sizeof(prev_note));
  memcpy(prev_note2, g_note2, sizeof(prev_note2));

  snprintf(g_note, sizeof(g_note), "%lu ms wave", ms);
  // Exact. This used to be rounded to 5 ms so a changing digit could not drag
  // the transfer window across the HUD — a windowed-mode concern, and windowed
  // mode is off. The readout is frozen a few frames after any change anyway, so
  // there is nothing left for the rounding to protect and it only made the
  // number disagree with the arithmetic.
  uint32_t tot = total_frame_ms(ms);
  snprintf(g_note2, sizeof(g_note2), "%lu ms total%s", (unsigned long)tot,
           g_win_mode == 1 ? " W" : (g_win_mode == 2 ? " T" : ""));

  // Any change to the text makes those pixels transition, so the strip has to
  // stay in the transfer long enough to be driven AND then settled.
  if (strcmp(prev_label, g_label) || strcmp(prev_note, g_note) ||
      strcmp(prev_note2, g_note2))
    g_hud_frames = TRAIL_MAX + 2;
}

// ---- FPS meter --------------------------------------------------------------
// Measured from the WALL CLOCK across whole frames, so it counts everything the
// panel really spends — plane writes, waveform, render, the lot. Deriving it from
// `tot_ms` instead would only restate the arithmetic already on the line above,
// and would silently omit anything not inside the timed region.
//
// Refreshed on a 1 Hz clock and NOTHING else, which is the one rule this readout
// has to obey: nothing DOWNSTREAM of the text may decide when the text updates.
// Frame time depends on whether the HUD sits in the transfer window, so
// "refresh when the fps digits change" is exactly the self-amplifying loop that
// once stretched the window from 17 KB to 39 KB per frame. millis() is upstream
// of all of it and cannot feed back.
#define HUD_FPS_SIZE 24
static char g_fps[16] = "";      // panel text, carries its own " fps" unit
static char g_fps_num[8] = "";   // bare number, for the RESULT line
static uint32_t g_fps_t0 = 0;
static uint32_t g_fps_frames = 0;

// Called on any regime change: the frames either side of one are not the same
// regime, and averaging across the boundary reports a rate that was never on the
// glass.
//
// The TEXT is cleared too, not just the accumulator. Keeping the old string
// visible until the next second elapsed meant the panel read "MODE 5" beside the
// rate for mode 10 — the banner updates within a few frames and the meter took a
// full second, so for that second the two lines disagreed. Blank is honest;
// wrong is not. Costs one extra HUD transition per mode change, and a mode change
// already GC-clears.
static void fps_reset(void) {
  g_fps_t0 = 0;
  g_fps_frames = 0;
  if (g_fps[0]) {
    g_fps[0] = '\0';
    g_fps_num[0] = '\0';
    g_hud_frames = TRAIL_MAX + 2;  // the cleared glyphs have to settle as well
  }
}

static void fps_tick(void) {
  uint32_t now = millis();
  if (!g_fps_t0) {  // first frame of a window carries no interval yet
    g_fps_t0 = now;
    g_fps_frames = 0;
    return;
  }
  g_fps_frames++;
  uint32_t dt = now - g_fps_t0;
  if (dt < 1000) return;

  // One decimal place. Whole numbers are too coarse here — mode 5 and mode 6 are
  // 11.4 and 10.4 fps, which both round to 11.
  uint32_t f10 = (g_fps_frames * 10000UL + dt / 2) / dt;
  char num[sizeof(g_fps_num)];
  snprintf(num, sizeof(num), "%lu.%lu", (unsigned long)(f10 / 10),
           (unsigned long)(f10 % 10));
  char next[sizeof(g_fps)];
  snprintf(next, sizeof(next), "%s fps", num);
  if (strcmp(next, g_fps)) {
    memcpy(g_fps, next, sizeof(g_fps));
    memcpy(g_fps_num, num, sizeof(g_fps_num));
    // Changed text is changed pixels, so the strip must stay in the transfer long
    // enough to be driven AND settled — the same rule set_label() follows.
    g_hud_frames = TRAIL_MAX + 2;
  }
  g_fps_t0 = now;
  g_fps_frames = 0;
}

// Two lines. Everything else that used to be here — seq bytes, plane strategy,
// frame counts, SPI split, voltage registers — is diagnostic detail that belongs
// on serial where it can be read at leisure, not on the panel being judged.
// Ordered 4x4 Bayer, shared by the ghost shading and the circle field. Positionally
// fixed, so a shape dithers to the same pattern wherever it lands and the greys read
// as texture rather than noise.
static const uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

static void fill_disc(int cx, int cy, int r) {
  if (r < 1) r = 1;
  int r2 = r * r;
  for (int dy = -r; dy <= r; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= PH) continue;  // Paint_SetPixel does NOT clip
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r2) continue;
      int x = cx + dx;
      if (x < 0 || x >= PW) continue;
      Paint_SetPixel((uint16_t)x, (uint16_t)y, BLACK);
    }
  }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t colour) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > PW - 1) x1 = PW - 1;
  if (y1 > PH - 1) y1 = PH - 1;
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) Paint_SetPixel((uint16_t)x, (uint16_t)y, colour);
}

// Text on a white pad. The maze runs under the HUD, and black glyphs on black wall
// are unreadable — so knock a white hole the size of the text plus PAD out first.
// Glyph advance is size/2 in this font, so the extent is known without measuring.
#define HUD_PAD 4
static void hud_text(int x, int y, const char *str, int size) {
  int w = (int)strlen(str) * (size / 2);
  fill_rect(x - HUD_PAD, y - HUD_PAD, x + w + HUD_PAD, y + size + HUD_PAD, WHITE);
  EPD_ShowString((uint16_t)x, (uint16_t)y, str, (uint16_t)size, BLACK);
}

static void draw_hud(void) {
  if (g_label[0]) hud_text(12, 10, g_label, 24);
  if (g_note[0]) hud_text(12, 42, g_note, 24);
  if (g_note2[0]) hud_text(12, 74, g_note2, 24);

  // Top right, on the same line as the mode banner. Paint_SetPixel does NOT clip,
  // so the start x is clamped rather than trusted.
  if (g_fps[0]) {
    int w = (int)strlen(g_fps) * (HUD_FPS_SIZE / 2);
    int x = (w + 12 >= PW) ? 0 : (PW - w - 12);
    hud_text(x, 10, g_fps, HUD_FPS_SIZE);
  }
}

// Static text the balls travel over. The real test is not the circles — it is
// whether this text comes back intact after a solid disc has covered it.
// Mixed sizes on purpose: 24 for weight, 16 for body, 12 for the thin strokes
// that a weak waveform smears first.
__attribute__((unused)) static void draw_body_text(void) {
  EPD_ShowString(16, 180, "THE QUICK BROWN FOX", 16, BLACK);
  EPD_ShowString(16, 210, "jumps over the lazy dog", 12, BLACK);
  EPD_ShowString(16, 270, "GHOST TEST", 24, BLACK);
  EPD_ShowString(16, 330, "0123456789", 16, BLACK);
  EPD_ShowString(16, 390, "abcdefghijklmnopqr", 12, BLACK);
  EPD_ShowString(16, 450, "WAVEFORM QUALITY", 16, BLACK);
  EPD_ShowString(16, 510, "thin strokes . , ; : ' \"", 12, BLACK);
  EPD_ShowString(16, 570, "EDGE SHARPNESS", 16, BLACK);
  EPD_ShowString(16, 630, "residue check 1234", 12, BLACK);
  EPD_ShowString(16, 690, "BOTTOM LINE", 16, BLACK);
  EPD_ShowString(16, 730, "does text survive?", 12, BLACK);
}

// Seed image: everything EXCEPT the balls. The seed drives hard, so it must not
// plant a solid disc at a repeatable location — that is what burned in.
// Discs now only ever reach the glass through animation.
// Mode number, the time, and one moving disc on white. Nothing else — the disc's
// trail on a clean field is the whole judgement.
// Frames left during which the readout may still change. Set on any mode or
// regime change, then counted down to zero and FROZEN.
//
// It cannot refresh every frame: a changed digit is a changed pixel, changed
// pixels enter the transfer window, and the window then has to span the HUD as
// well as the moving content. Worse, it self-amplifies — a wider window costs
// more SPI, which changes the "ms total" digits, which widens the window again.
// Measured: the window stretched from 17 KB to 39 KB per frame with the right
// edge pinned to the HUD.
//
// It also cannot be built once at arm time: that happens straight after a GC
// clear, before any frame has run, so the SPI figure is still zero and "total"
// wrongly equals "wave". A few frames of settling, then frozen, satisfies both.
// Display only — nothing here can reach the glass.
//
// A regime change resyncs with a FULL-panel write first, so the frames right
// after it measure the full transfer. Freezing on those reported ~90 ms of SPI
// forever while the steady state was ~20 ms. Rather than simply waiting longer
// (which prolongs the HUD churn that inflates the transfer window), the counter
// is re-armed whenever the transferred byte count changes materially — that is
// exactly the moment the old reading became wrong.
#define LABEL_SETTLE_FRAMES 4
static uint8_t g_label_settle = LABEL_SETTLE_FRAMES;

// Re-arming on "the byte count moved" seemed clever and was a feedback loop:
// bytes legitimately vary between the disc-only and disc-plus-HUD windows, so it
// fired every frame, which changed the text, which put the HUD back in the
// transfer, which changed the bytes again. Nothing that is DOWNSTREAM of the
// readout may be used to decide whether to update the readout.
//
// A plain countdown instead: after a regime change, wait for the resync and the
// transfer to reach steady state, then take a few samples and freeze for good.
static uint16_t g_settle_delay = 0;

// ---- Ms Pac-Man level -------------------------------------------------------
// 11 x 33 cells of 24 px fills 264 x 792 of the 272 x 792 canvas, 4 px of margin
// each side. The grid is odd in both axes so the classic carve-on-odd-cells maze
// algorithm applies directly.
#define MZ_CELL 24
#define MZ_COLS 11
#define MZ_ROWS 33
#define MZ_OX ((PW - MZ_COLS * MZ_CELL) / 2)
#define MZ_OY 0
#define MZ_STEP 4        // px per frame; 24/4 = 6 frames per cell, so centres land exactly
#define MZ_GHOSTS 4

static uint8_t mz_wall[MZ_ROWS][MZ_COLS];
static uint8_t mz_dot[MZ_ROWS][MZ_COLS];
static uint16_t mz_dots_left = 0;
static uint32_t mz_score = 0;

typedef struct {
  int px, py;      // pixel centre
  int8_t dx, dy;   // unit direction
  uint8_t shade;   // Bayer threshold: stands in for ghost colour
} MzEnt;

// Static-maze template.
//
// mz_draw_board() rasterised all 363 cells through Paint_SetPixel EVERY frame —
// ~190k pixel writes, measured at 38 ms, which was more than the entire per-frame
// cost we spent the session removing. The maze is static and the pellets change one
// cell at a time, so it is rasterised ONCE per level into this buffer and each frame
// starts as a memcpy of it (~27 KB, under 1 ms). Eating a pellet punches that one
// pellet out of the template rather than triggering a rebuild.
static uint8_t mz_bg[(uint32_t)EPD_H * FB_WBYTES];
static void mz_bg_rebuild(void);
static void mz_bg_clear_dot(int r, int c);

static MzEnt mz_pac;
static MzEnt mz_gh[MZ_GHOSTS];

static inline int mz_cx(int c) { return MZ_OX + c * MZ_CELL + MZ_CELL / 2; }
static inline int mz_cy(int r) { return MZ_OY + r * MZ_CELL + MZ_CELL / 2; }
static inline bool mz_open(int r, int c) {
  return r >= 0 && r < MZ_ROWS && c >= 0 && c < MZ_COLS && !mz_wall[r][c];
}
// True when an entity sits exactly on a cell centre, which is the only moment a
// direction change is legal — otherwise sprites drift off the lattice and clip walls.
static inline bool mz_centred(const MzEnt *e) {
  return ((e->px - MZ_OX - MZ_CELL / 2) % MZ_CELL) == 0 &&
         ((e->py - MZ_OY - MZ_CELL / 2) % MZ_CELL) == 0;
}
static inline int mz_col(const MzEnt *e) { return (e->px - MZ_OX) / MZ_CELL; }
static inline int mz_row(const MzEnt *e) { return (e->py - MZ_OY) / MZ_CELL; }

// Carve a perfect maze on the odd cells with an explicit stack (no recursion — this
// runs on the Arduino loop task), then MIRROR the left half onto the right so it
// reads as a Pac-Man board rather than a generic labyrinth, then punch a few extra
// holes so there are loops to chase around. A perfect maze has exactly one route
// between any two points, which makes for terrible pursuit.
static void mz_build(void) {
  memset(mz_wall, 1, sizeof(mz_wall));

  const int half = MZ_COLS / 2 + 1;  // carve cols 1..5, mirror 1..4 onto 9..6
  int8_t st_r[MZ_ROWS * MZ_COLS], st_c[MZ_ROWS * MZ_COLS];
  int sp = 0;
  st_r[0] = 1;
  st_c[0] = 1;
  sp = 1;
  mz_wall[1][1] = 0;

  while (sp) {
    int r = st_r[sp - 1], c = st_c[sp - 1];
    int dirs[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; i--) {  // shuffle
      int j = (int)(esp_random() % (uint32_t)(i + 1));
      int t = dirs[i];
      dirs[i] = dirs[j];
      dirs[j] = t;
    }
    bool moved = false;
    for (int i = 0; i < 4 && !moved; i++) {
      int nr = r, nc = c;
      if (dirs[i] == 0) nr -= 2;
      else if (dirs[i] == 1) nr += 2;
      else if (dirs[i] == 2) nc -= 2;
      else nc += 2;
      if (nr < 1 || nr >= MZ_ROWS - 1 || nc < 1 || nc >= half) continue;
      if (!mz_wall[nr][nc]) continue;
      mz_wall[(r + nr) / 2][(c + nc) / 2] = 0;
      mz_wall[nr][nc] = 0;
      st_r[sp] = (int8_t)nr;
      st_c[sp] = (int8_t)nc;
      sp++;
      moved = true;
    }
    if (!moved) sp--;
  }

  // Extra openings on the carved half, for loops.
  for (int k = 0; k < MZ_ROWS; k++) {
    int r = 1 + (int)(esp_random() % (uint32_t)(MZ_ROWS - 2));
    int c = 1 + (int)(esp_random() % (uint32_t)(half - 1));
    if (r % 2 == 1 || c % 2 == 1) mz_wall[r][c] = 0;
  }

  for (int r = 0; r < MZ_ROWS; r++)
    for (int c = 0; c < MZ_COLS / 2; c++) mz_wall[r][MZ_COLS - 1 - c] = mz_wall[r][c];

  // Outer ring stays solid.
  for (int c = 0; c < MZ_COLS; c++) { mz_wall[0][c] = 1; mz_wall[MZ_ROWS - 1][c] = 1; }
  for (int r = 0; r < MZ_ROWS; r++) { mz_wall[r][0] = 1; mz_wall[r][MZ_COLS - 1] = 1; }

  mz_dots_left = 0;
  for (int r = 0; r < MZ_ROWS; r++)
    for (int c = 0; c < MZ_COLS; c++) {
      mz_dot[r][c] = mz_wall[r][c] ? 0 : 1;
      if (mz_dot[r][c]) mz_dots_left++;
    }
}

// First step of a shortest path from (r0,c0) to the nearest cell where want() holds.
// Plain BFS over 363 cells — trivial next to 5 ms of SPI, and it means Pac-Man
// actually hunts pellets instead of wandering into corners.
static bool mz_bfs_step(int r0, int c0, bool dots, int8_t *odx, int8_t *ody) {
  static int16_t prev[MZ_ROWS * MZ_COLS];
  static int16_t q[MZ_ROWS * MZ_COLS];
  for (int i = 0; i < MZ_ROWS * MZ_COLS; i++) prev[i] = -1;
  int head = 0, tail = 0;
  int start = r0 * MZ_COLS + c0;
  q[tail++] = (int16_t)start;
  prev[start] = (int16_t)start;
  int found = -1;

  while (head < tail && found < 0) {
    int cur = q[head++];
    int r = cur / MZ_COLS, c = cur % MZ_COLS;
    static const int8_t dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
      int nr = r + dr[i], nc = c + dc[i];
      if (!mz_open(nr, nc)) continue;
      int nid = nr * MZ_COLS + nc;
      if (prev[nid] >= 0) continue;
      prev[nid] = (int16_t)cur;
      if (dots && mz_dot[nr][nc]) { found = nid; break; }
      q[tail++] = (int16_t)nid;
    }
  }
  if (found < 0) return false;

  int cur = found;
  while (prev[cur] != start && prev[cur] != cur) cur = prev[cur];
  *odx = (int8_t)((cur % MZ_COLS) - c0);
  *ody = (int8_t)((cur / MZ_COLS) - r0);
  return true;
}

static void mz_reset(void) {
  mz_build();
  mz_bg_rebuild();
  mz_score = 0;

  // Pac-Man in the lowest open cell, ghosts spread down the board.
  int pr = MZ_ROWS - 2, pc = MZ_COLS / 2;
  while (pr > 1 && !mz_open(pr, pc)) pr--;
  mz_pac.px = mz_cx(pc);
  mz_pac.py = mz_cy(pr);
  mz_pac.dx = 0;
  mz_pac.dy = -1;

  // Four dither levels stand in for the four ghost colours: 16 = solid, 4 = light.
  static const uint8_t kShades[MZ_GHOSTS] = {16, 11, 7, 4};
  for (int i = 0; i < MZ_GHOSTS; i++) {
    int gr = 2 + (i * (MZ_ROWS - 6)) / MZ_GHOSTS, gc = MZ_COLS / 2;
    while (gr < MZ_ROWS - 2 && !mz_open(gr, gc)) gr++;
    mz_gh[i].px = mz_cx(gc);
    mz_gh[i].py = mz_cy(gr);
    mz_gh[i].dx = 0;
    mz_gh[i].dy = 1;
    mz_gh[i].shade = kShades[i];
  }
}

static void mz_step_ent(MzEnt *e) {
  e->px += e->dx * MZ_STEP;
  e->py += e->dy * MZ_STEP;
}

static void mz_advance(void) {
  // --- Pac-Man
  if (mz_centred(&mz_pac)) {
    int r = mz_row(&mz_pac), c = mz_col(&mz_pac);
    if (mz_dot[r][c]) {
      mz_dot[r][c] = 0;
      mz_bg_clear_dot(r, c);  // one 7x7 patch, not a full re-rasterise
      if (mz_dots_left) mz_dots_left--;
      mz_score += 10;
    }
    if (!mz_dots_left) mz_reset();  // board cleared: new maze, keeps the demo going
    else {
      int8_t nx = 0, ny = 0;
      if (mz_bfs_step(r, c, true, &nx, &ny)) {
        mz_pac.dx = nx;
        mz_pac.dy = ny;
      } else if (!mz_open(r + mz_pac.dy, c + mz_pac.dx)) {
        mz_pac.dx = 0;
        mz_pac.dy = 0;
      }
    }
  }
  if (mz_pac.dx || mz_pac.dy) {
    int r = mz_row(&mz_pac), c = mz_col(&mz_pac);
    if (mz_centred(&mz_pac) && !mz_open(r + mz_pac.dy, c + mz_pac.dx)) {
      mz_pac.dx = 0;
      mz_pac.dy = 0;
    } else {
      mz_step_ent(&mz_pac);
    }
  }

  // --- Ghosts: greedy toward Pac-Man, no reversing unless cornered.
  for (int i = 0; i < MZ_GHOSTS; i++) {
    MzEnt *g = &mz_gh[i];
    if (mz_centred(g)) {
      int r = mz_row(g), c = mz_col(g);
      static const int8_t dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
      int best = -1, bestd = 0x7FFFFFFF;
      bool chase = (esp_random() % 100u) < 80u;  // 20% random, or they lock together
      for (int k = 0; k < 4; k++) {
        int nr = r + dr[k], nc = c + dc[k];
        if (!mz_open(nr, nc)) continue;
        if (dc[k] == -g->dx && dr[k] == -g->dy) continue;  // no U-turn
        int ddx = mz_cx(nc) - mz_pac.px, ddy = mz_cy(nr) - mz_pac.py;
        int d = chase ? (ddx * ddx + ddy * ddy) : (int)(esp_random() % 100000u);
        if (d < bestd) { bestd = d; best = k; }
      }
      if (best < 0) { g->dx = (int8_t)-g->dx; g->dy = (int8_t)-g->dy; }
      else { g->dx = (int8_t)dc[best]; g->dy = (int8_t)dr[best]; }
    }
    int r = mz_row(g), c = mz_col(g);
    if (mz_centred(g) && !mz_open(r + g->dy, c + g->dx)) continue;
    mz_step_ent(g);
  }
}

// Walls as solid blocks inset by 1 px, so adjacent wall cells read as one mass but
// the lattice still shows. Pellets are small discs on the open cells.
static void mz_draw_board(void) {
  for (int r = 0; r < MZ_ROWS; r++) {
    for (int c = 0; c < MZ_COLS; c++) {
      int x0 = MZ_OX + c * MZ_CELL, y0 = MZ_OY + r * MZ_CELL;
      if (mz_wall[r][c]) {
        fill_rect(x0, y0, x0 + MZ_CELL - 2, y0 + MZ_CELL - 2, BLACK);
      } else if (mz_dot[r][c]) {
        fill_disc(x0 + MZ_CELL / 2, y0 + MZ_CELL / 2, 2);
      }
    }
  }
}

static void mz_bg_rebuild(void) {
  paint_bind(mz_bg);
  Paint_Clear(WHITE);
  mz_draw_board();
  paint_bind(ImageBW);
}

static void mz_bg_clear_dot(int r, int c) {
  paint_bind(mz_bg);
  int x = MZ_OX + c * MZ_CELL + MZ_CELL / 2;
  int y = MZ_OY + r * MZ_CELL + MZ_CELL / 2;
  fill_rect(x - 3, y - 3, x + 3, y + 3, WHITE);  // pellet is r=2, so 7x7 covers it
  paint_bind(ImageBW);
}

// Ghost: domed top, straight flanks, zig-zag skirt, two white eyes with pupils.
// `shade` is a 4x4 Bayer threshold, which is how four "colours" survive a 1-bit
// panel — solid through to light stipple, distinguishable at a glance on glass.
static void draw_ghost(int cx, int cy, int R, uint8_t shade) {
  const int foot = R / 3;
  for (int dy = -R; dy <= R; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= PH) continue;
    int hw;
    if (dy < 0) {  // dome
      int q = R * R - dy * dy;
      hw = 0;
      while ((hw + 1) * (hw + 1) <= q) hw++;
    } else {
      hw = R;  // straight flanks
    }
    for (int dx = -hw; dx <= hw; dx++) {
      // Skirt: cut alternate notches out of the bottom `foot` rows.
      if (dy > R - foot) {
        int seg = (dx + R) / foot;
        if (seg & 1) {
          if (dy > R - foot + (foot - 1) - ((dx + R) % foot >= foot / 2 ? 0 : 0)) continue;
        }
      }
      int x = cx + dx;
      if (x < 0 || x >= PW) continue;
      if (kBayer4[y & 3][x & 3] < shade) Paint_SetPixel((uint16_t)x, (uint16_t)y, BLACK);
    }
  }
  // Eyes, always solid white with a solid pupil so they read against any shade.
  int er = R / 3, ey = cy - R / 3;
  for (int s = -1; s <= 1; s += 2) {
    int ex = cx + s * (R / 2);
    for (int dy = -er; dy <= er; dy++)
      for (int dx = -er; dx <= er; dx++) {
        if (dx * dx + dy * dy > er * er) continue;
        int x = ex + dx, y = ey + dy;
        if (x < 0 || x >= PW || y < 0 || y >= PH) continue;
        Paint_SetPixel((uint16_t)x, (uint16_t)y, WHITE);
      }
    fill_disc(ex + s * (er / 2), ey, er / 2 + 1);
  }
}

static void render_frame_static(void) {
  // Blit the template instead of clearing to white and re-rasterising the maze.
  paint_bind(ImageBW);
  memcpy(ImageBW, mz_bg, sizeof(ImageBW));
  if (g_settle_delay) {
    g_settle_delay--;
    if (g_settle_delay == 4) g_label_settle = 3;  // one late, accurate refresh
  }
  if (g_label_settle) {
    set_label();
    g_label_settle--;
  }
  draw_hud();  // last, so its white pad knocks the maze back out from under the text
}

// Starfish-flare UAP: five fat radiating arms, one arm aimed along the direction
// of travel — so a point LEADS and the notch between the two rear arms TRAILS.
//
// Polar boundary, not a sprite. It therefore scales with `radius N` and can be
// aimed at an arbitrary angle, where a bitmap has to snap to multiples of 90
// degrees or be resampled into mush.
//
// Ms Pac-Man, mouth pointing along (vx, vy) — nose-first, so the open wedge LEADS
// and the round back TRAILS.
//
// Procedural, not a bitmap: the wedge is an excluded angular sector with its vertex
// at the centre, which is exactly what Pac-Man's mouth is, and it can therefore be
// aimed at any angle instead of snapping to multiples of 90 degrees the way a
// sprite must. The bow is what makes her Ms — two small discs set off the head at
// 90 degrees to the heading, so it rotates with her.
#define PAC_MOUTH_DEG 38.0f  // half-angle of the open wedge
#define TWO_PI_F 6.2831853f
#define PI_F 3.14159265f


static void draw_pacman(int cx, int cy, int R, int vx, int vy) {
  const float t0 = atan2f((float)vy, (float)vx);
  const float mouth = PAC_MOUTH_DEG * PI_F / 180.0f;
  const int r2 = R * R;

  for (int dy = -R; dy <= R; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= PH) continue;
    for (int dx = -R; dx <= R; dx++) {
      if (dx * dx + dy * dy > r2) continue;
      // Angle away from the heading, wrapped to [-pi, pi]. Inside the wedge is
      // MOUTH, so it is left white.
      float d = atan2f((float)dy, (float)dx) - t0;
      if (d > PI_F) d -= TWO_PI_F;
      else if (d < -PI_F) d += TWO_PI_F;
      if (fabsf(d) < mouth) continue;
      int x = cx + dx;
      if (x < 0 || x >= PW) continue;
      Paint_SetPixel((uint16_t)x, (uint16_t)y, BLACK);
    }
  }

  // Bow: two lobes at 90 degrees off the heading, sitting on the crown. Scaled
  // off R so it survives `box N`.
  int br = R / 3;
  if (br < 2) br = 2;
  float bt = t0 - PI_F / 2.0f;  // "up" relative to travel
  float bd = (float)R * 0.92f;
  for (int side = -1; side <= 1; side += 2) {
    float a = bt + (float)side * 0.42f;
    int bxp = cx + (int)(cosf(a) * bd);
    int byp = cy + (int)(sinf(a) * bd);
    fill_disc(bxp, byp, br);
  }
}

static void render_frame(void) {
  render_frame_static();  // maze, pellets, then the HUD with its white pad
  // Ghosts first, Pac-Man last, so she passes over them rather than under.
  for (int i = 0; i < MZ_GHOSTS; i++)
    draw_ghost(mz_gh[i].px, mz_gh[i].py, MZ_CELL / 2 - 2, mz_gh[i].shade);
  draw_pacman(mz_pac.px, mz_pac.py, MZ_CELL / 2 - 2, mz_pac.dx ? mz_pac.dx : 0,
              mz_pac.dy ? mz_pac.dy : (mz_pac.dx ? 0 : -1));
}

// ---- panel ------------------------------------------------------------------
// Left exactly as it was. Two attempts to make a reflash behave like a cold boot
// — poking 0x22 before the hardware reset, then toggling PIN_PWR low for 250 ms
// — both made the glitching worse rather than better. Whatever the stale state
// is, it is not cleared this way. Power-cycle the board by hand after a reflash.
static void panel_on(void) {
  pinMode(PIN_PWR, OUTPUT);
  digitalWrite(PIN_PWR, HIGH);
  delay(20);
}

static void epd_ready(void) {
  EPD_GPIOInit();
  EPD_FastMode1Init();
}

// Deep de-ghost. Drives the whole panel black<->white N times using false-diff
// (336 ms per half, so ~672 ms per cycle) then settles white. This is the only
// thing that lifts accumulated charge from repeated same-pixel drives — a single
// GC does not. Boot and explicit `flush` only.
static void epd_deghost(uint8_t cycles) {
  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  for (uint8_t c = 0; c < cycles; c++) {
    memset(ImageBW, 0x00, sizeof(ImageBW));  // all black
    EPD_Display(ImageBW);
    EPD_Display_OldPlane_Ex(ImageBW, true);
    EPD_PartUpdate();
    memset(ImageBW, 0xFF, sizeof(ImageBW));  // all white
    EPD_Display(ImageBW);
    EPD_Display_OldPlane_Ex(ImageBW, true);
    EPD_PartUpdate();
    Serial.printf("deghost cycle %u/%u\n", (unsigned)(c + 1), (unsigned)cycles);
  }
  memset(ImagePrev, 0xFF, sizeof(ImagePrev));
}

// One GC flash. Boot and explicit `clear` only — never during animation.
static void epd_full_clear(void) {
  epd_ready();
  EPD_Display_Clear();
  EPD_Update();
  EPD_LoadWhiteBothPlanes();
}

// Force glass == NEW == OLD, so the next differential update starts from a
// coherent state. Every mode change does this.
//
// The obvious version — write the image to BOTH planes and fire — is broken:
// with NEW == OLD the differential waveform sees no changed pixels and drives
// nothing, so the glass keeps whatever it had while both planes claim the new
// image. Every later diff is then computed against an image that was never on
// the panel, and the pixels it lies about get driven once through a spurious
// transition and never revisited — a faint permanent ghost at the seed pose.
//
// Correct order: OLD = ~image first so EVERY pixel differs and actually gets
// driven, then rewrite OLD = image so the planes agree with the glass.
static void epd_seed(void) {
  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  EPD_Display(ImageBW);                     // NEW = image
  EPD_Display_OldPlane_Ex(ImageBW, true);   // OLD = ~image -> everything differs
  EPD_PartUpdate();                         // drive every pixel to the image
  EPD_Display_OldPlane_Ex(ImageBW, false);  // OLD = image -> planes match glass
  memcpy(ImagePrev, ImageBW, sizeof(ImagePrev));
  // This just wrote OLD = NEW across the WHOLE panel, so nothing is hot any
  // more. The history has to agree, or the next windowed frame will re-assert
  // pixels the RAM has already settled.
  trail_reset();
}

// ---- waveform arming --------------------------------------------------------
//
// Order matters and is not negotiable: epd_seed() fires 0xDC, which carries a
// Load-LUT bit and therefore restores the factory waveform. So a custom table is
// always pushed AFTER the seed, never before it.
// ---- boot circle field ------------------------------------------------------

#define FIELD_MIN 50
#define FIELD_MAX 80
#define FIELD_R_MIN 2
#define FIELD_R_MAX 20

// Scatter dithered circles across the whole panel, tiny to medium, each at its own
// dither level so they cover a range of apparent greys.
static void draw_circle_field(void) {
  int n = FIELD_MIN + (int)(esp_random() % (uint32_t)(FIELD_MAX - FIELD_MIN + 1));
  for (int i = 0; i < n; i++) {
    int r = FIELD_R_MIN + (int)(esp_random() % (uint32_t)(FIELD_R_MAX - FIELD_R_MIN + 1));
    int cx = r + (int)(esp_random() % (uint32_t)(PW - 2 * r));
    int cy = r + (int)(esp_random() % (uint32_t)(PH - 2 * r));
    int level = 3 + (int)(esp_random() % 14);  // 3..16 of 16 -> light to solid
    int rr = r * r;
    for (int dy = -r; dy <= r; dy++) {
      int y = cy + dy;
      if (y < 0 || y >= PH) continue;  // Paint_SetPixel does NOT clip
      for (int dx = -r; dx <= r; dx++) {
        if (dx * dx + dy * dy > rr) continue;
        int x = cx + dx;
        if (x < 0 || x >= PW) continue;
        if (kBayer4[y & 3][x & 3] < level) Paint_SetPixel((uint16_t)x, (uint16_t)y, BLACK);
      }
    }
  }
  Serial.printf("FIELD %d dithered circles seeded\n", n);
}

// Put the circle field on the glass, then deliberately LIE to the driver about it.
//
// After the false-diff paint below the glass shows circles. Setting both RAM planes
// to white then tells the controller the panel is blank, WITHOUT driving anything —
// EPD_LoadWhiteBothPlanes writes RAM only.
//
// That mismatch is the entire effect. From here a differential update drives only
// the pixels the bench frame actually changes: Ms Pac-Man and the HUD. A pixel she
// has just moved off is NEW=white / OLD=black, so it gets driven white and whatever
// circle was underneath is gone. A pixel she never reaches has old == new, gets no
// drive at all, and keeps its circle indefinitely. Nothing in the render path knows
// the circles exist — the erasing is a property of the differential waveform.
static void apply_mode_wave(void);

static void boot_circle_field(void) {
  paint_begin();
  draw_circle_field();

  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  EPD_Display(ImageBW);                    // NEW = field
  EPD_Display_OldPlane_Ex(ImageBW, true);  // OLD = ~field -> every pixel transitions
  (void)EPD_PartUpdate_Timed(nullptr);     // vendor waveform: maximum impulse

  EPD_LoadWhiteBothPlanes();                    // RAM only — no drive, glass keeps circles
  memset(ImagePrev, WHITE, sizeof(ImagePrev));  // and the diff baseline agrees
  trail_reset();                                // nothing is hot; history must say so
  apply_mode_wave();                            // the vendor partial reloaded the OTP LUT
}

static void apply_mode_wave(void) {
  const ModeInfo &m = mode_info();
  g_wave_loaded = false;
  // Every regime change lands here — mode switches, `drive N`, `fr N`, any
  // voltage command. Averaging frames from either side of one reports a rate that
  // was never on the glass, so drop the accumulator.
  fps_reset();
  EPD_GPIOInit();

  switch (m.wave) {
    case WV_OTP3:
      EPD_ReloadOtpLutSeq(0x91);
      set_label();
      return;
    case WV_OTPBW:
      EPD_ReloadOtpLutSeq(0x99);
      set_label();
      return;
    case WV_CUSTOM:
      wave_build(g_ws, &g_wp);
      break;
    case WV_GD:
      wave_build_gd(g_ws, g_gd_fr);
      break;
    case WV_PROBE:
      wave_build_probe(g_ws, g_probe_block, g_probe_level, g_probe_tp, g_wp.fr, &g_wp);
      break;
  }

  wave_load(g_ws, g_dual, g_wp.volt_mask);
  g_wave_loaded = true;
  set_label();
  g_label_settle = LABEL_SETTLE_FRAMES;
  g_settle_delay = 16;  // ... then one accurate refresh once the transfer settles

  uint16_t fr_count = wave_frames(g_ws);
  uint32_t per = wave_fr_period_us(g_ws[224]);
  Serial.printf("WAVE %s loaded: frames=%u fr=0x%02X(%s) dual=%u volt=%u -> predicted busy ~%lu ms\n",
                m.name, (unsigned)fr_count, (unsigned)g_ws[224], wave_fr_name(g_ws[224]),
                (unsigned)g_dual, (unsigned)g_wp.volt_mask,
                (unsigned long)((uint64_t)fr_count * per / 1000ULL));
}

// Bring the analog rails up (or down) outside the timed path. Every byte that
// clears b6 assumes this has already happened.
static void analog_power(bool on) { EPD_AnalogPower(on); }

// Only a custom LUT can be driven with a rails-up byte. The factory OTP
// waveform never releases BUSY under 0x0C — it sat in 8-second timeouts and
// eventually took the board down with it — so OTP modes are pinned to their own
// byte and the override cannot reach them.
static bool mode_is_custom(void) {
  uint8_t w = mode_info().wave;
  return w == WV_CUSTOM || w == WV_GD || w == WV_PROBE;
}

static uint8_t eff_seq(void) {
  if (!mode_is_custom()) return mode_info().seq22;
  return g_seq ? g_seq : mode_info().seq22;
}

static uint32_t fire_update(uint8_t seq22, uint32_t *busy_us) {
  // A byte with b6 clear does not power the analog block itself, so it only
  // works if the rails are already up. Do that once, outside the measurement.
  (void)EPD_EnsureRailsFor(seq22);
  // b1/b0 tear the rails back down, so the next update pays the ramp again.
  // The driver notes that for itself inside the activate helpers below.

  // Plain 0x22 + 0x20 on every path — no 0x21 display-control write, so the only
  // difference between modes is the sequence byte and the LUT behind it.
  if (seq22 == 0xDC) return EPD_PartUpdate_Timed(busy_us);
  return EPD_UpdateSeqPlain_Timed(seq22, busy_us);
}

// One write+fire cycle. Returns busy us; accumulates SPI us into *spi_acc.
// Forget every hot pixel. A trail-chased pixel is deliberately left with
// OLD != NEW so it re-drives; it only goes quiet when a later frame rewrites
// OLD = NEW for it. Any pixel that is hot when the transfer regime changes and
// then falls outside the window never gets that rewrite, so it re-drives on
// every frame forever — visible as patches flashing away in otherwise static
// areas until the moving content happens to sweep over them.
//
// Callers must pair this with a FULL-panel write of OLD = NEW (epd_seed does
// exactly that) so the RAM matches the cleared history.
static void trail_reset(void) {
  if (!g_trail_mask) return;
  for (uint8_t k = 0; k < TRAIL_MAX; k++)
    if (g_hist[k]) memset(g_hist[k], 0, PLANE_BYTES);
  memset(g_trail_mask, 0, PLANE_BYTES);
  if (g_span_mask) memset(g_span_mask, 0, PLANE_BYTES);
}

// Roll this frame's change mask into the ring and rebuild the OR of the last
// g_trail frames. A pixel that changed on frame f therefore stays hot through
// frame f+g_trail-1 and is driven that many times.
// Measured at 16 ms per frame before this was cut down — a third of a mode-1
// frame, and it was being charged to spi_ms because the call sat inside that
// timing span. The wire is only 12 ms for both planes (54400 bytes at 40 MHz is
// 10.9 ms theoretical, so ~91% of rate); the transport was never the problem.
//
// The old version walked ALL 8 ring slots every frame no matter what g_trail was
// — the `k < n` test gated only the write, not the pass — and rebuilt g_span_mask
// alongside, which is used by the windowed transfer path and by nothing else.
// Windowing is off by default, so that half was pure waste.
static void trail_update(void) {
  if (!g_trail_mask) return;
  uint8_t *cur = g_hist[g_hist_pos];
  for (uint32_t i = 0; i < PLANE_BYTES; i++) cur[i] = ImageBW[i] ^ ImagePrev[i];
  g_hist_pos = (uint8_t)((g_hist_pos + 1) % TRAIL_MAX);

  uint8_t n = g_trail > TRAIL_MAX ? TRAIL_MAX : g_trail;
  if (n < 1) n = 1;

  // Seed from the newest slot with a memcpy instead of memset-then-OR: same
  // result, one pass fewer.
  memcpy(g_trail_mask, g_hist[(g_hist_pos + TRAIL_MAX - 1) % TRAIL_MAX], PLANE_BYTES);
  for (uint8_t k = 1; k < n; k++) {
    const uint8_t *h = g_hist[(g_hist_pos + TRAIL_MAX - 1 - k) % TRAIL_MAX];
    for (uint32_t i = 0; i < PLANE_BYTES; i++) g_trail_mask[i] |= h[i];
  }

  // Only the windowed path reads this, and it needs the WHOLE ring, not just the
  // chased frames. Built from g_trail_mask so the first n slots are not re-walked.
  if (g_win_on && g_span_mask) {
    memcpy(g_span_mask, g_trail_mask, PLANE_BYTES);
    for (uint8_t k = n; k < TRAIL_MAX; k++) {
      const uint8_t *h = g_hist[(g_hist_pos + TRAIL_MAX - 1 - k) % TRAIL_MAX];
      for (uint32_t i = 0; i < PLANE_BYTES; i++) g_span_mask[i] |= h[i];
    }
  }
}

// Fixed-size window centred on everything the history ring has touched. Returns
// false if nothing has changed at all, in which case there is nothing to send.
static bool window_for_span(uint8_t *c0, uint8_t *c1, uint16_t *r0, uint16_t *r1) {
  uint8_t bc0 = 0xFF, bc1 = 0;
  uint16_t br0 = 0xFFFF, br1 = 0;
  for (uint16_t r = 0; r < Gate_BITS; r++) {
    for (uint8_t c = 0; c < FB_WBYTES; c++) {
      if (!g_span_mask[FB_IDX(r, c)]) continue;
      if (c < bc0) bc0 = c;
      if (c > bc1) bc1 = c;
      if (r < br0) br0 = r;
      if (r > br1) br1 = r;
    }
  }
  if (bc0 > bc1) return false;

  // SAFE = a ROW band at FULL WIDTH.
  //
  // Every column-limited window tried so far flashed in whatever region it did
  // not write, and the only consistently clean area was the one strip forced
  // into every transfer. Restricting columns is also what drags in the slave's
  // mirrored X mapping and the master/slave seam split — the parts of the
  // windowed path with no counterpart in the full-screen path that works.
  //
  // So: keep c0..c1 spanning the whole panel, exactly as the full path does, and
  // vary only the Y range. The column handling is then byte-identical to the
  // known-good transfer, and the only difference left is the row window.
  // Saves roughly half the bytes, and if it still flashes then the fault is
  // provably in the Y window rather than anywhere else.
  if (g_win_mode == 1) {
    uint16_t wr = g_win_rows;
    if (wr > Gate_BITS) wr = Gate_BITS;
    if ((uint16_t)(br1 - br0 + 1) > wr) wr = (uint16_t)(br1 - br0 + 1);
    int rc = ((int)br0 + (int)br1 + 1) / 2 - wr / 2;
    if (rc < 0) rc = 0;
    if (rc + wr > Gate_BITS) rc = Gate_BITS - wr;
    *c0 = 0;
    *c1 = FB_WBYTES - 1;
    *r0 = (uint16_t)rc;
    *r1 = (uint16_t)(rc + wr - 1);
    return true;
  }

  // Grow to the fixed size around the centre, then slide back inside the panel
  // rather than shrinking — the byte count must not depend on where the content
  // happens to be.
  uint8_t wc = g_win_cols;
  uint16_t wr = g_win_rows;
  if (wc > FB_WBYTES) wc = FB_WBYTES;
  if (wr > Gate_BITS) wr = Gate_BITS;
  if ((uint8_t)(bc1 - bc0 + 1) > wc) wc = (uint8_t)(bc1 - bc0 + 1);
  if ((uint16_t)(br1 - br0 + 1) > wr) wr = (uint16_t)(br1 - br0 + 1);

  int cc = ((int)bc0 + (int)bc1 + 1) / 2 - wc / 2;
  int rc = ((int)br0 + (int)br1 + 1) / 2 - wr / 2;
  if (cc < 0) cc = 0;
  if (rc < 0) rc = 0;
  if (cc + wc > FB_WBYTES) cc = FB_WBYTES - wc;
  if (rc + wr > Gate_BITS) rc = Gate_BITS - wr;

  *c0 = (uint8_t)cc;
  *c1 = (uint8_t)(cc + wc - 1);
  *r0 = (uint16_t)rc;
  *r1 = (uint16_t)(rc + wr - 1);
  return true;
}

static uint8_t eff_old(void) {
  return g_old_override >= 0 ? (uint8_t)g_old_override : mode_info().old_plane;
}

static uint32_t one_pass(uint8_t old_plane, uint8_t seq22, uint32_t *spi_acc) {
  EPD_GPIOInit();
  EPD_RestoreFullWindows();

  // Build the mask BEFORE the SPI span and time it on its own. It used to be
  // called inside the span, which reported PSRAM mask work as transport time and
  // sent us auditing the DMA path over something that was never SPI at all.
  bool use_trail = (old_plane == OLD_TRAIL) && g_trail && g_trail_mask;
  uint32_t t0 = micros();
  if (use_trail) trail_update();
  g_last_trail_us = micros() - t0;

  uint32_t spi0 = micros();
  EPD_Display(ImageBW);  // NEW plane
  if (old_plane == OLD_PREV)
    EPD_Display_OldPlane_Ex(ImagePrev, false);
  else if (old_plane == OLD_INV)
    EPD_Display_OldPlane_Ex(ImageBW, true);
  else if (old_plane == OLD_TRAIL) {
    if (use_trail)
      EPD_Display_OldPlane_Masked(ImageBW, g_trail_mask);
    else
      EPD_Display_OldPlane_Ex(ImagePrev, false);  // trail 0 == plain true-diff
  }
  *spi_acc += micros() - spi0;

  uint32_t busy_us = 0;
  fire_update(seq22, &busy_us);
  return busy_us;
}

// Windowed variant: identical waveform, only the transfer shrinks.
static uint32_t one_pass_windowed(uint8_t seq22, uint32_t *spi_acc) {
  EPD_GPIOInit();
  trail_update();

  uint8_t c0, c1;
  uint16_t r0, r1;
  uint32_t spi0 = micros();
  uint32_t bytes = 0;
  const uint8_t *mask = g_trail ? g_trail_mask : nullptr;

  // Fake crossing: drive every pixel, not merely rewrite it.
  if (g_flush_frames) {
    g_flush_frames--;
    EPD_RestoreFullWindows();
    EPD_Display(ImageBW);
    EPD_Display_OldPlane_Ex(ImageBW, true);  // OLD = ~NEW -> everything is "changed"
    g_win_bytes = 2UL * PLANE_BYTES;
    g_win_c0 = 0; g_win_c1 = FB_WBYTES - 1; g_win_r0 = 0; g_win_r1 = Gate_BITS - 1;
    *spi_acc += micros() - spi0;
    uint32_t busy_flush = 0;
    fire_update(seq22, &busy_flush);
    // Leave the planes agreeing again, or the next frame drives everything twice.
    EPD_Display_OldPlane_Ex(ImageBW, false);
    trail_reset();
    return busy_flush;
  }

  // Periodic full write — the ceiling on how long any region can stay unwritten.
  if (g_win_sync && ++g_win_sync_ctr >= g_win_sync) {
    g_win_sync_ctr = 0;
    EPD_RestoreFullWindows();
    EPD_Display(ImageBW);
    if (mask) EPD_Display_OldPlane_Masked(ImageBW, mask);
    else EPD_Display_OldPlane_Ex(ImagePrev, false);
    g_win_bytes = 2UL * PLANE_BYTES;
    g_win_c0 = 0; g_win_c1 = FB_WBYTES - 1; g_win_r0 = 0; g_win_r1 = Gate_BITS - 1;
    *spi_acc += micros() - spi0;
    uint32_t busy_full = 0;
    fire_update(seq22, &busy_full);
    return busy_full;
  }

  bool have = window_for_span(&c0, &c1, &r0, &r1);
  if (have) EPD_WriteWindowMasked(ImageBW, mask, c0, c1, r0, r1, true, &bytes);

  // Second, separate window for the HUD strip while its text is still settling.
  // Two small rectangles, not one bounding box around both — the disc is usually
  // far from the HUD, and a box spanning them would be most of the panel.
  // The HUD spans nearly every row but only the high columns, so a row band can
  // miss it entirely. Give it its own full-height strip while its text is still
  // in flight.
  if (g_hud_frames) {
    g_hud_frames--;
    bool covered = have && c0 <= HUD_C0 && c1 >= HUD_C1 && r0 == 0 && r1 == Gate_BITS - 1;
    if (!covered)
      EPD_WriteWindowMasked(ImageBW, mask, HUD_C0, HUD_C1, 0, Gate_BITS - 1, true, &bytes);
  }
  g_win_bytes = bytes;
  g_win_c0 = c0;
  g_win_c1 = c1;
  g_win_r0 = r0;
  g_win_r1 = r1;
  *spi_acc += micros() - spi0;

  uint32_t busy_us = 0;
  fire_update(seq22, &busy_us);
  return busy_us;
}

// Drive the WHOLE panel to white, several times over. Not a write — a drive.
//
// Writing white into RAM does nothing on its own: pixels that already read white
// are classified unchanged and never move, so any stripe left over from an
// earlier experiment sits there forever. Forcing OLD to the inverse makes every
// pixel "changed", so every pixel actually gets pushed to white, and repeating it
// gives the same cumulative impulse that clears the disc's trail. No GC flash —
// this is the fast differential waveform doing a full repaint.
static void drive_white_clear(uint8_t passes) {
  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  memset(ImageBW, 0xFF, sizeof(ImageBW));
  for (uint8_t p = 0; p < passes; p++) {
    EPD_Display(ImageBW);
    EPD_Display_OldPlane_Ex(ImageBW, true);  // OLD = black everywhere
    uint32_t busy = 0;
    fire_update(eff_seq(), &busy);
  }
  EPD_Display_OldPlane_Ex(ImageBW, false);  // planes agree again
  memcpy(ImagePrev, ImageBW, sizeof(ImagePrev));
  trail_reset();
}

// Armed when windowed mode is switched ON — that is where the bars come from,
// and switching back to the full transfer is what clears them. Doing this at
// startup was pointless: the app boots on the full path, which has no bars.
static bool g_pending_white_clear = false;

static void show_frame(void) {
  const ModeInfo &m = mode_info();
  uint32_t t0 = micros();

  uint32_t spi_us = 0, busy_us = 0;
  uint8_t passes = m.passes ? m.passes : 1;
  bool windowed = g_win_on && eff_old() == OLD_TRAIL && g_trail_mask;
  for (uint8_t p = 0; p < passes; p++)
    busy_us += windowed ? one_pass_windowed(eff_seq(), &spi_us)
                        : one_pass(eff_old(), eff_seq(), &spi_us);

  // Periodic full-panel cleanup without a GC flash. This is the shape a real app
  // would ship: cheap differential frames, with a false-diff scrub every N frames
  // to burn off accumulated ghosting.
  bool cleaned = false;
  if (m.auto_clean && g_clean_every && g_frame &&
      (g_frame % g_clean_every) == 0) {
    // Deliberately the vendor waveform: the scrub wants maximum impulse, not
    // speed. It reloads the OTP LUT, so a custom mode has to re-arm afterwards.
    for (uint8_t p = 0; p < g_clean_passes; p++)
      busy_us += one_pass(OLD_INV, 0xDC, &spi_us);
    cleaned = true;
    if (g_wave_loaded) apply_mode_wave();
  }

  g_last_spi_us = spi_us;
  g_last_busy_us = busy_us;
  g_last_tot_us = micros() - t0;
  g_last_to = EPD_BusyTimedOut();
  g_last_clean = cleaned;

  g_mode_frames++;
  g_mode_busy_sum += busy_us;

  if (g_last_to) {
    // Only recovery path: re-init and re-seed. Never a blind full reflash.
    Serial.println("WARN busy timeout -> reinit + seed");
    epd_ready();
    epd_seed();
    apply_mode_wave();
  }

  memcpy(ImagePrev, ImageBW, sizeof(ImagePrev));

  // Per-frame RESULT is a suspect in the ~18 ms of fixed per-frame overhead that
  // sits outside the timed region. The line is ~170 chars, and at 115200 baud over
  // the CH340 that is ~14.8 ms of wire time plus the formatting of a dozen args.
  //
  // `result N` prints one line every N frames (0 = silent). The line still carries
  // the wall-clock fps, so throttling it does not cost the measurement.
  if (!g_result_every || (g_frame % g_result_every) != 0) return;

  Serial.printf(
      "RESULT f=%lu mode=%s seq=0x%02X old=%u drive=%u fr=0x%02X pass=%u clean=%u spi_ms=%lu "
      "busy_ms=%lu trail_ms=%lu tot_ms=%lu to=%u avg_busy_ms=%lu fps=%s win=%u,%u..%u,%u bytes=%lu\n",
      (unsigned long)g_frame, m.name, (unsigned)eff_seq(), (unsigned)eff_old(),
      (unsigned)g_wp.drive_tp, (unsigned)g_wp.fr,
      (unsigned)passes, (unsigned)g_last_clean,
      (unsigned long)(g_last_spi_us / 1000UL),
      (unsigned long)(g_last_busy_us / 1000UL),
      (unsigned long)(g_last_trail_us / 1000UL),
      (unsigned long)(g_last_tot_us / 1000UL), (unsigned)g_last_to,
      (unsigned long)(g_mode_frames ? (g_mode_busy_sum / g_mode_frames / 1000ULL)
                                    : 0ULL),
      // The same string the panel is showing, so the glass can be checked against
      // the log instead of trusted. "-" for the first second of a new regime.
      g_fps_num[0] ? g_fps_num : "-",
      (unsigned)g_win_c0, (unsigned)g_win_r0, (unsigned)g_win_c1, (unsigned)g_win_r1,
      (unsigned long)g_win_bytes);
}

// One-shot full-width bar sweep, top to bottom. Bar height = the sprite box, and
// the step equals that height so the bar never overlaps itself — every pixel on the
// panel is driven through the B->W transition exactly ONCE.
//
// The point is normalisation, not motion. An incomplete white transition parks a
// pixel at a fixed grey and holds it there — measured on glass: the shade does not
// drift once abandoned. So a background that has been swept is uniformly that grey,
// and Pac-Man's wake afterwards is the SAME shade as everything around it. The
// artefact stops being visible because there is no pristine white left to contrast
// it against.
//
// Runs at whatever mode is armed, deliberately: the parked shade depends on drive
// frames, so the sweep has to be done at the mode that will actually be used or the
// background will not match the wake.
static void bar_sweep(void) {
  int h = (2 * g_radius * g_bar_pct) / 100;
  if (h < 4) h = 4;
  Serial.printf("SWEEP start bar_h=%d step=%d pct=%d drive=%u\n", h, h,
                g_bar_pct, (unsigned)g_wp.drive_tp);

  for (int y = 0; y < PH; y += h) {
    paint_begin();
    draw_hud();
    int y1 = y + h;
    if (y1 > PH) y1 = PH;
    for (int yy = y; yy < y1; yy++)
      for (int x = 0; x < PW; x++) Paint_SetPixel((uint16_t)x, (uint16_t)yy, BLACK);
    show_frame();
    g_frame++;
    fps_tick();
  }

  // One all-white frame so the LAST bar position is driven off too, otherwise it
  // stays black and the panel is uniform everywhere except the bottom band.
  paint_begin();
  draw_hud();
  show_frame();
  g_frame++;

  Serial.println("SWEEP done — background normalised");
}

static void advance_motion(void) { mz_advance(); }

// Start positions are randomised every boot. A fixed start pose meant the same
// pixels took a hard white->black drive on every single power-up, and that
// accumulates into real burn-in that one GC will not lift.
static void reset_balls(void) {
  for (int i = 0; i < NBALL; i++) {
    bsx[i] = kRatioX[i] < 0 ? -1 : 1;
    bsy[i] = kRatioY[i] < 0 ? -1 : 1;
    int lo = g_radius + 1;
    bx[i] = lo + (int)(esp_random() % (uint32_t)(PW - 2 * lo));
    by[i] = lo + (int)(esp_random() % (uint32_t)(PH - 2 * lo));
    if (esp_random() & 1) bsx[i] = -bsx[i];
    if (esp_random() & 1) bsy[i] = -bsy[i];
  }
}

static void clamp_balls(void) {
  int lo = g_radius + 1;
  for (int i = 0; i < NBALL; i++) {
    if (bx[i] < lo) bx[i] = lo;
    if (by[i] < lo) by[i] = lo;
    if (bx[i] > PW - g_radius - 2) bx[i] = PW - g_radius - 2;
    if (by[i] > PH - g_radius - 2) by[i] = PH - g_radius - 2;
  }
}

// ---- HALFSPLIT: is per-chip activation real? --------------------------------
//
// The |0x80 rule holds everywhere else on this cascade (0x11->0x91, 0x24->0xA4,
// 0x44->0xC4, 0x32->0xB2). If it also holds for update control and activate,
// then 0x22->0xA2 and 0x20->0xA0 fire the SLAVE chip alone.
//
// Two possible worlds:
//   A) 0xA2/0xA0 work  -> source drivers are independently activatable.
//   B) the slave's gate scan is cascaded off the master's timing controller,
//      in which case no command can start one chip without the other.
//
// Serial alone separates them: if BUSY never even asserts after 0xA0, the
// command did nothing. Glass then says WHICH half moved.
//
// Pattern is written straight into the native buffer via FB_IDX,
// bypassing Paint/rotation entirely so there is no ambiguity about which
// physical chip owns which bar: cols 0..49 = master, cols 50..99 = slave.
#define HS_MASTER_C0 6
#define HS_MASTER_C1 43
#define HS_MASTER_R0 40
#define HS_MASTER_R1 120
#define HS_SLAVE_C0 56
#define HS_SLAVE_C1 93
#define HS_SLAVE_R0 150
#define HS_SLAVE_R1 230

static uint8_t g_hs_step = 0;

static void fill_native(uint8_t c0, uint8_t c1, uint16_t r0, uint16_t r1, uint8_t v) {
  for (uint16_t r = r0; r <= r1 && r < Gate_BITS; r++)
    for (uint8_t c = c0; c <= c1 && c < FB_WBYTES; c++)
      ImageBW[FB_IDX(r, c)] = v;
}

  // Decisive isolation test. Everything static: one rectangle, one windowed
  // write, one update. No animation, no trail mask, no history, no moving
  // window. If the bar lands square and clean, the windowed transport addresses
  // RAM correctly and the fault is in the coverage logic around it. If it lands
  // skewed, offset, or striped, the transport itself is wrong and no amount of
  // coverage bookkeeping will help.
static void run_wintest(void) {
    g_running = false;
    Serial.println("WINTEST: clearing to a known state...");
    epd_full_clear();
    render_frame_static();
    epd_seed();
    // MUST re-arm. epd_full_clear() and epd_seed() both go through the vendor
    // OTP path, which drops the custom LUT — and firing 0x0C against the OTP
    // waveform is undefined. Without this the test measured 243 ms of factory
    // waveform and rendered inverted, which looks exactly like a transport fault
    // and is not one.
    apply_mode_wave();

    // Written straight into the native buffer, bypassing Paint and rotation, so
    // there is no question about which physical area it should occupy. Columns
    // 20..45 are master-only; the notch makes a mirror or flip obvious rather
    // than merely plausible.
    memset(ImageBW, 0xFF, sizeof(ImageBW));
    fill_native(20, 45, 60, 200, 0x00);   // solid bar
    fill_native(20, 27, 60, 90, 0xFF);    // notch in one corner

    // Window snapped to exactly the bar's native extent.
    uint8_t c0 = 0xFF, c1 = 0;
    uint16_t r0 = 0xFFFF, r1 = 0;
    for (uint16_t r = 0; r < Gate_BITS; r++)
      for (uint8_t c = 0; c < FB_WBYTES; c++)
        if (ImageBW[FB_IDX(r, c)] != 0xFF) {
          if (c < c0) c0 = c;
          if (c > c1) c1 = c;
          if (r < r0) r0 = r;
          if (r > r1) r1 = r;
        }
    if (c0 > c1) { Serial.println("WINTEST: nothing drawn?"); return; }

    uint32_t bytes = 0;
    EPD_GPIOInit();
    EPD_WriteWindowMasked(ImageBW, nullptr, c0, c1, r0, r1, true, &bytes);
    uint32_t busy = 0;
    fire_update(eff_seq(), &busy);
    memcpy(ImagePrev, ImageBW, sizeof(ImagePrev));
    trail_reset();

    Serial.printf("WINTEST window=%u,%u..%u,%u bytes=%lu busy_ms=%lu\n", (unsigned)c0,
                  (unsigned)r0, (unsigned)c1, (unsigned)r1, (unsigned long)bytes,
                  (unsigned long)(busy / 1000UL));
    Serial.println("LOOK: one solid bar, notch at ONE corner, nothing else on screen.");
    Serial.println("  clean  -> transport is fine, the bug is in window coverage");
    Serial.println("  broken -> the windowed transport itself addresses RAM wrongly");
  }

// Master bar high-left, slave bar low-right — diagonal, so which one moved is
// unmistakable even if the panel is held in either orientation.
static void hs_build_pattern(void) {
  memset(ImageBW, 0xFF, sizeof(ImageBW));
  fill_native(HS_MASTER_C0, HS_MASTER_C1, HS_MASTER_R0, HS_MASTER_R1, 0x00);
  fill_native(HS_SLAVE_C0, HS_SLAVE_C1, HS_SLAVE_R0, HS_SLAVE_R1, 0x00);
}

// Fire a target's update and report whether BUSY ever asserted.
// reg22/reg20 = 0x22/0x20 for master, 0xA2/0xA0 for slave.
static uint32_t hs_fire(uint8_t reg22, uint8_t seq, uint8_t reg20, bool *asserted) {
  EPD_WR_REG(reg22);
  EPD_WR_DATA8(seq);
  EPD_WR_REG(reg20);

  // Give BUSY up to 5 ms to rise. No rise at all == the chip ignored us.
  uint32_t t0 = micros();
  bool a = false;
  while ((micros() - t0) < 5000UL) {
    if (EPD_ReadBUSY != 0) { a = true; break; }
  }
  *asserted = a;
  return a ? EPD_WaitBusy_us(4000) : 0;
}

static void hs_baseline(void) {
  memset(ImageBW, 0xFF, sizeof(ImageBW));
  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  EPD_Display_Clear();
  EPD_Update();
  EPD_LoadWhiteBothPlanes();
}

// NEW = both bars, OLD = white. A differential update must therefore change
// BOTH halves — so whichever half moves is the half that actually fired.
static void hs_arm(void) {
  hs_build_pattern();
  EPD_GPIOInit();
  EPD_RestoreFullWindows();
  EPD_Display(ImageBW);
}

static void halfsplit_step(void) {
  bool a = false;
  uint32_t b = 0;

  switch (g_hs_step) {
    case 0:
      Serial.println("\n--- HALFSPLIT step 1/5: BASELINE ---");
      hs_baseline();
      Serial.println("LOOK: panel fully WHITE. press RIGHT for next step");
      break;

    case 1:
      Serial.println("\n--- step 2/5: ARM (write NEW plane only, no update) ---");
      hs_arm();
      Serial.println("LOOK: still WHITE — nothing fired yet. RAM now holds 2 bars:");
      Serial.println("      master bar cols 6-43, slave bar cols 56-93");
      break;

    case 2:
      Serial.println("\n--- step 3/5: FIRE SLAVE ONLY (0xA2 / 0xA0) ---");
      EPD_GPIOInit();
      b = hs_fire(0xA2, 0xDC, 0xA0, &a);
      Serial.printf("RESULT halfsplit fire=slave busy_asserted=%u busy_ms=%lu\n",
                    (unsigned)a, (unsigned long)(b / 1000UL));
      if (!a)
        Serial.println("VERDICT: BUSY never rose -> 0xA0 ignored. World B (cascaded).");
      else
        Serial.println("LOOK: ONE bar (slave only) = independent. BOTH = broadcast.");
      break;

    case 3:
      Serial.println("\n--- step 4/5: FIRE MASTER ONLY (0x22 / 0x20) ---");
      EPD_GPIOInit();
      b = hs_fire(0x22, 0xDC, 0x20, &a);
      Serial.printf("RESULT halfsplit fire=master busy_asserted=%u busy_ms=%lu\n",
                    (unsigned)a, (unsigned long)(b / 1000UL));
      Serial.println("LOOK: if the master bar appears NOW, per-chip activate works.");
      break;

    case 4: {
      Serial.println("\n--- step 5/5: STAGGERED (master, +168 ms, slave) ---");
      hs_baseline();
      hs_arm();
      uint32_t t0 = micros();
      bool am = false, as = false;
      EPD_WR_REG(0x22);
      EPD_WR_DATA8(0xDC);
      EPD_WR_REG(0x20);
      uint32_t tm = micros();
      while ((micros() - tm) < 5000UL)
        if (EPD_ReadBUSY != 0) { am = true; break; }
      delay(168);
      uint32_t bs = hs_fire(0xA2, 0xDC, 0xA0, &as);
      uint32_t total = micros() - t0;
      Serial.printf(
          "RESULT halfsplit fire=stagger master_asserted=%u slave_asserted=%u "
          "slave_busy_ms=%lu total_ms=%lu\n",
          (unsigned)am, (unsigned)as, (unsigned long)(bs / 1000UL),
          (unsigned long)(total / 1000UL));
      Serial.println("LOOK: both bars present? did they land at different times?");
      Serial.println("total_ms < 672 means the two waveforms overlapped.");
      break;
    }

    default:
      Serial.println("\n--- HALFSPLIT done. press RIGHT to run again ---");
      g_hs_step = 0;
      return;
  }
  g_hs_step++;
}

static void enter_mode(uint8_t m, bool announce) {
  g_mode = m % kModeCount;
  g_mode_frames = 0;
  g_mode_busy_sum = 0;
  if (mode_info().manual) {
    // Step-driven: do NOT seed or paint circles — the test owns the glass.
    g_running = false;
    g_hs_step = 0;
    Serial.println("HALFSPLIT armed — press RIGHT (or `step`) to advance each stage");
  } else {
    render_frame_static();  // no discs in the seed — see render_frame_static()
    epd_seed();             // fires 0xDC, so this restores the factory LUT ...
    apply_mode_wave();      // ... and only now is the mode's waveform armed
  }
  if (announce) {
    const ModeInfo &mi = mode_info();
    Serial.printf("MODE %u/%u %s seq=0x%02X old=%u — %s\n", (unsigned)(g_mode + 1),
                  (unsigned)kModeCount, mi.name, (unsigned)mi.seq22,
                  (unsigned)mi.old_plane, mi.note);
  }
}

// A sweep runs many frames without returning to loop(), so it needs its own way
// out. The command parser is deliberately NOT re-entered here — any serial byte
// or a LEFT press just aborts, which cannot recurse back into a sweep.
static volatile bool g_abort_sweep = false;
// `steps` isolates each value behind a GC clear by default; turn it off to judge
// how a setting behaves once ghosting has had a chance to build up.
static bool g_step_clear = true;
// Hold at the end of every step until told to move on. OFF by default: a panel
// that stops animating and waits is indistinguishable from a hung one, and it
// gets reported as a crash. Opt in with `stephold 1` when you want it.
static bool g_step_hold = false;

// While a step is animating, only LEFT / `stop` gets out. Anything else is
// ignored so a stray byte cannot silently kill a three-minute run.
static void poll_serial_pump(void) {
  if (g_act == ACT_STOP) {
    g_act = ACT_NONE;
    g_abort_sweep = true;
  }
}

enum : uint8_t { STEP_WAIT = 0, STEP_NEXT = 1, STEP_ABORT = 2 };

// Between steps the panel holds still and waits for a decision, so there is as
// much time to look as you want. RIGHT or any serial line advances; LEFT or
// `stop` ends the run.
static uint8_t step_input(void) {
  uint8_t a = g_act;
  if (a != ACT_NONE) {
    g_act = ACT_NONE;
    return (a == ACT_STOP) ? STEP_ABORT : STEP_NEXT;
  }
  static char buf[16];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;
      bool stop = !strcmp(buf, "stop") || !strcmp(buf, "q");
      len = 0;
      return stop ? STEP_ABORT : STEP_NEXT;
    }
    if (len < sizeof(buf) - 1) buf[len++] = c;
  }
  return STEP_WAIT;
}

// ---- timing law -------------------------------------------------------------
//
// The one measurement that decides whether any of this can work. All five LUT
// blocks run `tp` frames at VS = VSS: the panel burns the full waveform time
// with zero field across the pixels, so the glass cannot change and the only
// thing produced is BUSY. Sweeping tp gives the frame period as a slope and the
// fixed per-update overhead as an intercept; repeating it per FR code says
// whether LUT byte 224 is honoured at all on this cascade.
//
// With that in hand, "how fast can this panel go" stops being guesswork:
// busy_ms = overhead + frames x period, and frames is ours to choose.
static void run_calib(void) {
  static const uint8_t kFr[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0A, 0x0B, 0x0C};
  static const uint8_t kTp[] = {1, 2, 4, 8, 16, 32};
  const uint8_t nfr = sizeof(kFr);
  const uint8_t ntp = sizeof(kTp);

  Serial.printf("CALIB seq=0x%02X — all-VSS waveform, no field on the glass, image must not change\n",
                (unsigned)eff_seq());
  Serial.println("  fr    nominal      tp=1    tp=2    tp=4    tp=8   tp=16   tp=32   us/frame  fixed_ms");

  for (uint8_t i = 0; i < nfr; i++) {
    uint32_t us[6];
    for (uint8_t j = 0; j < ntp; j++) {
      wave_build_null(g_ws, kTp[j], kFr[i]);
      wave_load(g_ws, g_dual, WV_V_NONE);
      uint32_t busy = 0;
      fire_update(eff_seq(), &busy);
      us[j] = busy;
      delay(20);
    }
    // Slope across the widest span available; 31 frames of separation swamps
    // the microsecond noise on the BUSY pin.
    int32_t slope = (int32_t)(us[ntp - 1] - us[0]) / (int32_t)(kTp[ntp - 1] - kTp[0]);
    int32_t fixed = (int32_t)us[0] - slope * (int32_t)kTp[0];
    Serial.printf("  0x%02X  %-9s", (unsigned)kFr[i], wave_fr_name(kFr[i]));
    for (uint8_t j = 0; j < ntp; j++)
      Serial.printf("%6lu  ", (unsigned long)(us[j] / 1000UL));
    Serial.printf("%8ld  %8.1f\n", (long)slope, (double)fixed / 1000.0);
  }

  Serial.println("CALIB done. If us/frame is flat across fr, byte 224 is ignored and");
  Serial.println("the frame period is fixed by the gate scan — then only frame COUNT helps.");
  apply_mode_wave();
}

// Where the fixed cost actually lives. Same null waveform, same frame rate, only
// the activate byte changes — so the difference between rows is entirely the
// power sequencing each byte asks for.
static void run_seqscan(void) {
  static const uint8_t kSeq[] = {0xCF, 0xDC, 0xCC, 0x8C, 0x0C};
  static const char *kWhat[] = {
      "ramp up + display + ramp down (documented b/w display)",
      "ramp up + load temp + display, rails LEFT UP (vendor partial)",
      "ramp up + display, rails left up",
      "clock only + display  (needs rails already up)",
      "display only          (needs clock + rails already up)",
  };
  uint8_t saved = g_seq;

  Serial.println("SEQSCAN — null waveform at 125 Hz, glass must not change");
  Serial.println("  seq   tp=1    tp=32   us/frame  fixed_ms  meaning");
  for (uint8_t i = 0; i < sizeof(kSeq); i++) {
    g_seq = kSeq[i];
    uint32_t a = 0, b = 0;
    wave_build_null(g_ws, 1, 0x05);
    wave_load(g_ws, g_dual, WV_V_NONE);
    fire_update(kSeq[i], &a);
    delay(20);
    wave_build_null(g_ws, 32, 0x05);
    wave_load(g_ws, g_dual, WV_V_NONE);
    fire_update(kSeq[i], &b);
    delay(20);
    int32_t slope = (int32_t)(b - a) / 31;
    int32_t fixed = (int32_t)a - slope;
    Serial.printf("  0x%02X %6lu  %6lu  %8ld  %8.1f  %s\n", (unsigned)kSeq[i],
                  (unsigned long)(a / 1000UL), (unsigned long)(b / 1000UL), (long)slope,
                  (double)fixed / 1000.0, kWhat[i]);
  }
  g_seq = saved;
  Serial.println("SEQSCAN done — the cheapest row is the per-frame floor this panel can reach");
  apply_mode_wave();
}

// ---- DC-buildup fix comparison ---------------------------------------------
//
// One candidate per entry, each named on the glass, each held until a button
// says move on. The panel keeps animating the whole time — a still panel reads
// as a hung one.
struct FixCfg {
  const char *name;   // 24 px banner
  const char *note;   // 12 px under it
  uint8_t seq;        // activate byte for this candidate
  uint8_t volt_mask;  // which voltage registers to push
  uint8_t vcom;
  bool balance;       // charge-balanced filler on the idle LUTs
  uint16_t scrub;     // false-diff scrub every N frames (0 = off)
};

static const FixCfg kFixes[] = {
    {"1 BASELINE", "no fix - reference", 0x0C, WV_V_NONE, 0x08, false, 0},
    {"2 VCOM -0.2V", "0x2C=0x08 alone, no EOPT", 0x0C, WV_V_VCOM, 0x08, false, 0},
    {"3 VCOM -3V", "0x2C=0x78 - bias the other way", 0x0C, WV_V_VCOM, 0x78, false, 0},
    {"4 BALANCE", "+-15V filler on idle LUTs, 0 frames", 0x0C, WV_V_NONE, 0x08, true, 0},
    {"5 VCOM+BAL", "both together", 0x0C, WV_V_VCOM, 0x08, true, 0},
    {"6 RAILS DOWN", "0xCF - rails cycle, costs +124ms", 0xCF, WV_V_NONE, 0x08, false, 0},
    {"7 SCRUB /24", "baseline + falsediff scrub", 0x0C, WV_V_NONE, 0x08, false, 24},
};
static const uint8_t kFixCount = sizeof(kFixes) / sizeof(kFixes[0]);

static uint8_t mode_with_wave(uint8_t wv) {
  for (uint8_t i = 0; i < kModeCount; i++)
    if (kModes[i].wave == wv) return i;
  return 0;
}

// Hold one drive value for `reps` animation frames with the value named on the
// glass in 24 px, then move to the next.
//
// With clear_each, every step starts from a GC-cleared panel and a fresh seed,
// so what you are looking at is that setting alone and not ghosting inherited
// from the slower setting before it. It costs a full-screen flash per step —
// that is the price of an isolated comparison, and it is why the continuous
// (no-clear) form still exists for judging accumulated behaviour.
// Dwell is in MILLISECONDS, not frames: a frame costs 370 ms at drive=30 and
// 134 ms at drive=1, so a fixed frame count gives the fast settings a third of
// the viewing time — exactly backwards, since those are the ones being judged.
static bool run_steps(const uint8_t *vals, uint8_t n, uint32_t dwell_ms, bool clear_each) {
  if (mode_info().wave != WV_CUSTOM) enter_mode(mode_with_wave(WV_CUSTOM), true);
  if (dwell_ms < 200) dwell_ms = 200;
  uint8_t saved = g_wp.drive_tp;
  g_abort_sweep = false;

  if (clear_each) {
    epd_full_clear();
    render_frame_static();
    epd_seed();
  }

  int i = 0;
  while (i >= 0 && i < (int)n) {
    g_wp.drive_tp = vals[i];
    apply_mode_wave();
    // A step run owns the buttons, so the glass has to say so — otherwise the
    // wheel "not cycling modes" looks like the app is broken.
    snprintf(g_label_override, sizeof(g_label_override), "MODE %u/%u", (unsigned)(i + 1),
             (unsigned)n);
    set_label();
    snprintf(g_note, sizeof(g_note), "%lu ms wave", (unsigned long)predicted_busy_ms());
    snprintf(g_note2, sizeof(g_note2), "%lu ms total",
             (unsigned long)total_frame_ms(predicted_busy_ms()));

    if (clear_each) {
      // Clear, repaint the static frame with the new banner, then re-arm: both
      // epd_full_clear() and epd_seed() go through the vendor OTP path and drop
      // the custom LUT on the floor.
      epd_full_clear();
      render_frame_static();
      epd_seed();
      apply_mode_wave();
    }

    Serial.printf("STEP %u/%u drive=%u predicted_ms=%lu frames=%u\n", (unsigned)(i + 1),
                  (unsigned)n, (unsigned)vals[i], (unsigned long)predicted_busy_ms(),
                  (unsigned)wave_frames(g_ws));

    // Buttons act IMMEDIATELY. An earlier version swallowed presses during a
    // minimum dwell, which just made the wheel feel dead. The circles keep
    // moving the whole time, so nothing ever looks hung.
    uint32_t t_start = millis();
    uint32_t shown = 0;
    int next = i + 1;
    for (;;) {
      uint8_t a = g_act;
      if (a != ACT_NONE) {
        g_act = ACT_NONE;
        if (a == ACT_STOP) { g_abort_sweep = true; break; }
        if (a == ACT_MODE_DN) {  // left button = go back
          // At the first value there is nowhere to go back to. Ignore it —
          // re-entering the same step would GC-clear and replay, which reads as
          // a button that does nothing.
          if (i > 0) { next = i - 1; break; }
          Serial.println("STEP already at the first value");
        } else {
          next = i + 1;
          break;
        }
      }
      uint8_t in = step_input();
      if (in == STEP_ABORT) { g_abort_sweep = true; break; }
      if (in == STEP_NEXT) break;
      if (!g_step_hold && (millis() - t_start) >= dwell_ms) break;

      advance_motion();
      render_frame();
      show_frame();
      g_frame++;
      shown++;
    }

    Serial.printf("STEP %u/%u drive=%u measured_ms=%lu shown_frames=%lu in %lu ms\n",
                  (unsigned)(i + 1), (unsigned)n, (unsigned)vals[i],
                  (unsigned long)(g_last_busy_us / 1000UL), (unsigned long)shown,
                  (unsigned long)(millis() - t_start));
    if (g_abort_sweep) break;
    i = next < 0 ? 0 : next;  // the while condition ends the run past the last value
  }

  // Hand the buttons back, and make sure the glass stops claiming otherwise.
  // The drive value you stopped on is KEPT, aborted or not — throwing it away
  // and snapping back to the default is how the run ended up looking stuck on 14.
  (void)saved;
  g_label_override[0] = 0;
  g_note[0] = 0;
  g_note2[0] = 0;
  bool aborted = g_abort_sweep;
  g_abort_sweep = false;
  apply_mode_wave();
  render_frame();
  show_frame();
  Serial.printf("STEPS %s at drive=%u — buttons are back to mode cycling.\n",
                aborted ? "ended" : "done", (unsigned)g_wp.drive_tp);
  return !aborted;
}

// Walk the fix candidates. Every one animates continuously and holds until a
// button; nothing is on a timer, because judging a slow drift takes as long as
// it takes. UP/DOWN move back and forth so two candidates can be compared
// directly rather than only in sequence.
static void run_fixes(void) {
  if (mode_info().wave != WV_CUSTOM) enter_mode(mode_with_wave(WV_CUSTOM), true);
  uint8_t saved_seq = g_seq;
  WaveParams saved = g_wp;
  int idx = 0;
  g_abort_sweep = false;

  Serial.println("FIXES — RIGHT/DOWN/SEL next, UP prev, LEFT end");

  while (idx >= 0 && idx < kFixCount) {
    const FixCfg &f = kFixes[idx];
    g_seq = f.seq;
    g_wp.volt_mask = f.volt_mask;
    g_wp.vcom = f.vcom;
    g_wp.balance_idle = f.balance;
    snprintf(g_label_override, sizeof(g_label_override), "%s", f.name);
    snprintf(g_note, sizeof(g_note), "%s", f.note);
    apply_mode_wave();

    if (g_step_clear) {
      epd_full_clear();
      render_frame_static();
      epd_seed();
      apply_mode_wave();  // the clear and the seed both go through OTP
    }

    Serial.printf("FIX %s | %s | seq=0x%02X volt_mask=%u vcom=0x%02X balance=%u "
                  "scrub=%u predicted_ms=%lu\n",
                  f.name, f.note, (unsigned)f.seq, (unsigned)f.volt_mask,
                  (unsigned)f.vcom, (unsigned)f.balance, (unsigned)f.scrub,
                  (unsigned long)predicted_busy_ms());

    int next = idx + 1;
    uint32_t n_in_fix = 0;
    for (;;) {
      uint8_t a = g_act;
      if (a != ACT_NONE) {
        g_act = ACT_NONE;
        if (a == ACT_STOP) { g_abort_sweep = true; break; }
        if (a == ACT_MODE_DN) { next = idx - 1; break; }  // left — back and compare
        next = idx + 1;                                   // anything else advances
        break;
      }
      if (step_input() == STEP_NEXT) { next = idx + 1; break; }

      advance_motion();
      render_frame();
      show_frame();
      g_frame++;
      n_in_fix++;

      if (f.scrub && (n_in_fix % f.scrub) == 0) {
        uint32_t junk = 0;
        for (uint8_t p = 0; p < 2; p++) one_pass(OLD_INV, 0xDC, &junk);
        apply_mode_wave();  // 0xDC dropped the custom LUT
        Serial.printf("FIX %s scrub after %lu frames\n", f.name, (unsigned long)n_in_fix);
      }
    }

    Serial.printf("FIX %s ran %lu frames, busy=%lu ms\n", f.name, (unsigned long)n_in_fix,
                  (unsigned long)(g_last_busy_us / 1000UL));
    if (g_abort_sweep) break;
    idx = next < 0 ? 0 : next;
  }

  g_label_override[0] = 0;
  g_note[0] = 0;
  g_note2[0] = 0;
  g_seq = saved_seq;
  g_wp = saved;
  g_abort_sweep = false;
  apply_mode_wave();
  Serial.println("FIXES done");
}

// Walk the drive-phase length down one frame at a time.
static void run_sweep(int from, int to, int secs) {
  uint8_t vals[64];
  uint8_t n = 0;
  for (int d = from; d >= to && n < sizeof(vals); d--) vals[n++] = (uint8_t)d;
  Serial.printf("SWEEP drive %d -> %d, %d s each. Watch the glass; any key aborts.\n",
                from, to, secs);
  run_steps(vals, n, (uint32_t)secs * 1000UL, false);  // continuous — no flash between values
}

// ---- serial -----------------------------------------------------------------
static void print_wave(void) {
  const ModeInfo &m = mode_info();
  Serial.printf("WAVE mode=%s reset=%u drive=%u settle=%u rp=%u sr=%u fr=0x%02X(%s) "
                "invert=%u touch_bb=%u dual=%u volt=%u\n",
                m.name, (unsigned)g_wp.reset_tp, (unsigned)g_wp.drive_tp,
                (unsigned)g_wp.settle_tp, (unsigned)g_wp.rp, (unsigned)g_wp.sr_ab,
                (unsigned)g_wp.fr, wave_fr_name(g_wp.fr), (unsigned)g_wp.invert,
                (unsigned)g_wp.touch_bb, (unsigned)g_dual, (unsigned)g_wp.volt_mask);
  Serial.printf("     VGH=0x%02X VSH1=0x%02X VSH2=0x%02X VSL=0x%02X VCOM=0x%02X EOPT=0x%02X\n",
                (unsigned)g_wp.vgh, (unsigned)g_wp.vsh1, (unsigned)g_wp.vsh2,
                (unsigned)g_wp.vsl, (unsigned)g_wp.vcom, (unsigned)g_wp.eopt);
  Serial.printf("     probe: block=%u level=%u tp=%u   gd_fr=0x%02X\n",
                (unsigned)g_probe_block, (unsigned)g_probe_level, (unsigned)g_probe_tp,
                (unsigned)g_gd_fr);
  if (g_wave_loaded) wave_dump(g_ws);
}

static void print_modes(void) {
  static const char *kWave[5] = {"otp3", "otpbw", "custom", "gd26", "probe"};
  for (uint8_t i = 0; i < kModeCount; i++)
    Serial.printf("  %u %-13s seq=0x%02X old=%u lut=%-6s %s\n", (unsigned)(i + 1),
                  kModes[i].name, (unsigned)kModes[i].seq22,
                  (unsigned)kModes[i].old_plane, kWave[kModes[i].wave], kModes[i].note);
}

static void print_status(void) {
  const ModeInfo &m = mode_info();
  Serial.printf(
      "STATUS mode=%u/%u %s seq=0x%02X old=%u run=%u frame=%lu radius=%d "
      "speed=%d clean_every=%u clean_passes=%u spi=%s last_busy_ms=%lu "
      "avg_busy_ms=%lu\n",
      (unsigned)(g_mode + 1), (unsigned)kModeCount, m.name, (unsigned)m.seq22,
      (unsigned)m.old_plane, (unsigned)g_running, (unsigned long)g_frame, g_radius,
      g_speed, (unsigned)g_clean_every, (unsigned)g_clean_passes,
      EPD_SpiPathName(), (unsigned long)(g_last_busy_us / 1000UL),
      (unsigned long)(g_mode_frames ? (g_mode_busy_sum / g_mode_frames / 1000ULL)
                                    : 0ULL));
}

// ---- USB video streaming ----------------------------------------------------
// Frames arrive as a DELTA against the previous frame, in the panel's own buffer
// layout, so the device transforms nothing — it scatters the changed bytes into
// ImageBW and fires a normal differential update.
//
// Wire format, per frame:
//   3400 bytes   bitmap, one bit per framebuffer byte, set = that byte changed
//   N bytes      the new value of each changed byte, in order
//
// N is never transmitted. Both ends derive it by counting set bits, so there is no
// length field that can fall out of step. Measured on Big Buck Bunny at 792x272:
// 6837 B/frame average, 3.98x, worst case 25899 B on a scene cut.
//
// The dither has to be a fixed ordered matrix for this to work at all — a static
// area then produces byte-identical output every frame and drops out of the delta
// completely. Error diffusion would perturb every pixel and leave nothing to remove.
#define STREAM_BMP_BYTES (PLANE_BYTES / 8)
// 12288, matching the 4.26" reference. 4096 was a porting error: the reference notes
// chunk size "turned out not to affect throughput at all", but that was measured on
// NATIVE USB where a round trip is cheap. Over a CH340 every ack round trip pays USB's
// 1 ms frame quantum plus bridge latency, and at 4096 a frame costs 2-3 of them —
// ~4-6 ms per frame that never appears in the byte count. Safe now the rx buffer is
// 64 KB rather than the 256-byte default.
#define STREAM_CHUNK 12288
#define STREAM_BAUD 2000000UL

typedef struct {
  uint8_t *bmp;      // STREAM_BMP_BYTES
  uint8_t *pay;      // up to PLANE_BYTES
  uint32_t paylen;
} StreamSlot;

// The delta chain applies to THIS buffer, never to ImageBW.
//
// If the HUD were drawn into ImageBW, the next frame's delta would land on top of
// HUD pixels and every byte under the text would be permanently wrong — the host
// believes it is tracking pure video. So: delta -> g_vid, then g_vid -> ImageBW,
// then the overlay on top. One extra 27 KB memcpy, about 1 ms.
static uint8_t g_vid[(uint32_t)EPD_H * FB_WBYTES];
static volatile bool g_stream_restart = false;

static StreamSlot g_sslot[2];
static SemaphoreHandle_t g_sem_free = nullptr, g_sem_ready = nullptr;
static volatile bool g_srx_run = false, g_srx_done = false;
static volatile uint32_t g_stream_bad = 0;  // frames whose checksum did not match
static volatile uint32_t g_srx_frames = 0;

// Pull exactly `total` bytes, asking for them a chunk at a time.
//
// Bulk Serial.read(buf, n), NEVER Serial.readBytes(): the latter is
// Stream::readBytes, which calls read() once per byte and so performs one queue
// operation per byte — 27200 of them for a worst-case frame.
static bool stream_read_exact(uint8_t *dst, uint32_t total) {
  uint32_t off = 0;
  while (off < total) {
    uint32_t want = total - off;
    if (want > STREAM_CHUNK) want = STREAM_CHUNK;
    Serial.write((uint8_t)0x06);  // ACK: ask the host for the next chunk

    uint32_t have = 0, tw = millis(), reack = millis();
    while (have < want && (millis() - tw) < 4000) {
      int n = Serial.read(dst + off + have, want - have);
      if (n > 0) {
        have += (uint32_t)n;
        tw = millis();
      } else {
        // Re-send the ACK while nothing has arrived yet. The FIRST ack of a stream
        // goes out microseconds after the device changes baud, so if the host has
        // not switched yet it is simply lost — and a one-shot ack then deadlocks
        // until the 4 s timeout kills the stream. Repeating it makes the handshake
        // self-healing across the rate change. Only while have == 0: mid-chunk a
        // stray ack would desynchronise the host.
        if (have == 0 && (millis() - reack) > 150) {
          Serial.write((uint8_t)0x06);
          reack = millis();
        }
        // delay(1), not delay(0): vTaskDelay(0) yields only to tasks of equal or
        // higher priority, so this task would starve its core's IDLE task and the
        // watchdog would panic the board.
        delay(1);
      }
    }
    if (have != want) return false;  // short read = host finished or stalled
    off += want;
  }
  return true;
}

static uint32_t popcount_bytes(const uint8_t *p, uint32_t n) {
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) c += (uint32_t)__builtin_popcount(p[i]);
  return c;
}

// Receive on the OTHER core, into one of two slots.
//
// On the sibling panel USB and the panel ran in SERIES: the link measured 4.9 fps
// alone, the panel 8.6, and together exactly 3.1 = 1/(1/4.9 + 1/8.6). Receiving on
// its own core into a ping-pong buffer makes the SLOWER of the two set the rate
// instead of their sum.
static void stream_rx_task(void *) {
  uint8_t cur = 0;
  while (g_srx_run) {
    if (xSemaphoreTake(g_sem_free, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    StreamSlot *s = &g_sslot[cur];
    if (!stream_read_exact(s->bmp, STREAM_BMP_BYTES)) break;
    s->paylen = popcount_bytes(s->bmp, STREAM_BMP_BYTES);
    if (s->paylen > PLANE_BYTES) break;
    if (!stream_read_exact(s->pay, s->paylen + 1)) break;  // payload + checksum

    // One trailing checksum byte per frame, read as part of the SAME transfer as the
    // payload.
    //
    // It must not be a separate stream_read_exact: each call issues one ack per 4096
    // byte chunk, and the host sends payload+checksum as a single npay+1 stream. Two
    // calls means the device asks for one more chunk than the host sends, so it then
    // reads the NEXT frame's bitmap as this frame's checksum and both ends desync
    // permanently. That produced badck on 272 of 376 frames and looked exactly like a
    // failing link.
    //
    // Without it a dropped byte is INVISIBLE to the protocol: the device still reads
    // the popcount-derived number of bytes, it just reads one byte of the next
    // frame's bitmap as payload, and from then on everything is shifted. In a
    // column-major buffer a shifted run is an 8-px vertical stripe, which is exactly
    // the artefact being chased. This turns "I think the link is dropping bytes"
    // into a number.
    uint8_t want_ck = s->pay[s->paylen];
    uint8_t got_ck = 0;
    for (uint32_t i = 0; i < STREAM_BMP_BYTES; i++) got_ck ^= s->bmp[i];
    for (uint32_t i = 0; i < s->paylen; i++) got_ck ^= s->pay[i];
    // Written only here, on the receive task; read by the reporter. Spelled
    // out rather than `++` because incrementing a volatile is deprecated in
    // C++20 and the warning is not worth leaving in a reference tree.
    if (got_ck != want_ck) g_stream_bad = g_stream_bad + 1;
    xSemaphoreGive(g_sem_ready);
    cur ^= 1;
  }
  g_srx_done = true;
  g_srx_run = false;
  vTaskDelete(nullptr);
}

static void stream_apply(const StreamSlot *s) {
  const uint8_t *pay = s->pay;
  uint32_t k = 0;
  for (uint32_t i = 0; i < STREAM_BMP_BYTES; i++) {
    uint8_t b = s->bmp[i];
    if (!b) continue;
    uint32_t base = i * 8;
    for (uint8_t bit = 0; bit < 8; bit++)
      if (b & (0x80 >> bit)) g_vid[base + bit] = pay[k++];
  }
}

// Composite what the panel shows: clean video, then a compact overlay. ImageBW is
// scratch as far as the delta chain is concerned.
// Cached HUD strip.
//
// The overlay lives in portrait y 36..148, and portrait y maps to native X as
// 799 - y, so it occupies native byte columns ~81..95. The framebuffer is
// column-major, so those columns are ONE CONTIGUOUS RUN — which makes the whole
// overlay a single memcpy instead of ~40000 Paint_SetPixel calls per frame (a
// switch, a seam test and a read-modify-write each). Re-rasterised only when the
// text actually changes, i.e. once a second.
#define SHUD_C0 80
#define SHUD_C1 96
#define SHUD_OFF ((uint32_t)SHUD_C0 * Gate_BITS)
#define SHUD_BYTES ((uint32_t)(SHUD_C1 - SHUD_C0 + 1) * Gate_BITS)
static uint8_t g_shud[SHUD_BYTES];
static char g_shud_txt[24] = "";
static bool g_shud_valid = false;

static void stream_compose(void) {
  memcpy(ImageBW, g_vid, sizeof(ImageBW));

  char want[24];
  snprintf(want, sizeof(want), "%u|%s", (unsigned)g_wp.drive_tp,
           g_fps_num[0] ? g_fps_num : "--");
  if (g_shud_valid && !strcmp(want, g_shud_txt)) {
    memcpy(ImageBW + SHUD_OFF, g_shud, SHUD_BYTES);
    return;
  }
  memcpy(g_shud_txt, want, sizeof(g_shud_txt));
  g_shud_valid = true;
  paint_bind(ImageBW);
  // Size 48, the largest the font blob carries (12/16/24/48 only), and placed at
  // y=50 so it lands inside the letterbox band above a rotated video rather than on
  // top of the picture. Glyph advance is size/2, so 7 chars is 168 px of the 272
  // available. At 24 it was unreadable from a normal viewing distance.
  // Two lines rather than one: at size 48 the advance is 24 px per glyph, so
  // "MODE 4" plus "15.2 fps" on one line would be 15 chars = 360 px against the 272
  // available. Split, both still inside the letterbox band (y 0..153) above a
  // rotated video.
  char line[16];
  snprintf(line, sizeof(line), "MODE %u", (unsigned)g_wp.drive_tp);
  hud_text(12, 40, line, 48);
  snprintf(line, sizeof(line), "%s fps", g_fps_num[0] ? g_fps_num : "--");
  hud_text(12, 96, line, 48);
  // Snapshot the strip for the frames in between. The video is letterboxed white in
  // these columns, so nothing of the picture is baked into the cache.
  memcpy(g_shud, ImageBW + SHUD_OFF, SHUD_BYTES);
}

// `stream` — play until the host stops sending. Raises the baud for the duration
// and puts it back, so the command channel stays at 115200: a firmware locked to
// 2 Mbaud is a board you cannot talk to if the rate turns out to be unreliable.
static void stream_run(void) {
  for (int i = 0; i < 2; i++) {
    if (!g_sslot[i].bmp)
      g_sslot[i].bmp = (uint8_t *)heap_caps_malloc(STREAM_BMP_BYTES, MALLOC_CAP_SPIRAM);
    if (!g_sslot[i].pay)
      g_sslot[i].pay = (uint8_t *)heap_caps_malloc(PLANE_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!g_sslot[i].bmp || !g_sslot[i].pay) {
      Serial.println("ERR stream: no PSRAM for the ping-pong buffers");
      return;
    }
  }
  if (!g_sem_free) g_sem_free = xSemaphoreCreateCounting(2, 2);
  if (!g_sem_ready) g_sem_ready = xSemaphoreCreateCounting(2, 0);
  while (xSemaphoreTake(g_sem_ready, 0) == pdTRUE) {}
  while (xSemaphoreTake(g_sem_free, 0) == pdTRUE) {}
  xSemaphoreGive(g_sem_free);
  xSemaphoreGive(g_sem_free);

  g_running = false;   // the demo must not fight the stream for the panel
  fps_reset();

  // Start from a known plane state: white glass, planes agreeing, custom LUT armed.
  paint_begin();
  epd_seed();
  apply_mode_wave();
  memset(ImageBW, 0xFF, sizeof(ImageBW));
  memset(g_vid, 0xFF, sizeof(g_vid));  // the host's first delta is against white
  g_shud_valid = false;                // force one rasterise at the start of a stream

  Serial.printf("STREAM ready baud=%lu bmp=%u plane=%lu drive=%u\n",
                (unsigned long)STREAM_BAUD, (unsigned)STREAM_BMP_BYTES,
                (unsigned long)PLANE_BYTES, (unsigned)g_wp.drive_tp);
  Serial.flush();
  Serial.updateBaudRate(STREAM_BAUD);
  delay(20);

  g_srx_run = true;
  g_srx_done = false;
  g_stream_bad = 0;
  g_srx_frames = 0;
  xTaskCreatePinnedToCore(stream_rx_task, "srx", 4096, nullptr, 2, nullptr, 0);

  uint8_t cur = 0;
  uint32_t t0 = millis(), shown = 0;
  while (true) {
    if (xSemaphoreTake(g_sem_ready, pdMS_TO_TICKS(1500)) != pdTRUE) {
      if (g_srx_done) break;
      continue;
    }
    stream_apply(&g_sslot[cur]);
    xSemaphoreGive(g_sem_free);
    cur ^= 1;

    stream_compose();
    show_frame();
    g_frame++;
    shown++;
    fps_tick();

    // Buttons still work during a stream — the poll task owns core 0 and never
    // touches the serial link. A mode change LEAVES the stream rather than trying to
    // resynchronise it: the RX task may be halfway through a frame's bitmap, and
    // restarting the host mid-frame desynchronises both ends with no way back.
    // Exiting and re-entering is race-free and costs one clear.
    uint8_t act = g_act;
    if (act == ACT_MODE_UP || act == ACT_MODE_DN) {
      g_act = ACT_NONE;
      int v = (int)g_wp.drive_tp + (act == ACT_MODE_UP ? 1 : -1);
      if (v < 1) v = 10;
      if (v > 10) v = 1;
      g_wp.drive_tp = (uint8_t)v;
      g_stream_restart = true;
      break;
    }
    if (act == ACT_STOP) {
      g_act = ACT_NONE;
      break;
    }
  }

  g_srx_run = false;
  // Let the RX task notice and exit before the rate changes under it, or it writes
  // acks at 2 Mbaud into a 115200 link.
  for (int i = 0; i < 60 && !g_srx_done; i++) delay(25);
  Serial.updateBaudRate(115200);
  delay(20);

  if (g_stream_restart) {
    g_stream_restart = false;
    // Full clear so the next run starts from white, which is what the host's first
    // delta is encoded against. NO bar sweep: that parks the whole panel at the grey
    // an incomplete transition stalls at, and video has no static background for
    // that to help — it would only lift the black floor of every frame.
    epd_full_clear();
    apply_mode_wave();
    memset(g_vid, 0xFF, sizeof(g_vid));
    // The clear glitches the pulled-up button pins, and a phantom BTN_UP is ACT_RUN
    // — which is what dumped the panel into the bench demo instead of waiting for
    // the host to re-enter. Drop anything queued, and stay stopped.
    g_act = ACT_NONE;
    g_running = false;
    set_label();
    Serial.printf("STREAM restart drive=%u predicted_ms=%lu badck=%lu\n",
                  (unsigned)g_wp.drive_tp, (unsigned long)predicted_busy_ms(),
                  (unsigned long)g_stream_bad);
    return;
  }

  uint32_t ms = millis() - t0;
  Serial.printf("STREAM done %lu frames in %lu ms (%lu.%lu fps) badck=%lu\n",
                (unsigned long)shown, (unsigned long)ms,
                (unsigned long)(shown * 1000UL / (ms ? ms : 1)),
                (unsigned long)((shown * 10000UL / (ms ? ms : 1)) % 10),
                (unsigned long)g_stream_bad);
}

static void print_help(void) {
  Serial.println("commands:");
  Serial.println("  help | modes | status");
  Serial.println("  mode N | next | prev");
  Serial.println("  run | stop | step");
  Serial.println("  seed        re-sync NEW/OLD planes to current image");
  Serial.println("  clear       one GC full clear (only intentional flash)");
  Serial.println("  speed N     % of one shape-length per frame (10..400; 100 = no overlap)");
  Serial.println("  box N       Ms Pac-Man box in px (12..120) — sets the step too");
  Serial.println("  radius N    same dial in radius units (6..60)");
  Serial.println("  field       re-seed the dithered circle field without clearing");
  Serial.println("  sweep       one bar pass top-to-bottom: normalises the background shade");
  Serial.println("  stream      play delta-encoded video from the host (raises baud to 2M)");
  Serial.println("  bar N       sweep bar height, % of sprite box (10..200, default 80)");
  Serial.println("  spi bitbang | spi hw [MHz]   switch transport live");
  Serial.println("  flush [N]   deep de-ghost, N black/white cycles (default 3)");
  Serial.println("  clean N     TD_CLEAN scrub interval in frames (0=off)");
  Serial.println("  passes N    false-diff passes per scrub (1..4)");
  Serial.println("waveform (modes 5-7):");
  Serial.println("  wave        show params + decoded LUT");
  Serial.println("  calib       measure frame period vs FR — no change on glass");
  Serial.println("  seqscan     measure fixed cost per 0x22 activate byte — no change on glass");
  Serial.println("  seq X       force the activate byte (0xCF 0xDC 0xCC 0x8C 0x0C) | seq auto");
  Serial.println("  analog 0|1  power the analog rails up/down by hand");
  Serial.println("  reset N     opposite-polarity kick frames (0..63)");
  Serial.println("  drive N     target-polarity drive frames (1..63)  <- the speed knob");
  Serial.println("  settle N    VSS hold frames (0..63)");
  Serial.println("  rp N | sr N group / AB-phase repeats");
  Serial.println("  fr N        1=25 2=50 3=75 4=100 5=125Hz (9..12 = the .5 rates)");
  Serial.println("  pol 0|1     swap which rail drives to black");
  Serial.println("  bb 0|1      1-frame top-up on already-black pixels");
  Serial.println("  dual 0|1    write LUT to both cascade chips (default 1)");
  Serial.println("  fixes       walk the DC-buildup candidates, button-advanced");
  Serial.println("  volt MASK   1=EOPT 2=VGH 4=src 8=VCOM (8 = VCOM alone)");
  Serial.println("  balance 0|1 charge-balanced filler on idle LUTs (0 extra frames)");
  Serial.println("  vsh1 X | vsh2 X | vsl X | vcom X | vgh X | eopt X   (hex ok: 0x41)");
  Serial.println("  gdfr N      FR for GD26_CF (its shipped 0x08 is not a legal code)");
  Serial.println("  probe B L T drive only block B(0..4) at level L(0..3) for T frames");
  Serial.println("  sweep A B [S]  drive A->B by 1, S seconds each");
  Serial.println("  steps A,B,C [S]  dwell on just those drive values, S seconds each (default 15)");
  Serial.println("  stepclear 0|1    GC clear + reseed before each step (default 1)");
  Serial.println("  stephold 0|1     wait for RIGHT between steps instead of a timer (default 0)");
  Serial.println("buttons: LEFT/RIGHT = mode 1..10 (wraps)  UP=run  DOWN=stop  CLICK=window");
}

// Accepts 41, 0x41 or x41 — the datasheet quotes these registers in hex.
static bool parse_val(const char *s, long *out) {
  while (*s == ' ') s++;
  if (!*s) return false;
  char *end = nullptr;
  long v;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtol(s + 2, &end, 16);
  else if (s[0] == 'x' || s[0] == 'X') v = strtol(s + 1, &end, 16);
  else v = strtol(s, &end, 10);
  if (end == s) return false;
  *out = v;
  return true;
}

// One place for "set a byte, rebuild, re-arm, say so".
static bool set_wave_byte(char *s, const char *key, uint8_t *field, long lo, long hi) {
  size_t n = strlen(key);
  if (strncmp(s, key, n) || s[n] != ' ') return false;
  long v;
  if (!parse_val(s + n + 1, &v) || v < lo || v > hi) {
    Serial.printf("ERR %s %ld..%ld\n", key, lo, hi);
    return true;
  }
  *field = (uint8_t)v;
  apply_mode_wave();
  Serial.printf("OK %s=0x%02X (%u)\n", key, (unsigned)*field, (unsigned)*field);
  return true;
}

static void handle_cmd(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;

  if (!strcmp(s, "help")) { print_help(); return; }
  if (!strcmp(s, "modes")) { print_modes(); return; }
  if (!strcmp(s, "status")) { print_status(); return; }
  if (!strcmp(s, "run")) { g_running = true; Serial.println("OK run"); return; }
  if (!strcmp(s, "stop")) {
    g_running = false;
    // The fast path leaves the analog rails energised between updates. That is
    // the whole trick, but it must not be the idle state.
    if (EPD_AnalogIsOn()) analog_power(false);
    Serial.println("OK stop (rails down)");
    return;
  }
  if (!strcmp(s, "step")) { g_step_once = true; Serial.println("OK step"); return; }
  if (!strcmp(s, "next")) { enter_mode(g_mode + 1, true); return; }
  if (!strcmp(s, "prev")) { enter_mode((uint8_t)(g_mode + kModeCount - 1), true); return; }
  if (!strcmp(s, "seed")) {
    render_frame();
    epd_seed();
    apply_mode_wave();
    Serial.println("OK seed");
    return;
  }
  if (!strcmp(s, "clear")) {
    epd_full_clear();
    render_frame();
    epd_seed();
    apply_mode_wave();
    Serial.println("OK clear");
    return;
  }
  if (!strncmp(s, "result ", 7)) {
    int v = atoi(s + 7);
    if (v >= 0 && v <= 1000) {
      g_result_every = (uint16_t)v;
      Serial.printf("OK result=%u (0=off, else 1 line per N frames)\n",
                    (unsigned)g_result_every);
    } else {
      Serial.println("ERR result 0..1000");
    }
    return;
  }
  if (!strncmp(s, "old ", 4)) {
    const char *a = s + 4;
    while (*a == ' ') a++;
    if (!strncmp(a, "auto", 4)) {
      g_old_override = -1;
      Serial.printf("OK old=auto (mode default %u)\n", (unsigned)mode_info().old_plane);
      return;
    }
    int v = atoi(a);
    if (v >= 0 && v <= 3) {
      g_old_override = (int8_t)v;
      Serial.printf("OK old=%d  0=none(skip write) 1=prev 2=inv 3=trail\n", v);
    } else {
      Serial.println("ERR old 0..3 | auto");
    }
    return;
  }
  if (!strcmp(s, "stream")) {
    stream_run();
    return;
  }
  if (!strcmp(s, "sweep")) {
    bar_sweep();
    Serial.println("OK sweep");
    return;
  }
  // Bar height as a percent of the sprite box. Step follows it, so coverage stays
  // exactly-once at every setting and no band is ever driven twice.
  if (!strncmp(s, "bar ", 4)) {
    int v = atoi(s + 4);
    if (v >= 10 && v <= 200) {
      g_bar_pct = v;
      int h = (2 * g_radius * g_bar_pct) / 100;
      if (h < 4) h = 4;
      Serial.printf("OK bar=%d%% -> h=%d px (%d passes). `sweep` to apply.\n", g_bar_pct,
                    h, (PH + h - 1) / h);
    } else {
      Serial.println("ERR bar 10..200 (percent of the sprite box)");
    }
    return;
  }
  if (!strcmp(s, "field")) {  // manual only — no longer in the mode-change path
    boot_circle_field();
    Serial.println("OK field re-seeded (planes lied to white)");
    return;
  }
  if (!strcmp(s, "wave")) { print_wave(); return; }
  if (!strcmp(s, "calib")) { run_calib(); return; }
  if (!strcmp(s, "seqscan")) { run_seqscan(); return; }
  if (!strcmp(s, "fixes")) { run_fixes(); return; }
  if (!strncmp(s, "seq", 3) && (s[3] == ' ' || !s[3])) {
    if (s[3]) {
      if (!strncmp(s + 4, "auto", 4)) {
        g_seq = 0;
      } else {
        long v;
        if (!parse_val(s + 4, &v) || v < 0 || v > 255) {
          Serial.println("ERR seq 0x00..0xFF or `seq auto`");
          return;
        }
        if (!mode_is_custom() && (v & 0x40) == 0) {
          Serial.println("ERR that byte needs the rails already up — the OTP waveform "
                         "hangs on it. Switch to a custom-LUT mode first.");
          return;
        }
        g_seq = (uint8_t)v;
      }
    }
    uint8_t q = eff_seq();
    Serial.printf("SEQ 0x%02X (%s)  clk=%u analog=%u temp=%u loadlut=%u bw=%u display=%u "
                  "off_analog=%u off_clk=%u  rails_up=%u\n",
                  (unsigned)q, g_seq ? "forced" : "mode default", (unsigned)!!(q & 0x80),
                  (unsigned)!!(q & 0x40), (unsigned)!!(q & 0x20), (unsigned)!!(q & 0x10),
                  (unsigned)!!(q & 0x08), (unsigned)!!(q & 0x04), (unsigned)!!(q & 0x02),
                  (unsigned)!!(q & 0x01), (unsigned)EPD_AnalogIsOn());
    return;
  }
  if (!strncmp(s, "analog ", 7)) {
    analog_power(atoi(s + 7) != 0);
    Serial.printf("OK analog=%u\n", (unsigned)EPD_AnalogIsOn());
    return;
  }
  if (!strcmp(s, "reload")) { apply_mode_wave(); Serial.println("OK reload"); return; }

  if (set_wave_byte(s, "reset", &g_wp.reset_tp, 0, 63)) return;

  // `drive N` is the MODE dial, so it gets a GC clear and reseed of its own —
  // ahead of arming the new waveform, never after it.
  //
  // A mode has to be judged on what it puts on a BLANK panel. Switching straight
  // into it leaves the previous mode's ghosting on the glass, and that ghosting
  // then reads as the new mode's trail — which is exactly backwards when the
  // faster modes are the ones being blamed for trails. Doing the clear afterwards
  // is no better: it wipes the evidence and the first frames judged are the ones
  // recovering from the clear.
  //
  // Order is not negotiable. epd_full_clear() and epd_seed() both fire vendor
  // 0xDC, which carries the Load-LUT bit and restores the FACTORY waveform, so
  // apply_mode_wave() must be last or the custom table is silently dropped and
  // busy jumps to ~336 ms.
  if (!strncmp(s, "drive ", 6)) {
    long v;
    if (!parse_val(s + 6, &v) || v < 1 || v > 63) {
      Serial.println("ERR drive 1..63");
      return;
    }
    g_wp.drive_tp = (uint8_t)v;
    // Clear, then re-seed the circle field, then let the bench run. The field has
    // to be laid down AFTER the clear or the clear erases it, and boot_circle_field
    // re-arms the custom LUT on the way out because its own paint uses the vendor
    // waveform.
    // Two clear cycles before the sweep. The parked shade is only uniform if every
    // pixel starts from the same white, and one GC pass leaves the panel measurably
    // less even than two.
    bool was_running = g_running;
    epd_full_clear();
    epd_full_clear();
    apply_mode_wave();  // both clears run the vendor OTP path and drop the custom LUT
    // Sweep at the NEW mode, not the old one: the parked shade depends on drive
    // frames, so a background normalised at another mode will not match this one.
    bar_sweep();
    // Discard anything the clear transients queued, and put the run state back.
    g_act = ACT_NONE;
    g_running = was_running;
    Serial.printf("OK drive=%u (2x clear + sweep) predicted_ms=%lu\n",
                  (unsigned)g_wp.drive_tp, (unsigned long)predicted_busy_ms());
    return;
  }
  if (set_wave_byte(s, "settle", &g_wp.settle_tp, 0, 63)) return;
  if (set_wave_byte(s, "rp", &g_wp.rp, 1, 255)) return;
  if (set_wave_byte(s, "sr", &g_wp.sr_ab, 1, 255)) return;
  // Upper bounds are the datasheet maxima: VSH1/VSH2 0x4B = 17 V, VSL 0x3A =
  // -17 V. They used to accept 0..255, which let a typo drive the panel out of
  // spec with no warning and no benefit — above the maximum the frame count is
  // unchanged and the extra contrast is marginal, so there is nothing up there
  // to reach for. Lower bounds stay open: under-driving is a legitimate
  // experiment, over-driving is not.
  if (set_wave_byte(s, "vsh1", &g_wp.vsh1, 0, 0x4B)) return;
  if (set_wave_byte(s, "vsh2", &g_wp.vsh2, 0, 0x4B)) return;
  if (set_wave_byte(s, "vsl", &g_wp.vsl, 0, 0x3A)) return;
  if (set_wave_byte(s, "vcom", &g_wp.vcom, 0, 255)) return;
  if (set_wave_byte(s, "vgh", &g_wp.vgh, 0, 255)) return;
  if (set_wave_byte(s, "eopt", &g_wp.eopt, 0, 255)) return;

  if (!strncmp(s, "fr ", 3) || !strncmp(s, "gdfr ", 5)) {
    bool gd = (s[0] == 'g');
    long v;
    if (!parse_val(s + (gd ? 5 : 3), &v) || !wave_fr_valid((uint8_t)v)) {
      Serial.println("ERR fr must be 1..5 or 9..12 (25/50/75/100/125, 37.5/62.5/87.5/112.5 Hz)");
      return;
    }
    if (gd) g_gd_fr = (uint8_t)v; else g_wp.fr = (uint8_t)v;
    apply_mode_wave();
    Serial.printf("OK %s=0x%02X (%s)\n", gd ? "gdfr" : "fr", (unsigned)v,
                  wave_fr_name((uint8_t)v));
    return;
  }
  if (!strncmp(s, "pol ", 4)) {
    g_wp.invert = atoi(s + 4) != 0;
    apply_mode_wave();
    Serial.printf("OK pol=%u\n", (unsigned)g_wp.invert);
    return;
  }
  if (!strncmp(s, "bb ", 3)) {
    g_wp.touch_bb = atoi(s + 3) != 0;
    apply_mode_wave();
    Serial.printf("OK touch_bb=%u\n", (unsigned)g_wp.touch_bb);
    return;
  }
  if (!strncmp(s, "dual ", 5)) {
    g_dual = atoi(s + 5) != 0;
    apply_mode_wave();
    Serial.printf("OK dual=%u\n", (unsigned)g_dual);
    return;
  }
  // Bitmask, not a flag: 1=EOPT 2=VGH 4=source 8=VCOM, 15=all. `volt 8` writes
  // VCOM and nothing else, which is the only way to test VCOM in isolation.
  if (!strncmp(s, "volt ", 5)) {
    long v;
    if (!parse_val(s + 5, &v) || v < 0 || v > 15) {
      Serial.println("ERR volt MASK 0..15 (1=EOPT 2=VGH 4=src 8=VCOM)");
      return;
    }
    g_wp.volt_mask = (uint8_t)v;
    apply_mode_wave();
    Serial.printf("OK volt_mask=%u (EOPT=%u VGH=%u SRC=%u VCOM=%u)\n", (unsigned)v,
                  (unsigned)!!(v & 1), (unsigned)!!(v & 2), (unsigned)!!(v & 4),
                  (unsigned)!!(v & 8));
    return;
  }
  // How many frames a changed pixel keeps being re-driven. 0 = plain true-diff
  // (the old behaviour, leaves the ~50% grey trail).
  if (!strcmp(s, "wintest")) { run_wintest(); return; }
  // `mir 1 1` .. `mir 0 0` — four combinations, testable live without a reflash.
  if (!strncmp(s, "mir", 3) && (s[3] == ' ' || !s[3])) {
    int y = -1, x = -1;
    if (s[3] && sscanf(s + 4, "%d %d", &y, &x) == 2)
      EPD_SetWinMirror(y != 0, x != 0);
    Serial.printf("MIR ymirror=%u xmirror=%u  (row = %s, slave col = %s)\n",
                  (unsigned)EPD_WinYMirror(), (unsigned)EPD_WinXMirror(),
                  EPD_WinYMirror() ? "271 - buffer" : "buffer",
                  EPD_WinXMirror() ? "99 - buffer" : "buffer - 50");
    return;
  }
  if (!strncmp(s, "winsync ", 8)) {
    int v = atoi(s + 8);
    if (v < 0 || v > 64) { Serial.println("ERR winsync 0..64 (0 = never)"); return; }
    g_win_sync = (uint8_t)v;
    g_win_sync_ctr = 0;
    Serial.printf("OK winsync=%d — full write every %d frames\n", v, v);
    return;
  }
  // Same sweep on demand — this is what an app should call at the start of a
  // round, a level, or any point where the screen must be genuinely blank.
  if (!strcmp(s, "clearbars")) {
    drive_white_clear(4);
    render_frame();
    epd_seed();
    apply_mode_wave();
    Serial.println("OK clearbars - panel driven white x4, then reseeded");
    return;
  }
  // Pure transport benchmark: 54400 bytes of burst, no register writes, no
  // waveform. Separates DMA throughput from everything wrapped around it.
  if (!strcmp(s, "spibench")) {
    EPD_GPIOInit();
    uint32_t t0 = micros();
    EPD_SpiBurstBegin();
    EPD_SpiBurstStrided(ImageBW, NULL, ALLSCREEN_BYTES, 1, 0);
    EPD_SpiBurstEnd();
    uint32_t half = micros() - t0;
    EPD_SpiBurstBegin();
    EPD_SpiBurstStrided(ImageBW, NULL, ALLSCREEN_BYTES, 1, 0);
    EPD_SpiBurstEnd();
    uint32_t both = micros() - t0;
    Serial.printf("SPIBENCH %u bytes in %lu us (%lu us for 2x) -> %lu KB/s; "
                  "wire at %luMHz should be %lu us\n",
                  (unsigned)ALLSCREEN_BYTES, (unsigned long)half, (unsigned long)both,
                  (unsigned long)((uint64_t)ALLSCREEN_BYTES * 2 * 1000 / (both ? both : 1)),
                  (unsigned long)(EPD_GetHwSpiHz() / 1000000UL),
                  (unsigned long)((uint64_t)ALLSCREEN_BYTES * 2 * 8 * 1000000ULL /
                                  EPD_GetHwSpiHz()));
    return;
  }
  if (!strcmp(s, "refresh")) {
    g_flush_frames = g_trail ? g_trail : 4;
    Serial.printf("OK refresh - %u full-panel drives, no GC flash\n",
                  (unsigned)g_flush_frames);
    return;
  }
  if (!strncmp(s, "bounds ", 7)) {
    EPD_SetWinBounds(atoi(s + 7) != 0);
    Serial.printf("OK bounds=%u (%s)\n", (unsigned)EPD_WinBounds(),
                  EPD_WinBounds() ? "min,max" : "start,end");
    return;
  }
  if (!strcmp(s, "win")) {
    Serial.printf("WIN mode=%u  %ux%u px (%u cols x %u rows)  last=%lu bytes of %lu full\n",
                  (unsigned)g_win_mode, (unsigned)g_win_cols * 8, (unsigned)g_win_rows,
                  (unsigned)g_win_cols, (unsigned)g_win_rows, (unsigned long)g_win_bytes,
                  (unsigned long)(2UL * PLANE_BYTES));
    return;
  }
  if (!strncmp(s, "win ", 4)) {
    int want = atoi(s + 4);
    if (want < 0 || want > 2) { Serial.println("ERR win 0=off 1=safe 2=tight"); return; }
    if (want != (int)g_win_mode) {
      g_win_mode = (uint8_t)want;
      if (g_win_mode) g_pending_white_clear = true;
      // No automatic fake crossing on enable: driving every pixel several
            // times over re-drives the STATIC content too, and at a short
            // waveform that leaves it greyer than leaving it alone. `refresh`
            // still triggers one by hand when it is actually wanted.
      // Settle every hot pixel across the whole panel before changing regime —
      // otherwise the ones left outside the first window flash forever.
      render_frame();
      epd_seed();
      apply_mode_wave();
    }
    Serial.printf("OK win=%u (resynced)\n", (unsigned)g_win_on);
    return;
  }
  // Fixed transfer size. Set it once for the largest thing that moves; the byte
  // count — and therefore the frame time — then never varies with content.
  if (!strncmp(s, "winsize ", 8)) {
    int w = 0, h = 0;
    if (sscanf(s + 8, "%d %d", &w, &h) != 2 || w < 8 || w > 800 || h < 8 || h > 272) {
      Serial.println("ERR winsize WIDTH_PX HEIGHT_PX (8..800 x 8..272)");
      return;
    }
    g_win_cols = (uint8_t)((w + 7) / 8);
    g_win_rows = (uint16_t)h;
    Serial.printf("OK winsize %u x %u px -> %lu bytes/frame (full = %lu)\n",
                  (unsigned)g_win_cols * 8, (unsigned)g_win_rows,
                  (unsigned long)(2UL * g_win_cols * g_win_rows),
                  (unsigned long)(2UL * PLANE_BYTES));
    return;
  }
  if (!strncmp(s, "trail ", 6)) {
    int v = atoi(s + 6);
    if (v < 0 || v > TRAIL_MAX) { Serial.printf("ERR trail 0..%d\n", TRAIL_MAX); return; }
    g_trail = (uint8_t)v;
    Serial.printf("OK trail=%d frames of re-drive per change\n", v);
    return;
  }
  // The only remaining way to add impulse without adding time: run the source
  // rails at their maximum. VSH1 15 -> 17 V and VSL -15 -> -17 V is ~13% more
  // drive per frame at an identical frame count, so every mode keeps its exact
  // busy time. Mask is VCOM|SRC — EOPT stays out of it, or it would cost +17 ms
  // and flash. Both values are within the datasheet's stated range.
  if (!strncmp(s, "boost ", 6)) {
    bool on = atoi(s + 6) != 0;
    g_wp.vsh1 = on ? 0x4B : 0x41;  // 17 V / 15 V
    g_wp.vsl = on ? 0x3A : 0x32;   // -17 V / -15 V
    g_wp.volt_mask = on ? (uint8_t)(WV_V_VCOM | WV_V_SRC) : (uint8_t)WV_V_VCOM;
    apply_mode_wave();
    Serial.printf("OK boost=%u  VSH1=0x%02X VSL=0x%02X (busy unchanged)\n", (unsigned)on,
                  (unsigned)g_wp.vsh1, (unsigned)g_wp.vsl);
    return;
  }
  if (!strncmp(s, "balance ", 8)) {
    g_wp.balance_idle = atoi(s + 8) != 0;
    apply_mode_wave();
    Serial.printf("OK balance_idle=%u\n", (unsigned)g_wp.balance_idle);
    return;
  }
  if (!strncmp(s, "probe", 5)) {
    int b = -1, l = -1, t = -1;
    if (sscanf(s + 5, "%d %d %d", &b, &l, &t) >= 1) {
      if (b < 0 || b > 4) { Serial.println("ERR block 0..4 (0=C 1=WW 2=BW 3=WB 4=BB)"); return; }
      g_probe_block = (uint8_t)b;
      if (l >= 0 && l <= 3) g_probe_level = (uint8_t)l;
      if (t >= 1 && t <= 63) g_probe_tp = (uint8_t)t;
    }
    static const char *kBlk[5] = {"LUTC", "LUTWW", "LUTBW", "LUTWB", "LUTBB"};
    static const char *kLvl[4] = {"VSS", "VSH1", "VSL", "VSH2"};
    Serial.printf("PROBE %s at %s for %u frames — only pixels in that transition "
                  "class may move\n",
                  kBlk[g_probe_block], kLvl[g_probe_level], (unsigned)g_probe_tp);
    if (mode_info().wave != WV_PROBE) enter_mode(mode_with_wave(WV_PROBE), true);
    else apply_mode_wave();
    return;
  }
  if (!strncmp(s, "stepclear ", 10)) {
    g_step_clear = atoi(s + 10) != 0;
    Serial.printf("OK stepclear=%u\n", (unsigned)g_step_clear);
    return;
  }
  if (!strncmp(s, "stephold ", 9)) {
    g_step_hold = atoi(s + 9) != 0;
    Serial.printf("OK stephold=%u\n", (unsigned)g_step_hold);
    return;
  }
  // `steps 14,12,10,8 12` — dwell on exactly the values you care about.
  if (!strncmp(s, "steps ", 6)) {
    uint8_t vals[32];
    uint8_t n = 0;
    const char *p = s + 6;
    while (*p && n < sizeof(vals)) {
      while (*p == ',' || *p == ' ') {
        if (*p == ' ') break;
        p++;
      }
      if (*p == ' ' || !*p) break;
      int v = atoi(p);
      if (v < 1 || v > 63) { Serial.println("ERR steps values are 1..63"); return; }
      vals[n++] = (uint8_t)v;
      while (*p && *p != ',' && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    int secs = *p ? atoi(p) : 15;
    if (secs < 1) secs = 1;
    if (!n) { Serial.println("ERR steps A,B,C [SECONDS]"); return; }
    Serial.printf("STEPS %u values, %d s each, GC clear + reseed before each\n",
                  (unsigned)n, secs);
    run_steps(vals, n, (uint32_t)secs * 1000UL, g_step_clear);
    return;
  }
  if (!strncmp(s, "sweep ", 6)) {
    int a = 0, b = 0, n = 3;
    int got = sscanf(s + 6, "%d %d %d", &a, &b, &n);
    if (got < 2 || a < 1 || b < 1 || a > 63 || b > 63 || b > a) {
      Serial.println("ERR sweep FROM TO [FRAMES], 63 >= FROM >= TO >= 1");
      return;
    }
    run_sweep(a, b, n);
    return;
  }
  if (!strncmp(s, "mode ", 5)) {
    int v = atoi(s + 5);
    if (v >= 1 && v <= kModeCount) enter_mode((uint8_t)(v - 1), true);
    else Serial.println("ERR mode range");
    return;
  }
  if (!strncmp(s, "speed ", 6)) {
    int v = atoi(s + 6);
    // Percent of one shape-length per frame. 100 is the non-overlapping case;
    // below 100 successive positions overlap, above 100 they leave a gap.
    if (v >= 10 && v <= 400) {
      g_speed = v;
      Serial.printf("OK speed=%d%% -> %d px/frame (x=%d y=%d), shape depth %d px\n",
                    g_speed, step_mag(), step_x(0), step_y(0), g_radius);
    } else {
      Serial.println("ERR speed 10..400 (percent of one shape-length per frame)");
    }
    return;
  }
  // `spi bitbang` | `spi hw` | `spi hw 10`  — switch transport live, then
  // re-seed so the planes are known-good on the new path. Instant fallback.
  if (!strncmp(s, "spi", 3)) {
    const char *a = s + 3;
    while (*a == ' ') a++;
    if (!strncmp(a, "bitbang", 7)) {
      EPD_SetSpiPath(EPD_SPI_BITBANG);
    } else if (!strncmp(a, "hw", 2)) {
      int mhz = atoi(a + 2);
      // NOT clamped. 20 MHz is the SSD1683 datasheet maximum but this is a bench,
      // and refusing to run the experiment is worse than running it and looking at
      // the result. 40 MHz is in spec for nothing and works fine on the 4.26"
      // sibling; whether it works here is a question for the glass.
      //
      // The ESP32 divides 80 MHz by an integer, so the only reachable settings are
      // 20 (80/4), 26.7 (80/3) and 40 (80/2) — `spi hw 24` through `spi hw 32` all
      // resolve to 26.7.
      //
      // If a clock change ever wedges the controller (transfers complete but it
      // stops acting on commands, busy_ms = 0 the only symptom), that is the
      // mid-traffic device rebuild, not the clock. This path re-inits the panel
      // immediately afterwards, which is what makes it survivable.
      if (mhz >= 1 && mhz <= 40) EPD_SetHwSpiHz((uint32_t)mhz * 1000000UL);
      else if (mhz) Serial.println("ERR spi hw 1..40");
      EPD_SetSpiPath(EPD_SPI_HW);
    } else {
      Serial.printf("SPI path=%s\n", EPD_SpiPathName());
      return;
    }
    epd_ready();
    render_frame_static();
    epd_seed();
    apply_mode_wave();  // epd_ready + epd_seed both drop the custom LUT
    Serial.printf("OK spi path=%s\n", EPD_SpiPathName());
    return;
  }
  if (!strncmp(s, "flush", 5)) {
    int v = (s[5] == ' ') ? atoi(s + 6) : 3;
    if (v < 1) v = 1;
    if (v > 10) v = 10;
    Serial.printf("deghost x%d — this WILL flash, it is the point\n", v);
    epd_deghost((uint8_t)v);
    epd_full_clear();
    render_frame_static();
    epd_seed();
    apply_mode_wave();  // deghost + clear + seed all run the OTP path
    Serial.println("OK flush");
    return;
  }
  if (!strncmp(s, "clean ", 6)) {
    int v = atoi(s + 6);
    if (v >= 0 && v <= 500) { g_clean_every = (uint16_t)v; Serial.printf("OK clean_every=%d\n", v); }
    else Serial.println("ERR clean 0..500 (0=off)");
    return;
  }
  if (!strncmp(s, "passes ", 7)) {
    int v = atoi(s + 7);
    if (v >= 1 && v <= 4) { g_clean_passes = (uint8_t)v; Serial.printf("OK clean_passes=%d\n", v); }
    else Serial.println("ERR passes 1..4");
    return;
  }
  if (!strncmp(s, "radius ", 7)) {
    int v = atoi(s + 7);
    if (v >= 6 && v <= 60) {
      g_radius = v;
      clamp_balls();
      Serial.printf("OK radius=%d (box %d)\n", g_radius, 2 * g_radius);
    } else {
      Serial.println("ERR radius 6..60");
    }
    return;
  }
  // `box N` — sprite box in PIXELS, which is the number worth sweeping when the
  // question is how render cost scales with area. Step size follows automatically
  // (shape_depth() is the box), so the non-overlap property holds at every size.
  if (!strncmp(s, "box ", 4)) {
    int v = atoi(s + 4);
    if (v >= 12 && v <= 120) {
      g_radius = v / 2;
      clamp_balls();
      Serial.printf("OK box=%d px (radius=%d, step=%d px/frame)\n", 2 * g_radius,
                    g_radius, step_mag());
    } else {
      Serial.println("ERR box 12..120 (even numbers; it is 2 x radius)");
    }
    return;
  }
  Serial.printf("ERR unknown '%s' (try help)\n", s);
}

static void poll_serial(void) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      g_ser_line[g_ser_len] = 0;
      handle_cmd(g_ser_line);
      g_ser_len = 0;
      continue;
    }
    if (g_ser_len < sizeof(g_ser_line) - 1) g_ser_line[g_ser_len++] = c;
  }
}

// MODE n == drive n. Changing it rebuilds the LUT and (by default) GC-clears and
// reseeds first, so what you judge is that waveform alone.
// Four representative points rather than every value — 10 is the quality
// ceiling, 5 the midpoint, 2 and 1 the fast end. Wrapping, so 10 and 1 are one
// click apart and can be compared back to back. `drive N` still reaches any
// value 1..63 over serial.
// Every mode from 1 to 10, one per press, wrapping at both ends. Was {1,2,5,10}
// — four coarse stops — which skipped exactly the 3..9 range where the trade
// between speed and blackness actually lives on this panel.
static const uint8_t kWheel[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
#define WHEEL_N ((int)(sizeof(kWheel) / sizeof(kWheel[0])))

static void adjust_drive(int delta) {
  if (mode_info().wave != WV_CUSTOM) enter_mode(mode_with_wave(WV_CUSTOM), false);

  int idx = 0;  // land on the nearest wheel stop if drive was set over serial
  for (int i = 0; i < WHEEL_N; i++)
    if (kWheel[i] == g_wp.drive_tp) { idx = i; break; }
  idx = (idx + delta + WHEEL_N) % WHEEL_N;

  int v = kWheel[idx];
  g_wp.drive_tp = (uint8_t)v;
  apply_mode_wave();
  if (g_step_clear) {
    // Same order as the `drive` command: clear twice, normalise, then run.
    bool was_running = g_running;
    epd_full_clear();
    epd_full_clear();
    apply_mode_wave();  // the clears run the vendor OTP path and drop the custom LUT
    bar_sweep();
    g_act = ACT_NONE;   // clear transients can queue a phantom STOP
    g_running = was_running;
  }
  Serial.printf("MODE %u  drive=%u  busy=%lu ms  frames=%u\n", (unsigned)v, (unsigned)v,
                (unsigned long)predicted_busy_ms(), (unsigned)wave_frames(g_ws));
}

// The buttons can no longer reach the other bench modes. Landing in PROBE_CF or
// GD26_CF mid-comparison silently swaps the waveform out from under you, and
// that is indistinguishable from the waveform itself misbehaving. Those modes
// are still reachable over serial with `mode N` when they are actually wanted.
static void drain_actions(void) {
  uint8_t a = g_act;
  if (a == ACT_NONE) return;
  g_act = ACT_NONE;
  switch (a) {
    case ACT_RUN:
      if (mode_info().manual) { g_step_once = true; Serial.println("BTN step"); }
      else { g_running = true; Serial.println("BTN run"); }
      break;
    case ACT_STOP:
      g_running = false;
      if (EPD_AnalogIsOn()) analog_power(false);
      Serial.println("BTN stop");
      break;
    case ACT_MODE_DN: adjust_drive(-1); break;  // fewer frames, faster, lighter
    case ACT_MODE_UP: adjust_drive(+1); break;  // more frames, slower, blacker
    case ACT_WIN:
      g_win_mode = (uint8_t)((g_win_mode + 1) % 2);  // off <-> row band only
      if (g_win_mode) g_pending_white_clear = true;
      // No automatic fake crossing on enable: driving every pixel several
            // times over re-drives the STATIC content too, and at a short
            // waveform that leaves it greyer than leaving it alone. `refresh`
            // still triggers one by hand when it is actually wanted.
      g_label_settle = LABEL_SETTLE_FRAMES;
      g_settle_delay = 16;
      // Settle every hot pixel across the whole panel before changing regime,
      // or the ones left outside the first window re-drive forever.
      render_frame();
      epd_seed();
      apply_mode_wave();
      Serial.printf("BTN win=%u\n", (unsigned)g_win_on);
      break;
  }
}

// ---- arduino ----------------------------------------------------------------
void setup(void) {
  // 64 KB of UART receive buffer, and it MUST be set before begin().
  //
  // The default is 256 bytes, which is 1.28 ms of data at the 2 Mbaud the video
  // stream uses. Any time the receive task is descheduled by the display work the
  // FIFO overruns and bytes are dropped SILENTLY. In a delta-coded stream that is
  // unrecoverable: the payload alignment shifts, and because the framebuffer is
  // column-major a run of contiguous bytes is one 8-px vertical stripe — so dropped
  // bytes show up as vertical white lines appearing after a few seconds and never
  // healing. Cost is RAM only.
  size_t rxbuf = Serial.setRxBufferSize(65536);
  Serial.begin(115200);
  delay(120);

  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_RUN, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);

  panel_on();
  // Hardware SPI at 20 MHz, the datasheet maximum. Confirmed on glass
  // 2026-08-07. Takes the transfer from 101 ms to 40 ms per frame; `spi bitbang`
  // switches back live if it is ever suspect.
  //
  // SET THE CLOCK HERE, at boot, and prefer not to change it at runtime. The
  // SSD1677 sibling wedges its controller when the SPI device is rebuilt while
  // the panel is live, and `spi hw N` is exactly that path — it only gets away
  // with it here because it re-inits the panel immediately afterwards. This is
  // the value that matters.
  //
  // 40 MHz is reachable (the ESP32 divides 80 MHz by an integer: 20, 26.7, 40)
  // and NOT worth taking on this panel: the frame carries 27200 bytes, so the
  // wire is only ~11 ms of a ~130 ms frame, and ~17 ms of per-frame cost is fixed
  // driver overhead that does not scale with the clock at all. Measured 10/20/40
  // MHz all leave that same ~17 ms. The 4.26" panel gains real time from 40 MHz
  // only because it ships 96000 bytes a frame.
  EPD_SetHwSpiHz(40000000);
  EPD_SetSpiPath(EPD_SPI_HW);

  epd_ready();

  wave_defaults(&g_wp);

  // Change-mask ring lives in PSRAM — 8 x 27 KB is more than internal RAM can
  // spare, and the only work done on it is byte OR/XOR, so the slower bus does
  // not matter next to 86 ms of SPI.
  for (uint8_t k = 0; k < TRAIL_MAX; k++) {
    g_hist[k] = (uint8_t *)heap_caps_calloc(PLANE_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!g_hist[k]) g_trail = 0;
  }
  if (!g_trail) Serial.println("WARN no PSRAM for trail chasing — falling back to true-diff");

  // Source rails at their datasheet maximum, on by default.
  //
  // VSH1 15 -> 17 V and VSL -15 -> -17 V is ~13% more drive per frame at an
  // IDENTICAL frame count, so every mode keeps its exact busy time — voltage sets
  // how hard a pixel is driven, not how long. This was carried in the docs as
  // "the free lever, untested" until the SSD1677 sibling made it its default and
  // confirmed visibly darker blacks on glass, with the benefit growing as the mode
  // gets faster. That is precisely the regime this panel runs in, so it is now the
  // default here too. `boost 0` reverts to the POR ±15 V at runtime.
  //
  // Kept OUT of wave_defaults() on purpose: leaving that at the power-on values
  // is what makes "custom LUT with volt_mask off" a true phases-only experiment.
  // Mask is VCOM|SRC — EOPT must stay out, or it costs +17 ms and a visible flash.
  g_wp.vsh1 = 0x4B;
  g_wp.vsl = 0x3A;
  // No settle frame. It exists to let the pixel stack relax before the analog block
  // powers down, and on the rails-up 0x0C path the rails never drop between frames,
  // so there is nothing to relax for. Halves busy at mode 1: 2 LUT frames -> 1,
  // 16 ms -> 8 ms, 23.9 -> 30.2 fps. `settle 1` puts it back.
  g_wp.settle_tp = 0;
  g_wp.volt_mask = (uint8_t)(WV_V_VCOM | WV_V_SRC);

  reset_balls();
  mz_reset();  // carve the maze and place Pac-Man + ghosts

  Serial.println();
  Serial.println("=== CrowPanel 5.79 circle / plane-diff / waveform bench ===");
  Serial.printf("uart rx buffer granted: %u bytes\n", (unsigned)rxbuf);
  Serial.println("bit-bang SPI. Custom LUT is register-only (0x32/0xB2) — never OTP burn.");
  Serial.println("Start with `calib`: it measures the frame period without touching the glass.");
  print_modes();
  print_help();

  // Boot de-ghost then clear. Costs ~2 s once, and is the only thing that
  // undoes charge accumulated by earlier fixed-position drives.
  epd_deghost(2);
  epd_full_clear();
  // Straight into the custom waveform at MODE 8, already running. There is
  // nothing to set up and no run to start or lose.
  g_wp.drive_tp = 10;
  enter_mode(mode_with_wave(WV_CUSTOM), true);
  // Second clear, then normalise the background: one bar pass drives every pixel
  // through the same incomplete transition, so Pac-Man's wake afterwards matches its
  // surroundings and stops reading as a trail.
  epd_full_clear();
  apply_mode_wave();  // the clear runs the vendor OTP path and drops the custom LUT
  bar_sweep();
  g_running = true;

  xTaskCreatePinnedToCore(btn_task, "btn", 2048, nullptr, 2, nullptr, 0);

  Serial.println("READY — press RIGHT to run");
}

void loop(void) {
  poll_serial();
  drain_actions();

  if (mode_info().manual) {
    if (g_step_once) {
      g_step_once = false;
      halfsplit_step();
    }
    g_running = false;
    delay(5);
    return;
  }

  if (!g_running && !g_step_once) {
    delay(5);
    return;
  }
  g_step_once = false;

  advance_motion();
  render_frame();
  show_frame();
  g_frame++;
  fps_tick();

  // Entered windowed mode: sweep the panel white once so it starts from a
  // genuinely blank screen, then carry on.
  if (g_pending_white_clear) {
    g_pending_white_clear = false;
    Serial.println("WIN: driving panel white before windowed frames...");
    drive_white_clear(4);
    render_frame();
    epd_seed();
    apply_mode_wave();  // epd_seed fires 0xDC and drops the custom LUT
    Serial.println("WIN: clear done");
  }
}
