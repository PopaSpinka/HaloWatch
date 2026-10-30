// player.h — the "Music" screen: WearKit's player (lib/AmoledWear/wear_player.cpp — Horologist PlayerScreen, every
// number in docs/research/notes/player.md) ported to Halo one to one, playing real music (music.h).
//
//   * the big button — a "wave" of 10 lobes while playing, slowly turning (36° in 3 s), a circle on pause; around it
//     a ring — how much of the track has gone (gaps of 4° at its ends, round caps); pressing any of the three buttons
//     squeezes it into a pill;
//   * the row of buttons is a group (ButtonGroup): the pressed one grows by 12 dp, the neighbours give way;
//   * "pause" flows into "play" (167 ms), "previous" / "next" — the old icon rides off and the new one inflates
//     (vertices and timing from the Lottie files M3PlayPause / M3Next);
//   * a title too long for 311 px is a marquee (64 dp/s, a 4 s pause, 96 dp gap, soft edges); a new track — the old
//     title and artist slide out and fade, the new ones slide in (300 ms);
//   * the volume button opens the "+/−" screen (fade through), the level is an arc at the left edge (LevelIndicator);
//     the heart turns pink with a bounce.
//
// How it is drawn on Halo: plain parts are items (the side capsules, the small buttons, icons, arcs) — they report
// their own changes. The wave button with its ring and the texts (the marquee is clipped, its edges fade) are drawn
// by this file straight into the band (halo_row, halo_span, halo_blend), as WearKit drew them, through the screen's
// view (shift, scale, brightness); they report their changes themselves (pl_dmg: the area, when its state changed).
// The icons' animations are drawn once at start into masks, a mask per frame (64 × 64, 4 bits; 73 masks in PSRAM):
// in play they cost what a letter costs. The music keeps playing when the screen is closed.
// Included from HaloWatch.ino after music.h.

#define PL_PRIMARY 0xE9DDFF      // the big button
#define PL_PRIMARY_DIM 0xD0BCFF  // the side buttons
#define PL_ON_PRIMARY 0x210F48   // icons on buttons
#define PL_RING 0xF0EFFF         // the ring: played
#define PL_RING_TRACK 0x272430   // the ring: to go
#define PL_ON_SURFACE 0xF6EDFF   // text
#define PL_SMALL 0x3B393D        // the small buttons
#define PL_VOL_FILL 0xBAC3FF
#define PL_VOL_TRACK 0x332E3C
#define PL_VOL_BTN 0x4D3D76      // "+/−" on the volume screen
#define PL_LIKE 0xFF8BCB
#define PL_TITLE_Y 85.0f
#define PL_ARTIST_Y 123.0f
#define PL_TITLE_W 311.0f
#define PL_SIDE_H 108.0f
#define PL_SMALL_Y 367.0f
#define PL_RING_HW 4.0f

static int pl_cur, pl_prev;
static bool pl_playing, pl_active, pl_liked;
static float pl_len[MU_MAX], pl_tw[MU_MAX], pl_aw[MU_MAX], pl_elapsed;
static Press pl_grp[3], pl_center, pl_small[2], pl_vbtn[2];
static int pl_armed = -1, pl_last = 1, pl_small_armed = -1, pl_vol_armed = -1;
static HaloSpring pl_like_m, pl_pp = {1, 0, 1};  // pl_pp: 0 — "pause" shown (playing), 1 — "play"
static float pl_rot, pl_skip_t[2] = {9, 9}, pl_change_t = 9, pl_change_dir, pl_marquee_x, pl_marquee_wait = 4;
static int pl_vol = 6;
static float pl_vol_lvl = 0.6f, pl_vol_from = 0.6f, pl_vol_t = 9, pl_vol_hide_in, pl_vol_idle_in;
static HaloSpring pl_vol_show, pl_vol_screen;
static HaloSpringK K_EFFECT_FAST, K_EFFECT_SLOW, K_GROUP_PRESS, K_M3X_FAST;
static HaloEase EASE_PL_EMPH, EASE_PL_SKIP_IN, EASE_PL_STD;
static float pl_mid_t, pl_mid_a, pl_mid_i;
static HaloView pl_v;  // this frame's view of the screen (layout → draw)

// ---------- The wave button: a wave of 10 lobes, a circle or a pill (straight from wear_player.cpp) ----------
static float sh_c10 = 1, sh_s10;  // cos, sin 10φ — once a frame
struct ShapeK {
  float a, b;   // b = 0 — a circle of radius a
  float ia;     // 1/a
  float pw, ph; // the pill's halves
  float m;      // 0 — wave or circle, 1 — pill
};

static inline float pl_rsqrt(float x) {
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i = 0x5f3759df - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - 0.5f * x * y * y);
  return y * (1.5f - 0.5f * x * y * y);
}

// The wave's radius toward (dx, dy) and the slope correction (cos 10θ as the 10th power of the unit vector)
static inline void wave_r(const ShapeK &k, float dx, float dy, float *r, float *c) {
  const float ir = pl_rsqrt(fmaxf(dx * dx + dy * dy, 1e-4f)), ux = dx * ir, uy = dy * ir;
  const float x2 = ux * ux - uy * uy, y2 = 2 * ux * uy, x4 = x2 * x2 - y2 * y2, y4 = 2 * x2 * y2;
  const float x8 = x4 * x4 - y4 * y4, y8 = 2 * x4 * y4, x10 = x8 * x2 - y8 * y2, y10 = x8 * y2 + y8 * x2;
  const float C = x10 * sh_c10 + y10 * sh_s10, S = y10 * sh_c10 - x10 * sh_s10, g = 10 * k.b * S * k.ia;
  *r = k.a + k.b * C, *c = 1 - 0.5f * g * g;
}

static inline float pill_dist(const ShapeK &k, float dx, float dy) {
  const float r = fminf(k.pw, k.ph), qx = fabsf(dx) - (k.pw - r), qy = fabsf(dy) - (k.ph - r);
  const float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
  return halo_sqrt(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static float shape_dist(const ShapeK &k, float dx, float dy) {
  const float d = halo_sqrt(dx * dx + dy * dy);
  float db = d - k.a;
  if (k.b != 0) {
    float r, c;
    wave_r(k, dx, dy, &r, &c);
    db = (d - r) * c;
  }
  return k.m <= 0.001f ? db : db + (pill_dist(k, dx, dy) - db) * k.m;
}

struct Sector {
  float s0x, s0y, s1x, s1y;
  bool big, on;
};
static inline bool in_sector(const Sector &q, float dx, float dy) {
  const float cs = q.s0x * dy - q.s0y * dx, ce = dx * q.s1y - dy * q.s1x;
  return q.big ? (cs >= 0 || ce >= 0) : (cs >= 0 && ce >= 0);
}
static Sector sector_make(float a0, float a1) {
  Sector q;
  q.on = a1 > a0;
  q.s0x = sinf(a0), q.s0y = -cosf(a0), q.s1x = sinf(a1), q.s1y = -cosf(a1), q.big = a1 - a0 > HALO_PI;
  return q;
}
struct RingK {
  Sector fill, track;
  float cap[4][2];  // the round ends' centers (on screen)
  uint32_t c_fill, c_track;
  float hw;         // half the ring's thickness
};
struct SectorI {
  int32_t s0x, s0y, s1x, s1y;
  bool big, on;
};
static inline SectorI sector_i(const Sector &q) {
  return {(int32_t)(q.s0x * 1024), (int32_t)(q.s0y * 1024), (int32_t)(q.s1x * 1024), (int32_t)(q.s1y * 1024), q.big, q.on};
}
static inline bool in_sector_i(const SectorI &q, int32_t dx, int32_t dy) {
  const int32_t cs = q.s0x * dy - q.s0y * dx, ce = dx * q.s1y - dy * q.s1x;
  return q.big ? (cs >= 0 || ce >= 0) : (cs >= 0 && ce >= 0);
}
static uint32_t pl_mix_rgb(uint32_t a, uint32_t b, float t) {
  const int k = (int)(halo_clamp01(t) * 256);
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    const int ca = a >> sh & 255, cb = b >> sh & 255;
    out |= (uint32_t)(ca + (((cb - ca) * k) >> 8)) << sh;
  }
  return out;
}
static inline void pl_blend(uint16_t *p, uint32_t c, int a) {
  halo_blend(p, (c >> 19) & 31, (c >> 10) & 63, (c >> 3) & 31, a);
}

// A row without the pill: the distance to the center without a root, d ≈ (d² + a²)/2a; the wave's radius exactly at
// every 4th pixel, linearly between; all in 1/16 px. Fully covered — written straight
static void HALO_HOT shape_row_int(uint16_t *row, int pa, int pb, float cx, float dy, const ShapeK &k, bool wave,
                                   const RingK *ring, uint16_t c, uint32_t color, uint16_t cf16, uint16_t ct16, int y) {
  const int32_t aq = (int32_t)(k.a * 16 + 0.5f), A2 = aq * aq, K = (int32_t)(131072.0f * k.ia + 0.5f);
  const int32_t dyq = (int32_t)(dy * 16), dy2 = dyq * dyq, hwq = ring ? (int32_t)(ring->hw * 16) + 8 : 0;
  SectorI sf = {}, st = {};
  if (ring) sf = sector_i(ring->fill), st = sector_i(ring->track);
  int32_t dxq = (int32_t)((pa + 0.5f - cx) * 16);
  int32_t rq = aq << 2, drq = 0, cq = 256, dcq = 0;
  float r1 = 0, c1 = 1;
  const float fy = dyq * (1 / 16.0f);
  if (wave) wave_r(k, dxq * (1 / 16.0f), fy, &r1, &c1);
  const int32_t lim = ring ? hwq + 24 : 24;
  for (int x = pa; x <= pb; x++, dxq += 16) {
    if (((x - pa) & 3) == 0) {
      if (wave) {
        const float r0 = r1, c0 = c1;
        wave_r(k, dxq * (1 / 16.0f) + 4, fy, &r1, &c1);
        rq = (int32_t)(r0 * 64), drq = (int32_t)((r1 - r0) * 16), cq = (int32_t)(c0 * 256), dcq = (int32_t)((c1 - c0) * 64);
      }
      const int32_t xb = dxq + 48, d2a = dxq * dxq + dy2, d2b = xb * xb + dy2;
      const int32_t dda = ((((d2a + A2) >> 6) * K >> 16) - (rq >> 2)) * cq >> 8;
      const int32_t ddb = ((((d2b + A2) >> 6) * K >> 16) - ((rq + 3 * drq) >> 2)) * (cq + 3 * dcq) >> 8;
      const int xe = min(x + 3, pb);
      if (ring ? (dda > lim && ddb > lim) || (dda < -lim && ddb < -lim) : (dda > lim && ddb > lim)) {
        dxq += 16 * (xe - x), x = xe;
        continue;
      }
      if (!ring && dda < -lim && ddb < -lim) {
        halo_span(row, x, xe, c);
        dxq += 16 * (xe - x), x = xe;
        continue;
      }
    }
    const int32_t d2 = dxq * dxq + dy2, dq = ((d2 + A2) >> 6) * K >> 16;
    int32_t dd = (dq - (rq >> 2)) * cq >> 8;
    if (wave) rq += drq, cq += dcq;
    if (!ring) {
      const int32_t cov = 8 - dd;
      if (cov >= 16) row[x] = c;
      else if (cov > 0) pl_blend(&row[x], color, cov << 4);
      continue;
    }
    const int32_t cov = hwq - (dd < 0 ? -dd : dd);
    if (cov <= 0) continue;
    const bool in_f = sf.on && in_sector_i(sf, dxq, dyq), in_t = !in_f && st.on && in_sector_i(st, dxq, dyq);
    if (in_f || in_t) {
      if (cov >= 16) row[x] = in_f ? cf16 : ct16;
      else pl_blend(&row[x], in_f ? ring->c_fill : ring->c_track, cov << 4);
      continue;
    }
    float cf = 0, ct = 0;  // the gap between played and to go: only the round ends
    for (int e = 0; e < 4; e++) {
      const float ux = x + 0.5f - ring->cap[e][0], uy = y + 0.5f - ring->cap[e][1], q = ux * ux + uy * uy;
      if (q < (ring->hw + 1) * (ring->hw + 1)) {
        const float cc = ring->hw + 0.5f - halo_sqrt(q);
        if (e < 2 && ring->fill.on) cf = fmaxf(cf, cc);
        if (e >= 2 && ring->track.on) ct = fmaxf(ct, cc);
      }
    }
    if (cf > 0) pl_blend(&row[x], ring->c_fill, (int)(fminf(cf, 1) * 256));
    else if (ct > 0) pl_blend(&row[x], ring->c_track, (int)(fminf(ct, 1) * 256));
  }
}

// A row while the button is held (wave or circle flowing into a pill): floats; far from the edge — in jumps
static void HALO_HOT shape_row_float(uint16_t *row, int pa, int pb, float cx, float dy, int y, const ShapeK &k,
                                     const RingK *ring, uint16_t c, uint32_t color, uint16_t cf16, uint16_t ct16) {
  const float hw = ring ? ring->hw : 0, dy2 = dy * dy, a2 = k.a * k.a, inv2a = 0.5f * k.ia;
  const bool wave = k.b != 0;
  int wg = -1;
  float wr0 = k.a, wc0 = 1, wr1 = k.a, wc1 = 1;
  for (int x = pa; x <= pb;) {
    const float dx = x + 0.5f - cx, d = (dx * dx + dy2 + a2) * inv2a;
    float db = d - k.a;
    if (wave) {
      const int g = (x - pa) >> 2;
      if (g != wg) {
        const float gx = pa + 4 * g + 0.5f - cx;
        if (g == wg + 1) wr0 = wr1, wc0 = wc1;
        else wave_r(k, gx, dy, &wr0, &wc0);
        wave_r(k, gx + 4, dy, &wr1, &wc1);
        wg = g;
      }
      const float t = ((x - pa) & 3) * 0.25f;
      db = (d - (wr0 + (wr1 - wr0) * t)) * (wc0 + (wc1 - wc0) * t);
    }
    const float dd = db + (pill_dist(k, dx, dy) - db) * k.m;
    if (!ring) {
      if (dd >= 0.5f) {
        x += max(1, (int)((dd - 0.5f) * 0.75f));
        continue;
      }
      if (dd <= -0.5f) {
        const int n = max(1, (int)((-dd - 0.5f) * 0.9f));
        halo_span(row, x, min(pb, x + n - 1), c);
        x += n;
        continue;
      }
      pl_blend(&row[x], color, (int)((0.5f - dd) * 256));
      x++;
      continue;
    }
    const float cov = hw + 0.5f - fabsf(dd);
    if (cov <= 0) {
      x += max(1, (int)(-cov * 0.75f));
      continue;
    }
    const bool in_f = ring->fill.on && in_sector(ring->fill, dx, dy);
    const bool in_t = !in_f && ring->track.on && in_sector(ring->track, dx, dy);
    if ((in_f || in_t) && cov >= 2) {
      const int n = min(pb - x, (int)((cov - 1) * 0.75f));
      if (n >= 1 && in_sector(in_f ? ring->fill : ring->track, dx + n, dy)) {
        halo_span(row, x, x + n, in_f ? cf16 : ct16);
        x += n + 1;
        continue;
      }
    }
    float cf = in_f ? cov : 0, ct = in_t ? cov : 0;
    if (!in_f && !in_t)
      for (int e = 0; e < 4; e++) {
        const float ux = x + 0.5f - ring->cap[e][0], uy = y + 0.5f - ring->cap[e][1], q = ux * ux + uy * uy;
        if (q < (hw + 1) * (hw + 1)) {
          const float cc = hw + 0.5f - halo_sqrt(q);
          if (e < 2 && ring->fill.on) cf = fmaxf(cf, cc);
          if (e >= 2 && ring->track.on) ct = fmaxf(ct, cc);
        }
      }
    if (cf >= 1) row[x] = cf16;
    else if (ct >= 1) row[x] = ct16;
    else if (cf > 0) pl_blend(&row[x], ring->c_fill, (int)(cf * 256));
    else if (ct > 0) pl_blend(&row[x], ring->c_track, (int)(ct * 256));
    x++;
  }
}

static void shape_draw(float cx, float cy, const ShapeK &k, float rin, float rout, uint32_t color, const RingK *ring) {
  if (!halo_hits(cx - rout - 2, cy - rout - 2, cx + rout + 2, cy + rout + 2)) return;
  const uint16_t c = halo_565(color);
  const float out2 = (rout + 1) * (rout + 1), in2 = rin > 1 ? (rin - 1) * (rin - 1) : 0;
  const bool wave = k.b != 0, pill = k.m > 0.001f;
  const uint16_t cf16 = ring ? halo_565(ring->c_fill) : 0, ct16 = ring ? halo_565(ring->c_track) : 0;
  const int ya = max(hb.y, halo_floor(cy - rout - 1)), yb = min(hb.y + hb.rows - 1, (int)(cy + rout + 2));
  for (int y = ya; y <= yb; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= out2) continue;
    uint16_t *row = halo_row(y);
    const float xo = halo_sqrt(out2 - dy2), xi = dy2 < in2 ? halo_sqrt(in2 - dy2) : -1;
    const int xa = max(hb.x0, halo_floor(cx - xo)), xb = min(hb.x1, (int)(cx + xo) + 1);
    int sa = xi < 0 ? xb + 1 : max(xa, (int)(cx - xi) + 1), sb = xi < 0 ? xb : min(xb, halo_floor(cx + xi) - 1);
    if (sa > sb) sa = xb + 1, sb = xb;
    if (!ring && sa <= sb) halo_span(row, sa, sb, c);  // the middle of the button — filled
    for (int part = 0; part < 2; part++) {
      const int pa = part ? sb + 1 : xa, pb = part ? xb : sa - 1;
      if (pa > pb) continue;
      if (!pill) shape_row_int(row, pa, pb, cx, dy, k, wave, ring, c, color, cf16, ct16, y);
      else shape_row_float(row, pa, pb, cx, dy, y, k, ring, c, color, cf16, ct16);
    }
  }
}

// ---------- Icons into masks: drawn once at start, a mask per frame of their animations ----------
#define PL_IC 64
static HaloMask pl_m_pp[11], pl_m_skip[2][31];  // play/pause by tenths of the morph; skip: 30 frames + rest (both ways)
struct Poly6 {
  float nx[6], ny[6], nc[6];
};
static Poly6 play_half[2];
static const float PLAY_V[2][12] = {  // Lottie M3PlayPause: the play triangle's halves
    {4.6f, -11.0f, 4.4f, 10.1f, -5.8f, 16.7f, -11.6f, 14.5f, -11.6f, -15.2f, -6.0f, -17.7f},
    {18.5f, -0.7f, 18.4f, -0.5f, 11.6f, 5.6f, 3.6f, 10.6f, 3.8f, -11.5f, 11.5f, -6.8f},
};
static void poly_make(Poly6 &p, const float *v) {
  float area = 0;
  for (int i = 0; i < 6; i++) area += v[2 * i] * v[2 * ((i + 1) % 6) + 1] - v[2 * ((i + 1) % 6)] * v[2 * i + 1];
  const float sgn = area > 0 ? 1 : -1;
  for (int i = 0; i < 6; i++) {
    float ax = v[2 * i], ay = v[2 * i + 1], bx = v[2 * ((i + 1) % 6)], by = v[2 * ((i + 1) % 6) + 1];
    float ex = bx - ax, ey = by - ay, l = halo_sqrt(ex * ex + ey * ey);
    if (l < 1e-3f) ex = 1, ey = 0, l = 1e9f;
    p.nx[i] = sgn * ey / l, p.ny[i] = -sgn * ex / l, p.nc[i] = p.nx[i] * ax + p.ny[i] * ay;
  }
}
static inline float poly_dist(const Poly6 &p, float x, float y) {
  float d = -1e9f;
  for (int i = 0; i < 6; i++) d = fmaxf(d, p.nx[i] * x + p.ny[i] * y - p.nc[i]);
  return d;
}
static void cv_put(uint8_t *cv, int x, int y, float cov) {
  if (x < 0 || y < 0 || x >= PL_IC || y >= PL_IC || cov <= 0) return;
  const int v = (int)(fminf(cov, 1) * 255);
  if (v > cv[y * PL_IC + x]) cv[y * PL_IC + x] = v;
}
static void cv_capsule(uint8_t *cv, float x0, float y0, float x1, float y1, float r) {
  const float ex = x1 - x0, ey = y1 - y0, l2 = ex * ex + ey * ey, il2 = l2 > 1e-6f ? 1 / l2 : 0;
  for (int y = (int)(fminf(y0, y1) - r - 1); y <= (int)(fmaxf(y0, y1) + r + 2); y++)
    for (int x = (int)(fminf(x0, x1) - r - 1); x <= (int)(fmaxf(x0, x1) + r + 2); x++) {
      const float px = x + 0.5f - x0, py = y + 0.5f - y0, t = halo_clamp01((px * ex + py * ey) * il2);
      const float qx = px - t * ex, qy = py - t * ey;
      cv_put(cv, x, y, r + 0.5f - halo_sqrt(qx * qx + qy * qy));
    }
}
static void cv_playpause(uint8_t *cv, float t) {  // pause bars (capsules) flow into the play triangle's halves
  const float cx = PL_IC / 2, cy = PL_IC / 2;
  for (int y = 0; y < PL_IC; y++)
    for (int x = 0; x < PL_IC; x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy, qy = fmaxf(fabsf(py) - 14.45f, 0);
      float d = 1e9f;
      for (int h = 0; h < 2; h++) {
        const float bx = px - (h ? 11.55f : -11.55f), dp = halo_sqrt(bx * bx + qy * qy) - 5.75f;
        d = fminf(d, dp + (poly_dist(play_half[h], px, py) - dp) * t);
      }
      cv_put(cv, x, y, 0.5f - d);
    }
}
static inline float lerpf_(float a, float b, float t) {
  return a + (b - a) * t;
}
static void cv_skip(uint8_t *cv, float t, bool mirror) {  // Lottie M3Next; t — s since the press (≥ 0.5 — at rest)
  const float cx = PL_IC / 2, cy = PL_IC / 2, s = mirror ? -1 : 1, r = 2.1f;
  auto tri = [&](float ox, float nose, float k) {
    const float bx = cx + s * (-12.6f + ox), nx = cx + s * (nose + ox), h = 9.2f * k;
    cv_capsule(cv, bx, cy - h, bx, cy + h, r);
    cv_capsule(cv, bx, cy - h, nx, cy, r);
    cv_capsule(cv, bx, cy + h, nx, cy, r);
  };
  auto bar = [&](float ox, float len) {
    const float x = cx + s * (11.7f + ox), h = fmaxf(0, len / 2 - 2.15f);
    cv_capsule(cv, x, cy - h, x, cy + h, 2.15f);
  };
  if (t >= 0.5f) {
    tri(0, 2.7f, 1), bar(0, 23.7f);
    return;
  }
  const float f = t * 60;
  if (f < 14) {
    const float mv = halo_ease(EASE_PL_EMPH, f / 20), flat = halo_ease(EASE_PL_SKIP_IN, f / 11);
    tri(24.4f * mv, lerpf_(2.7f, -12.6f, flat), 1);
    bar(6.4f * mv, f < 7 ? 23.7f : lerpf_(23.7f, 4.3f, halo_clamp01((f - 7) / 6)));
  }
  if (f >= 7) {
    const float g = f < 18 ? halo_ease(EASE_PL_STD, (f - 7) / 11) * 1.09f : lerpf_(1.09f, 1, halo_ease(EASE_PL_STD, (f - 18) / 9));
    tri(0, lerpf_(-12.6f, 2.7f, g), fminf(1, g));
  }
  if (f >= 14) {
    const float ox = f < 20 ? lerpf_(-1.5f, 1.0f, halo_ease(EASE_PL_STD, (f - 14) / 6)) : lerpf_(1.0f, 0, halo_ease(EASE_PL_STD, (f - 20) / 9));
    bar(ox, 23.7f);
  }
}
static void cv_mask(HaloMask &m, const uint8_t *cv) {  // coverage → a 4-bit mask in PSRAM
  const int bpr = (PL_IC + 1) / 2;
  uint8_t *bits = (uint8_t *)heap_caps_calloc(PL_IC * bpr + PL_IC * 2, 1, MALLOC_CAP_SPIRAM);
  if (!bits) return;
  uint8_t *ext = bits + PL_IC * bpr;
  for (int y = 0; y < PL_IC; y++) {
    int first = PL_IC, last = -1;
    for (int x = 0; x < PL_IC; x++) {
      const int v = (cv[y * PL_IC + x] * 15 + 127) / 255;
      if (!v) continue;
      bits[y * bpr + x / 2] |= x & 1 ? v : v << 4;
      first = min(first, x), last = max(last, x);
    }
    ext[2 * y] = first > last ? 1 : first, ext[2 * y + 1] = first > last ? 0 : last;
  }
  m = {PL_IC, PL_IC, 0, 0, 0, bits, ext};
}

// ---------- Sound and tracks ----------
static void pl_set_playing(bool on) {
  if (!mu_n) return;
  if (on) {
    music_start_audio();
    if (mu_cur != pl_cur) mu_cmd = pl_cur;
  }
  pl_playing = on, mu_play = on;
  pl_pp.target = on ? 0 : 1;
}

static void pl_change_track(int dir) {
  if (!mu_n) return;
  pl_prev = pl_cur;
  pl_cur = (pl_cur + dir + mu_n) % mu_n;
  pl_change_t = 0, pl_change_dir = dir;
  pl_marquee_x = 0, pl_marquee_wait = 4;
  mu_cmd = pl_cur;  // from its beginning; plays if playing
}

// Volume 0..10 → the codec: 0 — silence, 1..10 — −40 … +6 dB in even steps for the ear (the codec's register is
// 0.5 dB a step, 0 dB = 0xBF; its top, +32 dB, only distorts music)
static int pl_codec_volume(int vol) {
  if (vol <= 0) return 0;
  const float db = -40 + (vol - 1) * 46.0f / 9, reg = 0xBF + 2 * db;
  return (int)lroundf((reg + 1) * 100 / 256);
}

static void pl_volume_set(int v) {
  pl_vol = constrain(v, 0, 10);
  halo_audio_volume(pl_codec_volume(pl_vol));
  pl_vol_from = pl_vol_lvl, pl_vol_t = 0;
  pl_vol_show.target = 1, pl_vol_hide_in = 2.5f;
}

// The row's cells (ButtonGroup): widths from the press springs
static void pl_cells(float w[3], float x[3]) {
  w[0] = 153, w[1] = 160, w[2] = 153;
  const float gl = pl_grp[0].s.x * 24, gr = pl_grp[2].s.x * 24, gm = pl_grp[1].s.x * 12;
  w[0] += gl, w[1] -= gl;
  w[2] += gr, w[1] -= gr;
  w[1] += 2 * gm, w[0] -= gm, w[2] -= gm;
  x[0] = w[0] / 2 + 10, x[1] = w[0] + w[1] / 2, x[2] = w[0] + w[1] + w[2] / 2 - 10;
}

// ---------- Build, tick (every frame), update (the finger, only while the screen is up) ----------
static void player_build() {
  K_EFFECT_FAST = halo_spring(halo_stiff(1400), 1);
  K_EFFECT_SLOW = halo_spring(halo_stiff(260), 1);
  K_GROUP_PRESS = halo_spring(0.222f / 2, 0.70f);  // faster(100): stiffness × 4
  K_M3X_FAST = halo_spring(HALO_M3X_FAST);
  halo_ease_make(EASE_PL_EMPH, 0.4f, 0, 0.2f, 1);
  halo_ease_make(EASE_PL_SKIP_IN, 0.632f, 0, 0.478f, 1);
  halo_ease_make(EASE_PL_STD, 0.2f, 0, 0, 1);
  poly_make(play_half[0], PLAY_V[0]), poly_make(play_half[1], PLAY_V[1]);
  static uint8_t cv[PL_IC * PL_IC];
  for (int i = 0; i <= 10; i++) memset(cv, 0, sizeof(cv)), cv_playpause(cv, i / 10.0f), cv_mask(pl_m_pp[i], cv);
  for (int d = 0; d < 2; d++)
    for (int f = 0; f <= 30; f++) memset(cv, 0, sizeof(cv)), cv_skip(cv, f < 30 ? f / 60.0f : 9, d == 0), cv_mask(pl_m_skip[d][f], cv);
  for (int i = 0; i < mu_n; i++) {
    pl_len[i] = fmaxf(0.1f, mu_tracks[i].samples / (float)mu_rate);
    pl_tw[i] = halo_text_width(t_title, mu_tracks[i].title), pl_aw[i] = halo_text_width(t24, mu_tracks[i].artist);
  }
  pl_mid_t = halo_text_mid(t_title, "Gymnopedie"), pl_mid_a = halo_text_mid(t24, "Erik Satie");
  pl_mid_i = halo_text_mid(ic24, "");
  halo_audio_volume(pl_codec_volume(pl_vol));
  pl_vol_lvl = pl_vol_from = pl_vol / 10.0f;
}

static void player_tick(float dt) {
  if (mu_ended) mu_ended = false, pl_change_track(1);  // the track ran out — the next one
  pl_elapsed = mu_cur == pl_cur ? mu_pos / (float)mu_rate : 0;
  const HaloFinger &f = halo_finger;
  const bool vs = pl_vol_screen.target > 0.5f, held = f.down && pl_active && !vs;
  for (int i = 0; i < 3; i++) press_update(pl_grp[i], held && pl_armed == i, dt, K_GROUP_PRESS);
  press_update(pl_center, held && pl_armed >= 0, dt, K_GROUP_PRESS);
  for (int i = 0; i < 2; i++) press_update(pl_small[i], held && pl_small_armed == i, dt);
  for (int i = 0; i < 2; i++) press_update(pl_vbtn[i], f.down && pl_active && vs && pl_vol_armed == i, dt);
  pl_active = false;  // player_update sets it again while the screen is up
  pl_like_m.target = pl_liked;
  halo_spring_run(pl_like_m, K_M3X_FAST, dt);
  halo_spring_run(pl_vol_screen, K_EFFECT_FAST, dt);
  if (pl_playing && pl_center.s.x < 0.001f) {  // the wave turns: −36° in 3 s (the shape repeats every 36°)
    pl_rot -= dt * (2 * HALO_PI / 10) / 3;
    if (pl_rot < -2 * HALO_PI / 10) pl_rot += 2 * HALO_PI / 10;
  } else {
    pl_rot = 0;
  }
  sh_c10 = cosf(10 * pl_rot), sh_s10 = sinf(10 * pl_rot);
  const float step = dt / 0.167f;  // "play/pause": 167 ms, linear
  pl_pp.x += pl_pp.target > pl_pp.x ? fminf(step, pl_pp.target - pl_pp.x) : -fminf(step, pl_pp.x - pl_pp.target);
  for (int i = 0; i < 2; i++) pl_skip_t[i] = fminf(pl_skip_t[i] + dt, 9);
  pl_change_t = fminf(pl_change_t + dt, 9);
  if (mu_n && pl_tw[pl_cur] > PL_TITLE_W && pl_change_t > 0.4f) {  // the marquee: 64 dp/s, a 4 s pause, a 96 dp gap
    if (pl_marquee_wait > 0) pl_marquee_wait -= dt;
    else if ((pl_marquee_x += dt * 64 * DP) >= pl_tw[pl_cur] + 96 * DP) pl_marquee_x = 0, pl_marquee_wait = 4;
  } else {
    pl_marquee_x = 0;
  }
  pl_vol_t = fminf(pl_vol_t + dt, 9);  // the level: 500 ms along (0, 0, 0, 1); the arc hides after 2.5 s
  pl_vol_lvl = pl_vol_from + (pl_vol / 10.0f - pl_vol_from) * halo_ease(EASE_SHARP, pl_vol_t / 0.5f);
  pl_vol_hide_in = fmaxf(pl_vol_hide_in - dt, -1);
  if (pl_vol_hide_in < 0) pl_vol_show.target = 0;
  halo_spring_run(pl_vol_show, pl_vol_show.target > 0.5f ? K_EFFECT_FAST : K_EFFECT_SLOW, dt);
}

static void player_update(float dt) {
  HaloFinger &f = halo_finger;
  pl_active = true;
  const bool vs = pl_vol_screen.target > 0.5f;
  if (vs) {  // the volume screen: "+" at the top, "−" at the bottom; a tap elsewhere or 3 s idle — close
    if (f.pressed) pl_vol_armed = fabsf(f.x0 - CX) < 66 && fabsf(f.y0 - 118) < 54 ? 0 : fabsf(f.x0 - CX) < 66 && fabsf(f.y0 - 348) < 54 ? 1 : -1;
    if (f.dragging) pl_vol_armed = -1;
    if (f.tap) {
      if (pl_vol_armed >= 0) pl_volume_set(pl_vol + (pl_vol_armed ? -1 : 1)), pl_vol_idle_in = 3;
      else pl_vol_screen.target = 0;
    }
    if ((pl_vol_idle_in -= dt) < 0) pl_vol_screen.target = 0;
    if (!f.down) pl_vol_armed = -1;
    return;
  }
  float w[3], x[3];
  pl_cells(w, x);
  if (f.pressed && !f.owner) {
    pl_armed = pl_small_armed = -1;
    if (fabsf(f.y0 - CX) < 80)
      for (int i = 0; i < 3; i++)
        if (fabsf(f.x0 - x[i]) < w[i] / 2) pl_armed = i;
    for (int i = 0; i < 2; i++)
      if (fabsf(f.x0 - (i ? 315.5f : 150.5f)) < 50 && fabsf(f.y0 - PL_SMALL_Y) < 38) pl_small_armed = i;
  }
  if (f.dragging) pl_armed = pl_small_armed = -1;
  if (f.tap && pl_armed >= 0) {
    if (pl_armed == 1) pl_set_playing(!pl_playing);
    else pl_change_track(pl_armed == 2 ? 1 : -1), pl_skip_t[pl_armed == 2] = 0;
  }
  if (f.tap && pl_small_armed == 0) pl_vol_screen.target = 1, pl_vol_idle_in = 3, pl_vol_show.target = 0;
  if (f.tap && pl_small_armed == 1) pl_liked = !pl_liked;
  if (pl_armed >= 0) pl_last = pl_armed;
  if (!f.down) pl_armed = pl_small_armed = -1;
}

// ---------- Layout and drawing ----------
static struct {
  ShapeK kb, kr;
  RingK rk;
  float cx, cy, r_in, r_out, b_in, b_out;
  uint32_t c_btn;
} BK;
static HaloItem pl_side[2], pl_side_ic[2], pl_big_ic, pl_sm[2], pl_sm_ic[2], pl_arc[2], pl_vb[2], pl_vb_ic[2], pl_vol_ic;
struct PlDmg {
  uint32_t h;
  float r[4];
};
static PlDmg pl_dmg_text, pl_dmg_big;

static inline uint32_t pl_mix(uint32_t h, float v) {
  int32_t q = (int32_t)(v * 64);
  return (h ^ (uint32_t)q) * 16777619u;
}
// A custom-drawn part changed (its state hash or its place) — redraw the old place and the new one
static void pl_damage(PlDmg &d, uint32_t h, float x0, float y0, float x1, float y1) {
  if (h == d.h && x0 == d.r[0] && y0 == d.r[1] && x1 == d.r[2] && y1 == d.r[3]) return;
  if (d.h) halo_damage_f(d.r[0], d.r[1], d.r[2], d.r[3]);
  halo_damage_f(x0, y0, x1, y1);
  d.h = h, d.r[0] = x0, d.r[1] = y0, d.r[2] = x1, d.r[3] = y1;
}

static void pl_ring_shape(ShapeK &k, bool ring, float w_pill, float h_pill, float s) {
  if (pl_playing) k.a = ring ? 72.84f : 60.84f, k.b = ring ? 3.50f : 3.44f;
  else k.a = ring ? 76 : 64, k.b = 0;
  k.a *= s, k.b *= s, k.ia = 1 / k.a;
  k.pw = w_pill / 2 * s, k.ph = h_pill / 2 * s, k.m = halo_clamp01(pl_center.s.x);
}

static void player_enter() {
  pl_armed = pl_small_armed = pl_vol_armed = -1;
}

static void player_layout(const HaloView &v) {
  pl_v = v;
  const float s = v.s, vs = halo_clamp01(pl_vol_screen.x);
  const float f_pl = halo_clamp01(1 - vs / 0.4f), f_vol = halo_clamp01((vs - 0.4f) / 0.6f), pa = f_pl;  // fade through
  float w[3], x[3];
  pl_cells(w, x);
  // Side buttons: capsules (cell − 48) × 108, primaryDim; their icons — masks of the skip animation
  for (int i = 0; i < 2; i++) {
    const int c = i ? 2 : 0;
    HaloItem &b = pl_side[i];
    b.kind = HI_RRECT, b.x = x[c], b.y = CX, b.w = w[c] - 48, b.h = PL_SIDE_H, b.r = PL_SIDE_H / 2, b.scale = 1;
    b.color = PL_PRIMARY_DIM, b.alpha = pa, b.on_black = true;
    halo_view_commit(b, v);
    HaloItem &ic = pl_side_ic[i];
    const float t = pl_skip_t[i];
    ic.kind = HI_MASK, ic.mask = &pl_m_skip[i][t >= 0.5f ? 30 : min(29, (int)(t * 60))];  // [0] — previous (mirrored)
    ic.x = x[c], ic.y = CX, ic.scale = 1, ic.color = PL_ON_PRIMARY, ic.alpha = pa, ic.on_black = false, ic.on = &b;
    halo_view_commit(ic, v);
  }
  // The big button: shapes, ring sectors and caps — once a frame (drawn in player_draw)
  const bool mid = pl_last == 1;
  pl_ring_shape(BK.kb, false, mid ? 152 : 104, 128, s);
  pl_ring_shape(BK.kr, true, mid ? 176 : 128, 152, s);
  BK.cx = halo_view_x(v, x[1]), BK.cy = halo_view_y(v, CX);
  const ShapeK &r = BK.kr, &k = BK.kb;
  const bool pr = r.m > 0.001f;
  BK.rk.hw = PL_RING_HW * s;
  BK.r_in = (pr ? fminf(r.a - r.b, fminf(r.pw, r.ph)) : r.a - r.b) - BK.rk.hw - 1;
  BK.r_out = (pr ? fmaxf(r.a + r.b, fmaxf(r.pw, r.ph)) : r.a + r.b) + BK.rk.hw + 1;
  BK.b_in = (pr ? fminf(k.a - k.b, fminf(k.pw, k.ph)) : k.a - k.b) - 1;
  BK.b_out = (pr ? fmaxf(k.a + k.b, fmaxf(k.pw, k.ph)) : k.a + k.b) + 1;
  const float prog = mu_n ? halo_clamp01(pl_elapsed / pl_len[pl_cur]) * 2 * HALO_PI : 0, gap = 4.05f * HALO_PI / 180;
  const float ends[4] = {gap, prog - gap, prog + gap, 2 * HALO_PI - gap};
  BK.rk.fill = sector_make(ends[0], ends[1]), BK.rk.track = sector_make(ends[2], ends[3]);
  for (int e = 0; e < 4; e++) {  // the ends' centers — on the ring's middle line
    const float ux = sinf(ends[e]), uy = -cosf(ends[e]);
    float rr = 76 * s;
    for (int it = 0; it < 3; it++) rr -= shape_dist(BK.kr, ux * rr, uy * rr);
    BK.rk.cap[e][0] = BK.cx + ux * rr, BK.rk.cap[e][1] = BK.cy + uy * rr;
  }
  const float ka = pa * v.a;
  BK.rk.c_fill = halo_dim(PL_RING, (int)(ka * 256)), BK.rk.c_track = halo_dim(PL_RING_TRACK, (int)(ka * 256));
  BK.c_btn = halo_dim(PL_PRIMARY, (int)(ka * 256));
  uint32_t h = 2166136261u;
  for (float q : {BK.cx, BK.cy, s, r.a, r.b, r.m, r.pw, k.pw, prog * 20, pl_rot * 400, ka}) h = pl_mix(h, q);
  pl_damage(pl_dmg_big, ka > 0.004f ? h | 1 : 1, BK.cx - BK.r_out - 2, BK.cy - BK.r_out - 2, BK.cx + BK.r_out + 2,
            BK.cy + BK.r_out + 2);
  HaloItem &bi = pl_big_ic;  // "play/pause": masks of the morph by tenths
  bi.kind = HI_MASK, bi.mask = &pl_m_pp[(int)lroundf(halo_clamp01(pl_pp.x) * 10)], bi.x = x[1], bi.y = CX, bi.scale = 1;
  bi.color = PL_ON_PRIMARY, bi.alpha = pa, bi.on_black = false, bi.on = nullptr;
  halo_view_commit(bi, v);
  // The small buttons: volume and like (pressed — corners 32 → 16)
  for (int i = 0; i < 2; i++) {
    const float p = halo_clamp01(pl_small[i].s.x), cx = i ? 315.5f : 150.5f;
    HaloItem &b = pl_sm[i];
    b.kind = HI_RRECT, b.x = cx, b.y = PL_SMALL_Y, b.w = 88, b.h = 64, b.r = 32 + (16 - 32) * p, b.scale = 1;
    b.color = PL_SMALL, b.alpha = pa, b.on_black = true;
    halo_view_commit(b, v);
    const float lm = halo_clamp01(pl_like_m.x), bounce = i ? 1 + 0.15f * fmaxf(0, pl_like_m.x - 1) * 4 : 1;
    text_mid(pl_sm_ic[i], ic24, i ? "" : "", pl_mid_i, cx, PL_SMALL_Y, bounce,
             i ? pl_mix_rgb(PL_ON_SURFACE, PL_LIKE, lm) : PL_ON_SURFACE, pa, bounce == 1 ? &b : nullptr);
    halo_view_commit(pl_sm_ic[i], v);
  }
  // Volume: the arc at the left edge (LevelIndicator, r 223, 142.8°…217.2° from 3 o'clock), with the volume screen too
  const float va = fmaxf(pl_vol_show.x, f_vol), a0 = (142.8f + 90) * HALO_PI / 180, A = 74.4f * HALO_PI / 180;
  const float g = 2.25f * HALO_PI / 180, a1 = a0 + A * halo_clamp01(pl_vol_lvl);
  for (int i = 0; i < 2; i++) {
    HaloItem &it = pl_arc[i];
    const float from = i ? a1 + g : a0 + g, to = i ? a0 + A - g : a1 - g;
    it.kind = HI_ARC, it.x = CX, it.y = CX, it.r = 223, it.w = 12, it.scale = 1, it.a0 = from, it.a1 = to;
    it.color = i ? PL_VOL_TRACK : PL_VOL_FILL, it.alpha = va > 0.01f && to - from > 0.01f ? va : 0, it.on_black = true;
    halo_view_commit(it, v);
  }
  // The volume screen: "+" at the top, "−" at the bottom, the icon in the middle; grows from 0.92 (fade through)
  const float sc = 0.92f + 0.08f * f_vol;
  for (int i = 0; i < 2; i++) {
    const float p = halo_clamp01(pl_vbtn[i].s.x), my = CX + ((i ? 348.0f : 118.0f) - CX) * sc;
    HaloItem &b = pl_vb[i];
    b.kind = HI_RRECT, b.x = CX, b.y = my, b.w = 120 * sc, b.h = 96 * sc, b.r = (48 + (16 - 48) * p) * sc, b.scale = 1;
    b.color = PL_VOL_BTN, b.alpha = f_vol, b.on_black = true;
    halo_view_commit(b, v);
    text_mid(pl_vb_ic[i], ic24, i ? "" : "", pl_mid_i, CX, my, sc, PL_PRIMARY, f_vol, sc == 1 ? &b : nullptr);
    halo_view_commit(pl_vb_ic[i], v);
  }
  text_mid(pl_vol_ic, ic24, "", pl_mid_i, CX, CX, 1.6f * sc, PL_ON_SURFACE, f_vol, nullptr);
  halo_view_commit(pl_vol_ic, v);
  // Texts: drawn in player_draw (the marquee is clipped) — their area is redrawn when they change
  uint32_t ht = 2166136261u;
  for (float q : {(float)pl_cur, (float)pl_prev, pl_change_t < 0.45f ? pl_change_t : 9.0f, pl_marquee_x, pa * v.a,
                  v.x, v.y, s})
    ht = pl_mix(ht, q);
  const float tx0 = halo_view_x(v, CX - 200), tx1 = halo_view_x(v, CX + 200);
  pl_damage(pl_dmg_text, pa * v.a > 0.004f ? ht | 1 : 1, tx0, halo_view_y(v, PL_TITLE_Y - 30), tx1,
            halo_view_y(v, PL_ARTIST_Y + 22));
  if (pl_marquee_x > 0 || pl_change_t < 0.45f || pl_playing) halo_subpixel = true;
}

// A title or an artist line, clipped to [xa, xb] (the marquee) — this task's half of the band narrowed meanwhile
static void pl_text(const HaloFont &f, const char *s, float x, float top, float sc, uint32_t color, float xa, float xb) {
  const int sx0 = hb.x0, sx1 = hb.x1;
  hb.x0 = max(hb.x0, (int)xa), hb.x1 = min(hb.x1, (int)xb);
  if (hb.x0 <= hb.x1) halo_text(f, s, x, top, color, 1, 0, sc, 0, 0);
  hb.x0 = sx0, hb.x1 = sx1;
}

static void pl_fade_col(int x, float k, int y0, int y1) {  // the marquee's soft edge: a column toward black
  if (x < hb.x0 || x > hb.x1) return;
  for (int y = max(y0, hb.y); y <= min(y1, hb.y + hb.rows - 1); y++) halo_blend(halo_row(y) + x, 0, 0, 0, (int)((1 - k) * 256));
}

static void pl_texts(float alpha) {
  const HaloView &v = pl_v;
  if (!mu_n) {
    halo_text(t_title, "No music", halo_view_x(v, CX), halo_view_y(v, PL_TITLE_Y) - pl_mid_t * v.s,
              halo_dim(PL_ON_SURFACE, (int)(alpha * 256)), 1, 1, v.s, 0, 0);
    halo_text(t24, "tools/halo_music.py --flash", halo_view_x(v, CX), halo_view_y(v, PL_ARTIST_Y) - pl_mid_a * v.s,
              halo_dim(DIM, (int)(alpha * 256)), 1, 1, v.s, 0, 0);
    return;
  }
  // A new track: the old lines slide out and fade, the new ones slide in (±12.5 % of the width, 300 ms; delays
  // 0 / 60 ms for the title, 30 / 90 ms for the artist)
  for (int line = 0; line < 2; line++) {
    const float y = line ? PL_ARTIST_Y : PL_TITLE_Y, mid = line ? pl_mid_a : pl_mid_t, wmax = line ? 331 : PL_TITLE_W;
    const float d_out = line ? 0.03f : 0, d_in = line ? 0.09f : 0.06f, sh = 0.125f * wmax * pl_change_dir;
    const HaloFont &f = line ? t24 : t_title;
    for (int which = 0; which < 2; which++) {
      if (!which && pl_change_t >= 0.3f + d_out) continue;
      const int t = which ? pl_cur : pl_prev;
      const float p = halo_ease(EASE_PL_EMPH, (pl_change_t - (which ? d_in : d_out)) / 0.3f);
      const float a = (which ? p : 1 - p) * alpha, off = which ? -sh * (1 - p) : sh * p;
      if (a <= 0.01f) continue;
      const char *s = line ? mu_tracks[t].artist : mu_tracks[t].title;
      const float tw = line ? pl_aw[t] : pl_tw[t], top = halo_view_y(v, y) - mid * v.s;
      const uint32_t c = halo_dim(PL_ON_SURFACE, (int)(a * 256));
      const float xa = halo_view_x(v, CX - wmax / 2), xb = halo_view_x(v, CX + wmax / 2);
      if (!line && tw > wmax) {  // the marquee: two copies 96 dp apart, the edges fade over 8 dp
        const float x0 = halo_view_x(v, CX - wmax / 2 - (which ? pl_marquee_x : 0) + off);
        pl_text(f, s, x0, top, v.s, c, xa, xb);
        pl_text(f, s, x0 + (tw + 96 * DP) * v.s, top, v.s, c, xa, xb);
        const int y0 = (int)top, y1 = (int)(top + f.line * v.s);
        for (int e = 0; e < 16; e++) pl_fade_col((int)xa + e, e / 16.0f, y0, y1), pl_fade_col((int)xb - 1 - e, e / 16.0f, y0, y1);
      } else {
        halo_text(f, s, halo_view_x(v, CX - tw / 2 + off), top, c, 1, 0, v.s, 0, 0);
      }
    }
  }
}

static void player_draw() {
  const float vs = halo_clamp01(pl_vol_screen.x), pa = halo_clamp01(1 - vs / 0.4f) * pl_v.a;
  if (pa > 0.004f) {
    pl_texts(pa);
    for (int i = 0; i < 2; i++) halo_item_draw(pl_side[i]), halo_item_draw(pl_side_ic[i]);
    shape_draw(BK.cx, BK.cy, BK.kr, BK.r_in, BK.r_out, 0, &BK.rk);
    shape_draw(BK.cx, BK.cy, BK.kb, BK.b_in, BK.b_out, BK.c_btn, nullptr);
    halo_item_draw(pl_big_ic);
    for (int i = 0; i < 2; i++) halo_item_draw(pl_sm[i]), halo_item_draw(pl_sm_ic[i]);
  }
  for (auto &it : pl_arc) halo_item_draw(it);
  for (int i = 0; i < 2; i++) halo_item_draw(pl_vb[i]), halo_item_draw(pl_vb_ic[i]);
  halo_item_draw(pl_vol_ic);
}

// No vertical gestures of its own: to the face — swipe up from the bottom edge (or down from the top), as everywhere
static const HaloScreen PLAYER = {player_enter, player_update, player_layout, player_draw, false, HALO_HOME_BOTH};
