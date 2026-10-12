#ifndef _STREAM_WIRE_H_
#define _STREAM_WIRE_H_

// USB frame wire used by inkvideo (any panel).
//
// wire=1 (legacy): ACK, occupancy[PLANE/8], ACK, lit[popcount]
// wire=2 (current): one ACK, then
//   u8  flags     bit0 = occupancy is RLE
//   u16 n_lit     little-endian
//   occupancy     raw PLANE/8, or RLE until that many bytes decoded
//   lit[n_lit]
//
// RLE: 0x00, count(1..255) → `count` zero bytes; any other byte is literal.
// 0x18 as the flags byte is CAN (host abort). Do not use 0x18 as a flags value.
//
// ONE cursor for the whole stream. The old reader gave each section its own
// local 128-byte buffer: the RLE decoder pulled up to 128 bytes off the wire,
// decoded the ~34 it needed, and dropped the rest on return. For a small frame
// the dropped remainder IS the lit payload — it arrives in the same USB packet
// — so the next read then waited out its full 4 s for bytes that had already
// been thrown away, and the frame aborted. Small deltas are the steady state of
// video, so this fired on essentially every frame after the first.
//
// Everything now reads through StreamWire: buffered bytes are drained before
// the wire is touched again, and the cursor survives across frames so a
// pipelined host cannot lose the head of frame N+1 either.

#include <string.h>

#define STREAM_ACK 0x06
#define STREAM_CAN 0x18
#define STREAM_WIRE 2
#define STREAM_STALL_MS 4000

typedef struct {
  uint8_t buf[256];
  int n;
  int i;
  uint32_t tw;
} StreamWire;

static StreamWire g_sw;

// Call at stream/frame entry, not per frame: leftovers between frames are real
// data, and dropping them is the bug above.
static void stream_wire_reset(void) {
  g_sw.n = 0;
  g_sw.i = 0;
  g_sw.tw = millis();
}

static bool sw_stalled(void) { return (millis() - g_sw.tw) > STREAM_STALL_MS; }

// Ensure at least one buffered byte. False only on a real stall.
static bool sw_fill(void) {
  while (g_sw.i >= g_sw.n) {
    int n = Serial.read(g_sw.buf, (int)sizeof(g_sw.buf));
    if (n > 0) {
      g_sw.n = n;
      g_sw.i = 0;
      g_sw.tw = millis();
      return true;
    }
    if (sw_stalled()) return false;
    delay(1);
  }
  return true;
}

static bool sw_byte(uint8_t *out) {
  if (!sw_fill()) return false;
  *out = g_sw.buf[g_sw.i++];
  return true;
}

static bool sw_read(uint8_t *dst, uint32_t total) {
  uint32_t o = 0;
  if (g_sw.i < g_sw.n) {
    uint32_t avail = (uint32_t)(g_sw.n - g_sw.i);
    uint32_t take = total < avail ? total : avail;
    memcpy(dst, g_sw.buf + g_sw.i, take);
    g_sw.i += (int)take;
    o = take;
  }
  // Bulk goes straight into dst — a plane can be 48000 bytes and does not want
  // to be copied twice.
  while (o < total) {
    int n = Serial.read(dst + o, (int)(total - o));
    if (n > 0) {
      o += (uint32_t)n;
      g_sw.tw = millis();
      continue;
    }
    if (sw_stalled()) return false;
    delay(1);
  }
  return true;
}

static bool sw_read_rle(uint8_t *dst, uint32_t out_len) {
  uint32_t o = 0;
  while (o < out_len) {
    uint8_t b;
    if (!sw_byte(&b)) return false;
    if (b != 0x00) {
      dst[o++] = b;
      continue;
    }
    uint8_t z;
    if (!sw_byte(&z)) return false;
    uint32_t run = z ? (uint32_t)z : 1u;
    if (o + run > out_len) return false;
    memset(dst + o, 0, run);
    o += run;
  }
  return true;
}

static bool stream_recv_wire2(uint8_t *dst, const uint8_t *base, uint8_t *bmp,
                              uint8_t *lit) {
  Serial.write((uint8_t)STREAM_ACK);
  g_sw.tw = millis();  // the stall window is per frame, the cursor is not
  // The flags byte alone first. The host's Stop is ONE CAN byte, and reading
  // all three header bytes before looking at it sat out the full 4 s stall
  // for the two that never come — Stop took ~4 s on every panel.
  uint8_t hdr[3];
  if (!sw_byte(&hdr[0])) return false;
  if (hdr[0] == STREAM_CAN) return false;
  if (!sw_read(hdr + 1, 2)) return false;
  const uint8_t flags = hdr[0];
  const uint16_t n_lit = (uint16_t)hdr[1] | ((uint16_t)hdr[2] << 8);
  if (n_lit > EPD_PLANE_BYTES) return false;
  const uint32_t occ = EPD_PLANE_BYTES / 8u;
  if (flags & 1u) {
    if (!sw_read_rle(bmp, occ)) return false;
  } else {
    if (!sw_read(bmp, occ)) return false;
  }
  if (n_lit && !sw_read(lit, n_lit)) return false;

  memcpy(dst, base, EPD_PLANE_BYTES);
  uint32_t k = 0;
  for (uint32_t i = 0; i < occ; i++) {
    uint8_t m = bmp[i];
    if (!m) continue;
    uint32_t at = i << 3;
    if (m & 0x80) dst[at + 0] = lit[k++];
    if (m & 0x40) dst[at + 1] = lit[k++];
    if (m & 0x20) dst[at + 2] = lit[k++];
    if (m & 0x10) dst[at + 3] = lit[k++];
    if (m & 0x08) dst[at + 4] = lit[k++];
    if (m & 0x04) dst[at + 5] = lit[k++];
    if (m & 0x02) dst[at + 6] = lit[k++];
    if (m & 0x01) dst[at + 7] = lit[k++];
  }
  return k == n_lit;
}

#endif
