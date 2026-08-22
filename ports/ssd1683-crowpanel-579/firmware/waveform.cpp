#include "waveform.h"

#include "EPD_Init.h"
#include "spi.h"

#define BLOCK_STRIDE 42
#define GROUP_STRIDE 7

static inline uint8_t phase(uint8_t vs, uint8_t tp) {
  if (tp > 63) tp = 63;
  return (uint8_t)(((vs & 3u) << 6) | tp);
}

static inline uint8_t *group_at(uint8_t *ws, uint8_t block, uint8_t grp) {
  return ws + (uint32_t)block * BLOCK_STRIDE + (uint32_t)grp * GROUP_STRIDE;
}

static void tail_volts(uint8_t *ws, const WaveParams *p) {
  ws[227] = p->eopt;
  ws[228] = p->vgh;
  ws[229] = p->vsh1;
  ws[230] = p->vsh2;
  ws[231] = p->vsl;
  ws[232] = p->vcom;
}

void wave_defaults(WaveParams *p) {
  p->fr = 0x05;  // 125 Hz — the fastest legal frame rate on this part
  // No opposite-polarity kick by default. It helps the pigment let go on a
  // one-shot drive, but with trail chasing every re-driven pixel runs the WHOLE
  // LUT again — so the kick replays as a dark flash on pixels that were already
  // clean, and with a multi-frame history those flashes arrive staggered and
  // read as a pulse travelling behind the moving content. Repeated drives give
  // the release the kick was there to provide, so it costs nothing to drop and
  // it makes every mode two frames (16 ms) faster.
  p->reset_tp = 0;
  // MODE 10: judged on glass 2026-08-07 — "very dark black levels, good enough
  // for any normal task". 105 ms versus the vendor path's 336 ms.
  p->drive_tp = 10;
  p->settle_tp = 1;
  p->rp = 1;
  p->sr_ab = 1;
  p->invert = false;
  p->touch_bb = false;
  p->balance_idle = false;
  // VCOM is written by default, and alone. Measured on glass 2026-08-07: leaving
  // the factory VCOM in place makes the whole panel creep darker every update
  // (undriven pixels sit at VSS against a negative common), and forcing it to
  // -3V makes that dramatically worse. -0.2V removes it outright, for free.
  // The mask keeps EOPT out of it — EOPT 0x22 costs +17 ms and a visible flash.
  p->volt_mask = WV_V_VCOM;
  // Power-on defaults from the datasheet — writing these changes nothing, so a
  // custom LUT with write_volt off and these values on is a true phases-only test.
  p->vsh1 = 0x41;  // 15 V
  p->vsh2 = 0xA8;  // 5 V
  p->vsl = 0x32;   // -15 V
  // 0x08 = -0.2 V, the least-biased LEGAL setting. The register only defines
  // 0x08..0x78 (-0.2 V .. -3 V); 0x00 is "NA" and writing it is undefined
  // behaviour, which is exactly what an earlier "VCOM = 0" attempt did.
  p->vcom = 0x08;
  p->vgh = 0x17;   // 20 V
  p->eopt = 0x22;  // 2 discharge scan frames
}

void wave_build(uint8_t *ws233, const WaveParams *p) {
  memset(ws233, 0, WAVE_BYTES);

  // LUTBW ends the pixel WHITE, LUTWB ends it BLACK. Which rail does which is
  // panel-dependent, so `invert` flips the pair — the PROBE mode settles it.
  uint8_t to_white = p->invert ? VS_VSL : VS_VSH1;
  uint8_t to_black = p->invert ? VS_VSH1 : VS_VSL;

  uint8_t rp = p->rp ? p->rp : 1;
  uint8_t sr = p->sr_ab ? p->sr_ab : 1;
  uint8_t drive = p->drive_tp ? p->drive_tp : 1;

  // Net impulse is (drive - reset). The reset phase pushes the OPPOSITE way, so
  // once it reaches the drive length the pixel lands mid-grey, and once it
  // exceeds it the pixel is driven backwards and the image comes out INVERTED.
  // Clamp it: the kick is only ever worth having if the drive can out-pull it.
  uint8_t reset = p->reset_tp;
  if (reset >= drive) reset = (drive > 1) ? (uint8_t)(drive - 1) : 0;

  struct {
    uint8_t block, target, kick;
  } lut[2] = {
      {WB_LUTBW, to_white, to_black},
      {WB_LUTWB, to_black, to_white},
  };

  for (uint8_t i = 0; i < 2; i++) {
    uint8_t *g = group_at(ws233, lut[i].block, 0);
    g[0] = rp;
    g[1] = reset ? phase(lut[i].kick, reset) : 0;
    g[2] = phase(lut[i].target, drive);
    g[3] = p->settle_tp ? phase(VS_VSS, p->settle_tp) : 0;
    g[4] = 0;
    g[5] = sr;
    g[6] = p->settle_tp ? 1 : 0;
  }

  // Charge-balanced filler for the pixels that are NOT changing. Same span as
  // the driven blocks so the timeline still lines up, half at +VSH1 and half at
  // -VSL so the net charge is zero — free in time, since every LUT block runs
  // concurrently and the waveform is only as long as its longest block.
  if (p->balance_idle) {
    uint8_t span = (uint8_t)(reset + drive);
    uint8_t h = (uint8_t)(span / 2);
    uint8_t rest = (uint8_t)(p->settle_tp + (span - 2 * h));
    for (uint8_t b = 0; b < 2; b++) {
      uint8_t *g = group_at(ws233, b ? WB_LUTBB : WB_LUTWW, 0);
      g[0] = rp;
      g[1] = h ? phase(VS_VSH1, h) : 0;
      g[2] = h ? phase(VS_VSL, h) : 0;
      g[3] = rest ? phase(VS_VSS, rest) : 0;
      g[4] = 0;
      g[5] = sr;
      g[6] = rest ? 1 : 0;
    }
  }

  // Good Display gives already-black pixels a single-frame top-up so they do
  // not fade over a long run of partials. Costs 1 frame, so it is optional.
  if (p->touch_bb) {
    uint8_t *g = group_at(ws233, WB_LUTBB, 0);
    g[0] = 1;
    g[1] = phase(to_black, 1);
    g[5] = 1;
  }

  ws233[224] = p->fr;
  tail_volts(ws233, p);
}

void wave_build_null(uint8_t *ws233, uint8_t tp, uint8_t fr) {
  memset(ws233, 0, WAVE_BYTES);
  if (!tp) tp = 1;
  // Every block ticks the same number of frames at VS = VSS: full timing cost,
  // zero field across the pixel, so the image on the glass cannot change.
  for (uint8_t b = 0; b < 5; b++) {
    uint8_t *g = group_at(ws233, b, 0);
    g[0] = 1;
    g[1] = phase(VS_VSS, tp);
    g[5] = 1;
  }
  ws233[224] = fr;
  // Leave the voltage trailer at 0 — wave_load is called with write_volt=false
  // for the null probe, so these bytes are never sent.
}

void wave_build_probe(uint8_t *ws233, uint8_t block, uint8_t level, uint8_t tp,
                      uint8_t fr, const WaveParams *volts) {
  memset(ws233, 0, WAVE_BYTES);
  if (block > 4) block = 4;
  if (!tp) tp = 1;
  uint8_t *g = group_at(ws233, block, 0);
  g[0] = 1;
  g[1] = phase(level, tp);
  g[5] = 1;
  ws233[224] = fr;
  if (volts) tail_volts(ws233, volts);
}

// Good Display / typewriter partial table for this panel family, byte-for-byte.
// Decoded: LUTWW 24+1 frames at VSS, LUTBW 24 frames VSH1 + 1, LUTWB 24 frames
// VSL + 1, LUTBB 24 VSS + 1 VSH1; group 1 adds a single frame each. 26 frames.
static const uint8_t kGdPart233[WAVE_BYTES] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x18, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x58, 0x41, 0x00, 0x00, 0x01, 0x00, 0x01, 0x41, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x98, 0x81, 0x00, 0x00, 0x01, 0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x18, 0x41, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x06, 0x17, 0x41, 0xA8, 0x32, 0x00,
};

void wave_build_gd(uint8_t *ws233, uint8_t fr) {
  memcpy(ws233, kGdPart233, WAVE_BYTES);
  ws233[224] = fr;
}

uint16_t wave_frames(const uint8_t *ws233) {
  uint16_t worst = 0;
  for (uint8_t b = 0; b < 5; b++) {
    uint32_t total = 0;
    for (uint8_t gi = 0; gi < 6; gi++) {
      const uint8_t *g = ws233 + (uint32_t)b * BLOCK_STRIDE + (uint32_t)gi * GROUP_STRIDE;
      uint16_t ab = (uint16_t)((g[1] & 0x3F) + (g[2] & 0x3F)) * g[5];
      uint16_t cd = (uint16_t)((g[3] & 0x3F) + (g[4] & 0x3F)) * g[6];
      total += (uint32_t)g[0] * (ab + cd);
    }
    if (total > worst) worst = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
  }
  return worst;
}

void wave_load(const uint8_t *ws233, bool dual, uint8_t volt_mask) {
  EPD_GPIOInit();

  // Clock on — the LUT registers are clocked logic.
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0x80);
  EPD_WR_REG(0x20);
  delay(2);

  EPD_WriteLutTarget(0x00, ws233);
  if (volt_mask) EPD_WriteLutVoltagesMaskTarget(0x00, ws233, volt_mask);

  // The cascade slave keeps its own copy of everything. Writing the master only
  // leaves the two halves running different waveforms — that is what made the
  // earlier "GD phases only" attempt look broken across the seam.
  if (dual) {
    EPD_WriteLutTarget(0x80, ws233);
    if (volt_mask) EPD_WriteLutVoltagesMaskTarget(0x80, ws233, volt_mask);
  }
}

// MEASURED periods, not the datasheet's nominal ones. `calib` on this panel gives
// figures consistently ~1.4% above nominal (8112 us at FR 0x05, not 8000), so a
// prediction built on nominal reads a millisecond or two below the busy time the
// panel actually reports. Using the measured values makes the on-screen number
// agree with the measured one instead of quietly disagreeing.
uint32_t wave_fr_period_us(uint8_t fr) {
  switch (fr & 0x0F) {
    case 0x1: return 40556;  // 25 Hz nominal
    case 0x2: return 20306;  // 50
    case 0x3: return 13509;  // 75
    case 0x4: return 10151;  // 100
    case 0x5: return 8112;   // 125
    case 0x9: return 27017;  // 37.5
    case 0xA: return 16223;  // 62.5
    case 0xB: return 11607;  // 87.5
    case 0xC: return 9029;   // 112.5
    default: return 0;       // not a defined setting
  }
}

const char *wave_fr_name(uint8_t fr) {
  switch (fr & 0x0F) {
    case 0x1: return "25Hz";
    case 0x2: return "50Hz";
    case 0x3: return "75Hz";
    case 0x4: return "100Hz";
    case 0x5: return "125Hz";
    case 0x9: return "37.5Hz";
    case 0xA: return "62.5Hz";
    case 0xB: return "87.5Hz";
    case 0xC: return "112.5Hz";
    default: return "INVALID";
  }
}

bool wave_fr_valid(uint8_t fr) { return wave_fr_period_us(fr) != 0; }

void wave_dump(const uint8_t *ws233) {
  static const char *kName[5] = {"LUTC ", "LUTWW", "LUTBW", "LUTWB", "LUTBB"};
  for (uint8_t b = 0; b < 5; b++) {
    for (uint8_t gi = 0; gi < 6; gi++) {
      const uint8_t *g = ws233 + (uint32_t)b * BLOCK_STRIDE + (uint32_t)gi * GROUP_STRIDE;
      bool empty = true;
      for (uint8_t k = 0; k < 7; k++)
        if (g[k]) empty = false;
      if (empty) continue;
      Serial.printf("  %s g%u  RP=%u  A=%u/%u B=%u/%u C=%u/%u D=%u/%u  SRab=%u SRcd=%u\n",
                    kName[b], (unsigned)gi, (unsigned)g[0], (unsigned)(g[1] >> 6),
                    (unsigned)(g[1] & 0x3F), (unsigned)(g[2] >> 6), (unsigned)(g[2] & 0x3F),
                    (unsigned)(g[3] >> 6), (unsigned)(g[3] & 0x3F), (unsigned)(g[4] >> 6),
                    (unsigned)(g[4] & 0x3F), (unsigned)g[5], (unsigned)g[6]);
    }
  }
  Serial.printf("  FR=0x%02X (%s)  EOPT=0x%02X VGH=0x%02X VSH1=0x%02X VSH2=0x%02X "
                "VSL=0x%02X VCOM=0x%02X  frames=%u\n",
                (unsigned)ws233[224], wave_fr_name(ws233[224]), (unsigned)ws233[227],
                (unsigned)ws233[228], (unsigned)ws233[229], (unsigned)ws233[230],
                (unsigned)ws233[231], (unsigned)ws233[232], (unsigned)wave_frames(ws233));
}
