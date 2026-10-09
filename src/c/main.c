/* ---------------------------------------------------------------------------
 *  PEBBLE vs TILE
 *  Pseudo-3D, 100% vector, zero-bitmap helix-style smasher for Pebble (emery).
 *
 *  - No float / double / math.h : only sin_lookup(), cos_lookup() and 8.8 ints
 *  - No malloc in the game loop : static Disk[] / Particle[] pools, two
 *    pre-created GPaths whose points are rewritten each draw
 *  - Layout is derived from the layer bounds, so it adapts to any screen
 *
 *  Controls
 *    The tower spins by itself (faster the deeper you go).
 *    hold SELECT      dive and smash - time it so a safe gap is under the ball
 *                     (or flick your wrist = short smash)
 *    Chain disks without letting go of SELECT to build a COMBO. Smash 3 disks
 *    in a row and the meter fills: FIREBALL for 3 s - the ball is immune to
 *    Danger tiles and bursts every disk into 24 fire shards.
 *    No screen shake / camera wobble anywhere: the view stays rock steady.
 * ------------------------------------------------------------------------- */
#include <pebble.h>
#include <stdlib.h>
#include <string.h>
#include "game_types.h"

/* ============================== globals ================================== */
static Window   *s_window;
static Layer    *s_canvas;
static AppTimer *s_timer;
static GPath    *s_path_quad;   /* shared 4-point path: wedges, walls, shards */
static GPath    *s_path_tri;    /* shared 3-point path: shards, splat spikes  */

/* layout (computed from bounds in layout_init) */
static int s_w, s_h, s_cx;
static int s_rout;      /* disk outer radius (px)                        */
static int s_ry;        /* disk outer radius projected vertically        */
static int s_ball_dy;   /* surface offset (below disk centre) under ball */
static int s_rest_ty;   /* screen Y of disk 0's centre when ball rests   */

/* state machine */
static AppState s_state = STATE_TITLE;
static uint32_t s_frame;
static uint32_t s_state_frames;

/* world */
static Disk     s_disks[NUM_DISKS];
static Particle s_particles[MAX_PARTICLES];   /* ring buffer pool */
static int      s_particle_head;

static int32_t s_tower_rot;
static int32_t s_current_tower_y;   /* 8.8, screen Y of disk 0 centre (rendered) */
static int32_t s_target_tower_y;    /* 8.8, where the camera wants it            */
static int32_t s_ball_h, s_ball_vy; /* 8.8, height above active disk / velocity  */
static int s_cur, s_score, s_high, s_tap_frames;
static int s_combo;                 /* consecutive smashes, SELECT never released (scoring) */
static int s_meter;                 /* 0..FIREBALL_COMBO: fills toward fireball             */
static int s_fire_frames;           /* frames of fireball left                              */
static BallMode s_ball_mode = BALL_NORMAL;
static int s_toast_frames;
static const char *s_toast_text;
static int s_death_x, s_death_y;

/* input */
static bool s_btn_select, s_select_lock;

/* title scene */
static int32_t s_title_rot, s_title_h, s_title_vy;
#define TITLE_MASK 0x0C31
static const Disk s_title_disk = { 0, TITLE_MASK, false, 1, 0 };   /* 2 colours only */

static uint32_t s_rng = 2463534242u;
static char s_buf[40];

/* ============================== math utils =============================== */
static inline int32_t ang(int32_t a) { return a & (TRIG_MAX_ANGLE - 1); }

static uint32_t rnd(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

static int32_t rnd_range(int32_t lo, int32_t hi) {
  return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

static int32_t isqrt32(int32_t n) {   /* only ever called with tiny n */
  int32_t x = 0;
  while ((x + 1) * (x + 1) <= n) x++;
  return x;
}

static int32_t sector_start(int k) { return (k * TRIG_MAX_ANGLE) / NUM_SECTORS; }

/* point on a tilted (isometric) ellipse: angle a, radius r */
static GPoint proj(int cx, int cy, int32_t a, int r) {
  a = ang(a);
  return GPoint(cx + (cos_lookup(a) * r) / TRIG_MAX_RATIO,
                cy + (sin_lookup(a) * r * TILT_PCT) / (TRIG_MAX_RATIO * 100));
}

/* point on a true circle */
static GPoint polar(int cx, int cy, int32_t a, int r) {
  a = ang(a);
  return GPoint(cx + (cos_lookup(a) * r) / TRIG_MAX_RATIO,
                cy + (sin_lookup(a) * r) / TRIG_MAX_RATIO);
}

static int sector_under_ball(int32_t disk_rot) {
  return (ang(FRONT_ANGLE - disk_rot) * NUM_SECTORS) / TRIG_MAX_ANGLE;
}

/* ============================== colours ================================== */
/* Palette "Sky, Gold & Spring". Pair j = (safe_color(j), danger_color(j)); pair 0
 * alone is the 2-colour look of the early levels, pairs 1 and 2 are added later.
 * Safe colours are all LIGHT, danger colours all DARKER reds, and the three safe
 * colours differ in brightness as well as hue, so sky blue and spring green no
 * longer blend. Walls are the same hue one step darker (shading, not a new colour). */
static GColor safe_color(int j) {
  switch (j) {
    case 0:  return GColorVividCerulean;      /* sky blue     0,170,255 */
    case 1:  return GColorYellow;             /* gold       255,255,0   */
    default: return GColorMediumSpringGreen;  /* spring green 0,255,170 */
  }
}
static GColor danger_color(int j) {
  switch (j) {
    case 0:  return GColorRed;                /* red        255,0,0     */
    case 1:  return GColorJazzberryJam;       /* jazzberry  170,0,85    */
    default: return GColorBulgarianRose;      /* maroon      85,0,0     */
  }
}
static GColor safe_wall(int j) {
  switch (j) {
    case 0:  return GColorCobaltBlue;
    case 1:  return GColorLimerick;
    default: return GColorJaegerGreen;
  }
}
static GColor danger_wall(int j) {
  switch (j) {
    case 0:  return GColorDarkCandyAppleRed;
    case 1:  return GColorImperialPurple;
    default: return GColorBlack;
  }
}

static int pairs_for_disk(int i) {
  return (i < TIER2_DISK) ? 1 : (i < TIER3_DISK) ? 2 : 3;
}

static GColor top_color(const Disk *d, int k) {
  if (d->finish) return (k & 1) ? GColorWhite : GColorChromeYellow;
  int j = (k + d->seed) % d->pairs;
  return ((d->danger_mask >> k) & 1) ? danger_color(j) : safe_color(j);
}

static GColor wall_color(const Disk *d, int k) {
  if (d->finish) return (k & 1) ? GColorLightGray : GColorWindsorTan;
  int j = (k + d->seed) % d->pairs;
  return ((d->danger_mask >> k) & 1) ? danger_wall(j) : safe_wall(j);
}

static GColor pulse_color(int idx) {
  switch (idx) {
    case 0:  return GColorDarkGray;
    case 1:  return GColorLightGray;
    case 2:  return GColorYellow;
    default: return GColorWhite;
  }
}

/* ============================== primitives =============================== */
static void fill_quad(GContext *ctx, GPoint a, GPoint b, GPoint c, GPoint d, GColor col) {
  s_path_quad->points[0] = a;
  s_path_quad->points[1] = b;
  s_path_quad->points[2] = c;
  s_path_quad->points[3] = d;
  graphics_context_set_fill_color(ctx, col);
  gpath_draw_filled(ctx, s_path_quad);
}

static void fill_tri(GContext *ctx, GPoint a, GPoint b, GPoint c, GColor col) {
  s_path_tri->points[0] = a;
  s_path_tri->points[1] = b;
  s_path_tri->points[2] = c;
  graphics_context_set_fill_color(ctx, col);
  gpath_draw_filled(ctx, s_path_tri);
}

static void fill_circle(GContext *ctx, int x, int y, int r, GColor col) {
  if (r < 1) return;
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_circle(ctx, GPoint(x, y), (uint16_t)r);
}

static void text_shadow(GContext *ctx, const char *s, GFont f, GRect r,
                        GColor fg, GColor bg) {
  graphics_context_set_text_color(ctx, bg);
  graphics_draw_text(ctx, s, f, GRect(r.origin.x + 1, r.origin.y + 2, r.size.w, r.size.h),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, s, f, r, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_background(GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, GRect(0, 0, s_w, s_h), 0, GCornerNone);
}

/* ============================== tower pieces ============================= */
/* ---- Disk geometry -------------------------------------------------------
 * The tower is a FLUSH stack: SPACING == DISK_THICK, so the foot of one disk's
 * wall is exactly the top edge of the next disk's wall. Consequences:
 *   - the top face of every disk below the active one is hidden behind the
 *     disk above it, so only the ACTIVE disk draws a top face; all lower disks
 *     draw just their (tall) front-facing walls;
 *   - the wall under the ball's x position is always the sector the ball will
 *     hit on that disk, so the column of colours straight down is the preview.
 * Every face is a 4-point convex polygon projected with proj().              */

/* Front-facing side wall of one disk. The face uses the sector's full-strength
 * colour so Safe / Danger read clearly; a darker band at the foot (DISK_RIM)
 * separates it from the disk below. `lip` adds a darker edge where the top face
 * meets the wall (active disk only). */
static void draw_disk_walls(GContext *ctx, int cx, int ey, int32_t rot,
                            const Disk *d, bool lip) {
  for (int k = 0; k < NUM_SECTORS; k++) {
    int32_t a0 = rot + sector_start(k);
    int32_t a1 = rot + sector_start(k + 1);
    if (sin_lookup(ang((a0 + a1) / 2)) < 0) continue;   /* back side: hidden */
    GPoint o0 = proj(cx, ey, a0, s_rout);
    GPoint o1 = proj(cx, ey, a1, s_rout);
    GColor shade = wall_color(d, k);
    fill_quad(ctx, GPoint(o0.x, o0.y - 1), GPoint(o1.x, o1.y - 1),     /* -1: tucks under whatever */
              GPoint(o1.x, o1.y + DISK_THICK), GPoint(o0.x, o0.y + DISK_THICK),   /* is drawn after it, so */
              top_color(d, k));                                                    /* no 1 px seam shows    */
    fill_quad(ctx,
              GPoint(o0.x, o0.y + DISK_THICK - DISK_RIM), GPoint(o1.x, o1.y + DISK_THICK - DISK_RIM),
              GPoint(o1.x, o1.y + DISK_THICK + 1),        GPoint(o0.x, o0.y + DISK_THICK + 1),
              shade);                                      /* +1 px overlap hides seams */
    if (lip) {
      fill_quad(ctx, GPoint(o0.x, o0.y - 1), GPoint(o1.x, o1.y - 1),
                GPoint(o1.x, o1.y + 2), GPoint(o0.x, o0.y + 2), shade);
    }
  }
}

/* Top face: 12 wedges, each a 4-point polygon (inner pair + outer pair). */
static void draw_disk_top(GContext *ctx, int cx, int ey, int32_t rot, const Disk *d) {
  for (int k = 0; k < NUM_SECTORS; k++) {
    int32_t a0 = rot + sector_start(k);
    int32_t a1 = rot + sector_start(k + 1) + SEAM_OVERLAP;
    fill_quad(ctx, proj(cx, ey, a0, 0), proj(cx, ey, a0, s_rout),
              proj(cx, ey, a1, s_rout), proj(cx, ey, a1, 0),
              top_color(d, k));
  }
}

/* A complete disk (walls, then top face): the active disk and the title disk. */
static void draw_disk(GContext *ctx, int cx, int ey, int32_t rot, const Disk *d) {
  draw_disk_walls(ctx, cx, ey, rot, d, true);
  draw_disk_top(ctx, cx, ey, rot, d);
}

/* Central cylinder segment with 3 flat-shaded tiers (light / mid / dark). */
static void draw_spine(GContext *ctx, int cx, int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > s_h) y1 = s_h;
  if (y1 <= y0) return;
  int h = y1 - y0;
  int w = SPINE_W / 3;
  int x = cx - SPINE_W / 2;
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(x, y0, w, h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(x + w, y0, w, h), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(x + 2 * w, y0, SPINE_W - 2 * w, h), 0, GCornerNone);
}

/* ============================== ball + shadow ============================ */
#if defined(PBL_COLOR)
/* Real translucency: darken the 2-bit R/G/B channels of pixels already in the
 * framebuffer inside an ellipse. level 1 = soft, 2 = strong. */
static void shade_oval(GContext *ctx, int cx, int cy, int rx, int ry,
                       int level, bool dither) {
  if (ry < 1) return;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect fbb = gbitmap_get_bounds(fb);
  for (int dy = -ry; dy <= ry; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= fbb.size.h) continue;
    int hw = (rx * isqrt32(ry * ry - dy * dy)) / ry;
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int x0 = cx - hw, x1 = cx + hw;
    if (x0 < (int)row.min_x) x0 = row.min_x;
    if (x1 > (int)row.max_x) x1 = row.max_x;
    for (int x = x0; x <= x1; x++) {
      if (dither && ((x + y) & 1)) continue;
      uint8_t p = row.data[x];
      int r = (p >> 4) & 3, g = (p >> 2) & 3, b = p & 3;
      r = (r > level) ? r - level : 0;
      g = (g > level) ? g - level : 0;
      b = (b > level) ? b - level : 0;
      row.data[x] = (uint8_t)(0xC0 | (r << 4) | (g << 2) | b);
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}
#endif

/* All colours come from the COL_* / BALL_STYLE block at the top of game_types.h. */
static void draw_ball(GContext *ctx, int x, int y, bool fire) {
  if (fire) {
    int glow = BALL_R + 3 + (int)((s_frame >> 1) & 1);             /* flickering glow    */
#if FIRE_OUTLINE_PX > 0
    fill_circle(ctx, x, y, glow + FIRE_OUTLINE_PX, COL_FIRE_OUTLINE);
#endif
    fill_circle(ctx, x, y, glow,              COL_FIRE_GLOW);
    fill_circle(ctx, x, y, BALL_R + 1,        COL_FIRE_BORDER);    /* outer border       */
    fill_circle(ctx, x, y, BALL_R - 2,        COL_FIRE_CORE);      /* glowing centre     */
    fill_circle(ctx, x - 1, y - 1, 2,         COL_FIRE_HOT);       /* hot spot           */
    return;
  }
#if BALL_OUTLINE_PX > 0
  fill_circle(ctx, x, y, BALL_R + BALL_OUTLINE_PX, COL_BALL_OUTLINE);
#endif
  fill_circle(ctx, x,     y,     BALL_R,           COL_BALL_RIM);       /* rim / core shadow */
  fill_circle(ctx, x - 1, y - 1, BALL_R - 1,       COL_BALL_BODY);      /* base colour       */
  fill_circle(ctx, x - 2, y - 3, (BALL_R * 5) / 8, COL_BALL_MID);
  fill_circle(ctx, x - 3, y - 4, (BALL_R * 3) / 8, COL_BALL_LIGHT);     /* lighter highlight */
  fill_circle(ctx, x - 4, y - 5, 1,                COL_BALL_SPECULAR);  /* specular dot      */
}

/* Fireball look is shown for the whole mode, but blinks back to the normal
 * ball in the last FIREBALL_BLINK frames: "let go - immunity is ending". */
static bool fire_visible(void) {
  return s_ball_mode == BALL_FIREBALL &&
         (s_fire_frames > FIREBALL_BLINK || ((s_fire_frames >> 1) & 1));
}

/* surf_y = screen Y of the disk surface directly under the ball.
 * h_px   = ball height above that surface (drives shadow size + opacity). */
static void draw_ball_with_shadow(GContext *ctx, int x, int surf_y, int h_px, bool fire) {
  if (h_px < 0) h_px = 0;
  int t = (h_px > 32) ? 32 : h_px;
#if defined(PBL_COLOR)
  int rx = 6 + ((32 - t) * 6) / 32;          /* 12 px on the ground -> 6 px high up */
  int ry = (rx * 3) / 8;
  if (ry < 2) ry = 2;
  int level  = (t < 10) ? 2 : 1;
  bool faint = (t >= 20);
  shade_oval(ctx, x, surf_y, rx, ry, level, faint);
#endif
  draw_ball(ctx, x, surf_y - BALL_R - h_px, fire);
}

/* ============================== particles ================================ */
static void spawn_particle(int x, int y, int32_t vx, int32_t vy, int size_px,
                           GColor col, uint8_t life, uint8_t shrink) {
  Particle *p = &s_particles[s_particle_head];       /* ring: overwrite oldest */
  s_particle_head = (s_particle_head + 1) % MAX_PARTICLES;
  p->x = FP(x);
  p->y = FP(y);
  p->vx = vx;
  p->vy = vy;
  p->angle = (int32_t)(rnd() & (TRIG_MAX_ANGLE - 1));
  p->spin = rnd_range(-1800, 1800);
  p->size = FP(size_px);
  p->color = col;
  p->life = life;
  p->shrink = shrink;
  p->sides = (rnd() & 1) ? 3 : 4;
}

static GColor fire_color(void) {
  switch (rnd() % 3) {
    case 0:  return GColorRed;
    case 1:  return GColorOrange;
    default: return GColorYellow;
  }
}

/* Normal: 12..16 shards in the disk's own colours.
 * Fireball: FIRE_SHARDS (24) shards in a Red / Orange / Yellow palette. */
static void spawn_disk_shards(int cx, int ey, const Disk *d, bool fire) {
  int32_t off = d->angle_off + s_tower_rot;
  int n = fire ? FIRE_SHARDS : 12 + (int)(rnd() % 5);
  for (int i = 0; i < n; i++) {
    int32_t a = (int32_t)(rnd() & (TRIG_MAX_ANGLE - 1));
    int r = (int)rnd_range(6, s_rout - 2);
    GPoint pt = proj(cx, ey, a, r);
    int k = (ang(a - off) * NUM_SECTORS) / TRIG_MAX_ANGLE;
    int32_t sp = rnd_range(120, 700);
    int32_t vx = (cos_lookup(ang(a)) * sp) / TRIG_MAX_RATIO;
    int32_t vy = (sin_lookup(ang(a)) * sp * TILT_PCT) / (TRIG_MAX_RATIO * 100)
                 - rnd_range(300, 1100);
    spawn_particle(pt.x, pt.y, vx, vy, (int)rnd_range(3, 6),
                   fire ? fire_color() : top_color(d, k),
                   (uint8_t)rnd_range(18, 30), 238);
  }
}

static void spawn_burst(int x, int y, int n, GColor a, GColor b) {
  for (int i = 0; i < n; i++) {
    int32_t ra = (int32_t)(rnd() & (TRIG_MAX_ANGLE - 1));
    int32_t sp = rnd_range(250, 1100);
    spawn_particle(x, y,
                   (cos_lookup(ra) * sp) / TRIG_MAX_RATIO,
                   (sin_lookup(ra) * sp) / TRIG_MAX_RATIO - 300,
                   (int)rnd_range(2, 5), (i & 1) ? a : b,
                   (uint8_t)rnd_range(20, 34), 240);
  }
}

static void spawn_confetti(void) {
  for (int i = 0; i < 2; i++) {
    GColor c;
    switch (rnd() % 5) {
      case 0:  c = GColorYellow;            break;
      case 1:  c = GColorOrange;            break;
      case 2:  c = GColorCyan;              break;
      case 3:  c = GColorMediumSpringGreen; break;
      default: c = GColorMagenta;           break;
    }
    spawn_particle((int)rnd_range(0, s_w), -4, rnd_range(-120, 120),
                   rnd_range(60, 200), (int)rnd_range(3, 5), c, 110, 251);
  }
}

static void update_particles(void) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle *p = &s_particles[i];
    if (!p->life) continue;
    p->life--;
    p->vy += PART_GRAVITY_FP;
    p->x += p->vx;
    p->y += p->vy;
    p->angle += p->spin;
    p->size = (p->size * p->shrink) >> 8;           /* scale down */
    if (p->size < 100) p->life = 0;                 /* < ~0.4 px: despawn */
  }
}

static void draw_particles(GContext *ctx) {
  static const int8_t tri[3][2]  = { {64, 0}, {-40, 52}, {-24, -44} };
  static const int8_t quad[4][2] = { {60, -20}, {30, 56}, {-60, 24}, {-36, -50} };
  for (int i = 0; i < MAX_PARTICLES; i++) {
    const Particle *p = &s_particles[i];
    if (!p->life) continue;
    int px = p->x >> FP_SHIFT, py = p->y >> FP_SHIFT;
    if (px < -16 || px > s_w + 16 || py < -16 || py > s_h + 16) continue;
    int n = p->sides;
    GPath *path = (n == 3) ? s_path_tri : s_path_quad;
    const int8_t (*shape)[2] = (n == 3) ? tri : quad;
    int32_t c = cos_lookup(ang(p->angle)), s = sin_lookup(ang(p->angle));
    for (int j = 0; j < n; j++) {
      int32_t vx = (shape[j][0] * p->size) >> 6;
      int32_t vy = (shape[j][1] * p->size) >> 6;
      int32_t rx = (vx * c - vy * s) / TRIG_MAX_RATIO;
      int32_t ry = (vx * s + vy * c) / TRIG_MAX_RATIO;
      path->points[j] = GPoint(px + (rx >> FP_SHIFT), py + (ry >> FP_SHIFT));
    }
    graphics_context_set_fill_color(ctx, p->color);
    gpath_draw_filled(ctx, path);
  }
}

/* ============================== splat ==================================== */
static void draw_splat(GContext *ctx, int cx, int cy, int pct) {
  static const int8_t  jit[10] = { 3, -5, 6, -2, 5, -6, 2, -4, 7, -3 };  /* degrees */
  static const uint8_t len[10] = { 40, 28, 46, 30, 42, 26, 48, 32, 38, 29 };
  int base = (15 * pct) / 100;
  int32_t da = TRIG_MAX_ANGLE / 28;
  for (int i = 0; i < 10; i++) {
    int32_t a = (i * TRIG_MAX_ANGLE) / 10 + (jit[i] * TRIG_MAX_ANGLE) / 360;
    int L = (len[i] * pct) / 100;
    fill_tri(ctx, polar(cx, cy, a, L), polar(cx, cy, a - da, base),
             polar(cx, cy, a + da, base), GColorOrange);
    if (i & 1) {
      GPoint d = polar(cx, cy, a, L + 7);
      fill_circle(ctx, d.x, d.y, (3 * pct) / 100, GColorOrange);
    }
  }
  fill_circle(ctx, cx, cy, (19 * pct) / 100, GColorOrange);
  fill_circle(ctx, cx - 2, cy - 2, (13 * pct) / 100, GColorChromeYellow);
  if (pct >= 80) {                                  /* dizzy X eyes */
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(cx - 9, cy - 6), GPoint(cx - 4, cy - 1));
    graphics_draw_line(ctx, GPoint(cx - 9, cy - 1), GPoint(cx - 4, cy - 6));
    graphics_draw_line(ctx, GPoint(cx + 4, cy - 6), GPoint(cx + 9, cy - 1));
    graphics_draw_line(ctx, GPoint(cx + 4, cy - 1), GPoint(cx + 9, cy - 6));
    graphics_context_set_stroke_width(ctx, 1);
  }
}

/* ============================== scene renderers ========================== */
static void draw_prompt(GContext *ctx, const char *s, int y) {
  int32_t sn = sin_lookup(ang((int32_t)s_frame * 1500));
  int idx = ((sn + TRIG_MAX_RATIO) * 4) / (2 * TRIG_MAX_RATIO + 1);   /* 0..3 */
  text_shadow(ctx, s, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
              GRect(0, y, s_w, 36), pulse_color(idx), GColorBlack);
}

static void draw_title(GContext *ctx) {
  draw_background(ctx);
  int top = PBL_IF_ROUND_ELSE(22, 4);
  text_shadow(ctx, "PEBBLE", fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK),
              GRect(0, top, s_w, 38), GColorOrange, GColorBlack);
  text_shadow(ctx, "vs TILE", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
              GRect(0, top + 34, s_w, 34), GColorRed, GColorBlack);

  int ey = (s_h * PBL_IF_ROUND_ELSE(55, 60)) / 100;
  draw_disk(ctx, s_cx, ey, s_title_rot, &s_title_disk);
  draw_ball_with_shadow(ctx, s_cx, ey + s_ball_dy, s_title_h >> FP_SHIFT, false);

  int py = s_h - PBL_IF_ROUND_ELSE(72, 46);
  draw_prompt(ctx, "PRESS SELECT TO START", py);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, "Time SELECT to smash the gaps!",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, py + 20, s_w, 18), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

/* Combo meter: FIREBALL_COMBO pips fill as you chain smashes; once full it
 * becomes a draining 3-second fireball timer. */
static void draw_meter(GContext *ctx) {
  const int w = 16, h = 7, gap = 3;
  int total = FIREBALL_COMBO * w + (FIREBALL_COMBO - 1) * gap;
  int x0 = s_cx - total / 2;
  int y = PBL_IF_ROUND_ELSE(36, 30);
  if (s_ball_mode == BALL_FIREBALL) {
    int fill = (total * s_fire_frames) / FIREBALL_FRAMES;
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, GRect(x0 - 1, y - 1, total + 2, h + 2), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(x0, y, total, h), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, ((s_frame >> 1) & 1) ? GColorYellow : GColorOrange);
    graphics_fill_rect(ctx, GRect(x0, y, fill, h), 0, GCornerNone);
  } else {
    for (int i = 0; i < FIREBALL_COMBO; i++) {
      GColor c = GColorDarkGray;
      if (i < s_meter) c = (i == FIREBALL_COMBO - 1) ? GColorYellow : GColorOrange;
      graphics_context_set_fill_color(ctx, c);
      graphics_fill_rect(ctx, GRect(x0 + i * (w + gap), y, w, h), 0, GCornerNone);
    }
  }
}

static void draw_hud(GContext *ctx) {
  snprintf(s_buf, sizeof(s_buf), "%d", s_score);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
              GRect(0, PBL_IF_ROUND_ELSE(6, 0), s_w, 30), GColorWhite, GColorBlack);
  draw_meter(ctx);
#if !defined(PBL_ROUND)
  snprintf(s_buf, sizeof(s_buf), "LV %d", (s_cur < NUM_DISKS) ? s_cur + 1 : NUM_DISKS);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
              GRect(0, 6, 56, 18), GColorLightGray, GColorBlack);
  if (s_combo >= 2) {
    snprintf(s_buf, sizeof(s_buf), "x%d", s_combo);
    text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                GRect(s_w - 56, 6, 56, 18), GColorYellow, GColorBlack);
  }
#endif
}

static void draw_gameplay(GContext *ctx) {
  draw_background(ctx);

  int cx = s_cx;                                        /* camera never shakes or wobbles */
  int base = s_current_tower_y >> FP_SHIFT;             /* disk 0 centre Y (lerped scroll) */

  int first = s_cur;
  int last = first;
  while (last + 1 < NUM_DISKS && (base + (last + 1) * SPACING) < s_h) last++;

  /* Lower disks, bottom -> top. Their top faces are hidden by the flush stack,
   * so only the tall walls are drawn (cheap, and exactly what you can see). */
  for (int k = last; k > first && k < NUM_DISKS; k--) {
    const Disk *d = &s_disks[k];
    draw_disk_walls(ctx, cx, base + k * SPACING, d->angle_off + s_tower_rot, d, false);
  }

  if (first < NUM_DISKS) {
    int ey = base + first * SPACING;
    const Disk *d = &s_disks[first];
    draw_disk(ctx, cx, ey, d->angle_off + s_tower_rot, d);   /* active disk: walls + top */
    draw_spine(ctx, cx, 0, ey);       /* pole only rises ABOVE the top disk - none between disks */
    draw_ball_with_shadow(ctx, cx, ey + s_ball_dy, s_ball_h >> FP_SHIFT, fire_visible());
  }

  draw_particles(ctx);
  draw_hud(ctx);

  if (s_toast_frames > 0 && s_toast_text) {            /* "FIREBALL!" / "MORE COLORS!" / "SPEED UP!" */
    GColor c = ((s_toast_frames / 4) & 1) ? GColorYellow : GColorWhite;
    text_shadow(ctx, s_toast_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                GRect(0, (s_h * 52) / 100, s_w, 30), c, GColorBlack);
  }
}

static void draw_gameover(GContext *ctx) {
  draw_background(ctx);
  int cy = (s_h * 26) / 100;
  int pct = (s_state_frames < 8) ? (int)s_state_frames * 14 : 100;
  draw_splat(ctx, s_cx, cy, pct);
  text_shadow(ctx, "SPLAT!", fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK),
              GRect(0, cy + 30, s_w, 38), GColorWhite, GColorBlack);
  snprintf(s_buf, sizeof(s_buf), "SCORE %d", s_score);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
              GRect(0, cy + 70, s_w, 24), GColorYellow, GColorBlack);
  snprintf(s_buf, sizeof(s_buf), "BEST %d", s_high);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
              GRect(0, cy + 92, s_w, 24), GColorCyan, GColorBlack);
  draw_prompt(ctx, "PRESS SELECT TO RETRY", s_h - PBL_IF_ROUND_ELSE(72, 46));
  draw_particles(ctx);
}

static void draw_victory(GContext *ctx) {
  draw_background(ctx);
  draw_particles(ctx);
  int y0 = (s_h * 15) / 100;
  text_shadow(ctx, "YOU WIN!", fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK),
              GRect(0, y0, s_w, 38), GColorYellow, GColorBlack);
  text_shadow(ctx, "BOTTOM REACHED", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
              GRect(0, y0 + 40, s_w, 24), GColorMediumSpringGreen, GColorBlack);
  snprintf(s_buf, sizeof(s_buf), "SCORE %d", s_score);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
              GRect(0, y0 + 74, s_w, 30), GColorWhite, GColorBlack);
  snprintf(s_buf, sizeof(s_buf), "BEST %d", s_high);
  text_shadow(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
              GRect(0, y0 + 104, s_w, 24), GColorCyan, GColorBlack);
  draw_prompt(ctx, "SELECT: PLAY AGAIN", s_h - PBL_IF_ROUND_ELSE(72, 46));
}

static void canvas_update(Layer *layer, GContext *ctx) {
  (void)layer;
  switch (s_state) {
    case STATE_TITLE:    draw_title(ctx);    break;
    case STATE_GAMEPLAY: draw_gameplay(ctx); break;
    case STATE_GAMEOVER: draw_gameover(ctx); break;
    case STATE_VICTORY:  draw_victory(ctx);  break;
  }
}

/* ============================== game logic =============================== */
static void layout_init(GRect b) {
  s_w = b.size.w;
  s_h = b.size.h;
  s_cx = s_w / 2;
  s_rout = (s_w * 3) / 10;
  s_ry = (s_rout * TILT_PCT) / 100;
  s_ball_dy = (s_ry * RING_PCT) / 100;
  s_rest_ty = BALL_Y + BALL_R - s_ball_dy;   /* ball bottom touches the surface */
}

/* Auto-spin speed while the ball is above disk `idx`.
 * Constant until SPEED_DISK (colours are the difficulty first), then it ramps. */
static int32_t tower_spin(int idx) {
  if (idx < SPEED_DISK) return SPIN_BASE;
  int32_t v = SPIN_BASE + (idx - SPEED_DISK + 1) * SPIN_PER_DISK;   /* first faster disk = SPEED_DISK */
  return (v > SPIN_MAX) ? SPIN_MAX : v;
}

static inline bool mask_bit(uint16_t m, int k) {
  return (m >> (((k % NUM_SECTORS) + NUM_SECTORS) % NUM_SECTORS)) & 1;
}

/* Longest circular run of adjacent SAFE sectors (bit = 0) in a danger mask. */
static int longest_safe_arc(uint16_t mask) {
  if (mask == 0) return NUM_SECTORS;
  int p = 0;
  while (!mask_bit(mask, p)) p++;               /* start right after a Danger sector */
  int run = 0, best = 0;
  for (int i = 1; i <= NUM_SECTORS; i++) {
    if (mask_bit(mask, p + i)) run = 0;
    else if (++run > best) best = run;
  }
  return best;
}

/* Fairness rule applied to EVERY generated disk:
 *   1. a lone Safe sector (Danger on both sides) is a single-sector opening,
 *      which is not allowed -> it becomes Danger;
 *   2. what is left must contain an arc of >= MIN_SAFE_ARC adjacent Safe
 *      sectors; if not, MIN_SAFE_ARC sectors are opened at gap_centre.
 * After this, every Safe run on every disk is at least 2 sectors wide. */
static uint16_t enforce_safe_arcs(uint16_t mask, int gap_centre) {
  uint16_t out = mask;
  for (int k = 0; k < NUM_SECTORS; k++) {
    if (!mask_bit(mask, k) && mask_bit(mask, k - 1) && mask_bit(mask, k + 1))
      out |= (uint16_t)(1u << k);
  }
  if (longest_safe_arc(out) < MIN_SAFE_ARC) {
    for (int j = 0; j < MIN_SAFE_ARC; j++)
      out &= (uint16_t)~(1u << ((gap_centre + j) % NUM_SECTORS));
  }
  return out;
}

/* Builds the stack around a "gap corridor": a window of safe sectors whose
 * centre spirals down the tower in step with the spin, so a well-timed hold
 * of SELECT can chain several disks (a combo). Now and then the corridor
 * jumps to a random spot - read the walls below the ball and let go in time.
 * Everything outside the gap is mostly Danger "Tile"; enforce_safe_arcs()
 * then guarantees no single-sector openings and >= 2 adjacent safe sectors. */
static void generate_tower(void) {
  const int32_t sector_units = TRIG_MAX_ANGLE / NUM_SECTORS;
  int32_t c = FP((int)(rnd() % NUM_SECTORS));          /* gap centre, 8.8 sectors */
  for (int i = 0; i < NUM_DISKS; i++) {
    Disk *d = &s_disks[i];
    d->angle_off = 0;                                   /* tower-aligned */
    d->finish = (i == NUM_DISKS - 1);
    d->danger_mask = 0;
    d->pairs = (uint8_t)pairs_for_disk(i);
    d->seed = (uint8_t)(rnd() % NUM_SECTORS);
    if (d->finish) continue;                            /* finish disk: all safe */

    if (i > 0) {
      /* the ball slides past ~spin * (frames per disk) per disk -> follow it */
      c -= (tower_spin(i) * DIVE_FRAMES_FP) / sector_units;
      c += rnd_range(-90, 90);                          /* +-0.35 sector wobble */
      if (rnd() % 100 < 18) c = FP((int)(rnd() % NUM_SECTORS));   /* corridor break */
    }

    int centre = (int)((c + 128) >> FP_SHIFT);
    centre = ((centre % NUM_SECTORS) + NUM_SECTORS) % NUM_SECTORS;
    int half = (i == 0) ? 3 : (i < 6) ? 2 : 1;          /* 7 / 5 / 3 sectors wide */
    int dp = 50 + i * 2;                                /* % chance an off-gap sector is Danger */
    if (dp > 90) dp = 90;

    uint16_t m = 0;
    for (int k = 0; k < NUM_SECTORS; k++) {
      int dist = k - centre;
      if (dist < 0) dist = -dist;
      if (dist > NUM_SECTORS / 2) dist = NUM_SECTORS - dist;
      if (dist <= half) continue;                       /* in the gap: safe */
      if ((int)(rnd() % 100) < dp) m |= (uint16_t)(1u << k);
    }
    /* level 1 must introduce BOTH colours: guarantee one Danger sector opposite the gap */
    if (i == 0 && m == 0) m = (uint16_t)(1u << ((centre + NUM_SECTORS / 2) % NUM_SECTORS));

    d->danger_mask = enforce_safe_arcs(m, centre);
  }
}

static void start_game(void) {
  s_rng ^= (uint32_t)time(NULL) ^ (s_frame * 2654435761u);
  if (!s_rng) s_rng = 1;
  generate_tower();
  memset(s_particles, 0, sizeof(s_particles));
  s_particle_head = 0;
  s_cur = 0;
  s_score = 0;
  s_combo = 0;
  s_meter = 0;
  s_ball_mode = BALL_NORMAL;
  s_fire_frames = 0;
  s_tap_frames = 0;
  s_toast_frames = 0;
  s_toast_text = NULL;
  s_tower_rot = 0;
  s_ball_h = 0;
  s_ball_vy = BOUNCE_V_FP;
  s_current_tower_y = s_target_tower_y = FP(s_rest_ty);
  s_select_lock = s_btn_select;   /* a held SELECT must be released before it smashes */
  s_state = STATE_GAMEPLAY;
  s_state_frames = 0;
}

/* ================= combo meter + fireball state machine ====================
 *
 *   BALL_NORMAL ──(3rd consecutive smash)──► BALL_FIREBALL ──(3 s)──► BALL_NORMAL
 *      ▲  meter 0..2, fills +1 per disk           │ immune to Danger tiles,
 *      │  SELECT released / tap over -> meter 0   │ 24 fire shards per disk,
 *      └──────────────────────────────────────────┘ meter frozen at 0
 *
 * "Consecutive" = SELECT (or the wrist-tap smash) never let go: any frame
 * without smash input calls combo_break(). The fireball itself is timer-based
 * and is NOT cancelled by releasing SELECT.                                  */
static void fireball_start(void) {
  s_ball_mode = BALL_FIREBALL;
  s_fire_frames = FIREBALL_FRAMES;
  s_meter = 0;
  s_toast_text = "FIREBALL!";
  s_toast_frames = TOAST_FRAMES;
  vibes_long_pulse();
}

static void fireball_stop(void) {
  s_ball_mode = BALL_NORMAL;
  s_fire_frames = 0;
  s_meter = 0;                       /* meter starts over once the fire is out */
}

static void fireball_tick(void) {    /* once per gameplay frame */
  if (s_ball_mode == BALL_FIREBALL && --s_fire_frames <= 0) fireball_stop();
}

static void combo_break(void) {
  s_combo = 0;
  s_meter = 0;
}

static void combo_on_smash(void) {   /* called after every disk broken */
  s_combo++;
  s_score += s_combo;
  if (s_ball_mode == BALL_NORMAL && ++s_meter >= FIREBALL_COMBO) fireball_start();
}

static void end_run(AppState next) {
  if (s_score > s_high) {
    s_high = s_score;
    persist_write_int(PERSIST_KEY_HIGH, s_high);
  }
  fireball_stop();
  s_state = next;
  s_state_frames = 0;
}

static void player_die(void) {
  int ey = (s_current_tower_y >> FP_SHIFT) + s_cur * SPACING;
  s_death_x = s_cx;
  s_death_y = ey + s_ball_dy - BALL_R;
  spawn_burst(s_death_x, s_death_y, 14, GColorOrange, GColorRed);
  vibes_double_pulse();
  end_run(STATE_GAMEOVER);
}

/* The ball has reached the active disk while diving. */
static void smash_disk(void) {
  Disk *d = &s_disks[s_cur];
  bool fire = (s_ball_mode == BALL_FIREBALL);
  int k = sector_under_ball(d->angle_off + s_tower_rot);
  if (!d->finish && ((d->danger_mask >> k) & 1) && !fire) {   /* fireball: immune */
    player_die();
    return;
  }
  int ey = (s_current_tower_y >> FP_SHIFT) + s_cur * SPACING;
  spawn_disk_shards(s_cx, ey, d, fire);
  if (!fire) vibes_short_pulse();    /* fireball: no per-disk buzz, it fires ~every 2 frames */
  s_cur++;
  if (s_cur < NUM_DISKS) {
    if (pairs_for_disk(s_cur) > pairs_for_disk(s_cur - 1)) {
      s_toast_text = "MORE COLORS!";
      s_toast_frames = TOAST_FRAMES;
    } else if (s_cur == SPEED_DISK) {
      s_toast_text = "SPEED UP!";
      s_toast_frames = TOAST_FRAMES;
    }
  }
  s_target_tower_y -= FP(SPACING);   /* camera: bring the next disk up to the ball */
  s_ball_h += FP(SPACING);           /* ball is now one disk higher than its target */
  combo_on_smash();                  /* may start FIREBALL (its toast wins over the tier toast) */
  if (s_cur >= NUM_DISKS) {
    s_score += 25;
    vibes_long_pulse();
    end_run(STATE_VICTORY);
  }
}

static void update_title(void) {
  s_title_rot += 600;
  s_title_vy -= GRAVITY_FP;
  s_title_h += s_title_vy;
  if (s_title_h <= 0) {
    s_title_h = 0;
    s_title_vy = BOUNCE_V_FP;
  }
}

static void update_gameplay(void) {
  /* the tower spins on its own; it speeds up as you descend */
  s_tower_rot = ang(s_tower_rot + tower_spin(s_cur));

  /* ball */
  bool held = (s_btn_select && !s_select_lock) || s_tap_frames > 0;
  if (s_tap_frames > 0) s_tap_frames--;
  if (!held) combo_break();                      /* SELECT released: the chain is over */

  if (held) s_ball_vy = -SMASH_V_FP;
  else      s_ball_vy -= GRAVITY_FP;
  s_ball_h += s_ball_vy;

  if (held) {
    /* Leftover depth carries into the next disk, so the dive stays smooth and
     * can even break two thin disks in one frame. smash_disk() may end the run. */
    while (s_ball_h <= 0 && s_state == STATE_GAMEPLAY) smash_disk();
  } else if (s_ball_h <= 0) {                    /* normal landing: bounce */
    s_ball_h = 0;
    s_ball_vy = BOUNCE_V_FP;
  }

  /* Camera: fixed-point lerp of current -> target (smooth scroll, no wobble).
   * While SELECT is held the camera scrolls at EXACTLY the dive speed, so the
   * ball stays put on screen and the tower rushes past it. Without this the
   * lerp lagged behind the dive (then overtook it) and pulled the ball up and
   * down at every disk - it looked like bouncing. Released: plain lerp, and the
   * ball bounces as normal. */
  int32_t diff = s_target_tower_y - s_current_tower_y;      /* <= 0: target is above */
  if (diff > -64) {
    s_current_tower_y = s_target_tower_y;
  } else {
    int32_t step = (diff * CAM_LERP_FP) >> 8;               /* lerp step, negative   */
    if (held) step = -SMASH_V_FP;                           /* lock-step with dive   */
    if (step < diff) step = diff;                           /* never overshoot       */
    s_current_tower_y += step;
  }

  fireball_tick();
  if (s_toast_frames > 0) s_toast_frames--;
}

static void game_update(void) {
  s_frame++;
  s_state_frames++;
  switch (s_state) {
    case STATE_TITLE:    update_title();     break;
    case STATE_GAMEPLAY: update_gameplay();  break;
    case STATE_VICTORY:  spawn_confetti();   break;
    case STATE_GAMEOVER: break;
  }
  update_particles();
}

/* ============================== input ==================================== */
static void select_down(ClickRecognizerRef r, void *c) {
  (void)r; (void)c;
  s_btn_select = true;
  if (s_state == STATE_TITLE) {
    start_game();
  } else if ((s_state == STATE_GAMEOVER || s_state == STATE_VICTORY) && s_state_frames > 15) {
    start_game();
  }
}
static void select_up(ClickRecognizerRef r, void *c) {
  (void)r; (void)c;
  s_btn_select = false;
  s_select_lock = false;
}

static void click_config(void *ctx) {
  (void)ctx;
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_down, select_up, NULL);
}

/* wrist tap = short smash burst (the watch has no touchscreen) */
static void tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis; (void)direction;
  if (s_state == STATE_GAMEPLAY && !s_select_lock) s_tap_frames = TAP_SMASH_FRAMES;
}

/* ============================== lifecycle ================================ */
static void frame_cb(void *data) {
  (void)data;
  s_timer = app_timer_register(FRAME_MS, frame_cb, NULL);   /* re-arm first: steady cadence */
  game_update();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  layout_init(b);

  /* Backing arrays are only used to size the paths; points are rewritten
   * through path->points[] before every draw, so this works whether the SDK
   * copies or references the array. */
  static GPoint quad_init[4] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
  static GPoint tri_init[3]  = { {0, 0}, {1, 0}, {0, 1} };
  GPathInfo quad_info = { .num_points = 4, .points = quad_init };
  GPathInfo tri_info  = { .num_points = 3, .points = tri_init };
  s_path_quad = gpath_create(&quad_info);
  s_path_tri  = gpath_create(&tri_info);

  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);

  s_title_h = 0;
  s_title_vy = BOUNCE_V_FP;
  s_state = STATE_TITLE;
  s_state_frames = 0;

  s_timer = app_timer_register(FRAME_MS, frame_cb, NULL);
}

static void window_unload(Window *w) {
  (void)w;
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  if (s_path_quad) { gpath_destroy(s_path_quad); s_path_quad = NULL; }
  if (s_path_tri)  { gpath_destroy(s_path_tri);  s_path_tri  = NULL; }
  if (s_canvas)    { layer_destroy(s_canvas);    s_canvas    = NULL; }
}

static void init(void) {
  s_rng ^= (uint32_t)time(NULL);
  if (!s_rng) s_rng = 1;
  s_high = persist_exists(PERSIST_KEY_HIGH) ? persist_read_int(PERSIST_KEY_HIGH) : 0;

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  accel_tap_service_subscribe(tap_handler);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  window_destroy(s_window);          /* triggers window_unload -> frees paths + layer */
  s_window = NULL;
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
