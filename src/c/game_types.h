#pragma once
/* ---------------------------------------------------------------------------
 *  Pebble vs Tile - shared types and tunables
 *  Everything is fixed-point / integer. No float, no double, no math.h.
 * ------------------------------------------------------------------------- */
#include <pebble.h>

/* ---------- application state machine ---------- */
typedef enum {
  STATE_TITLE = 0,
  STATE_GAMEPLAY,
  STATE_GAMEOVER,
  STATE_VICTORY
} AppState;

/* ---------- ball mode (combo / fireball state machine) ---------- */
typedef enum {
  BALL_NORMAL = 0,      /* meter charging: 0..FIREBALL_COMBO-1 consecutive smashes */
  BALL_FIREBALL         /* FIREBALL_FRAMES of immunity to Danger tiles            */
} BallMode;

/* ---------- fixed point (8.8) ---------- */
#define FP_SHIFT          8
#define FP(x)             ((int32_t)(x) * (1 << FP_SHIFT))

/* ---------- world / geometry tunables ---------- */
#define FRAME_MS          33      /* ~30 FPS                                    */
#define NUM_DISKS         36      /* last one is the checkered finish disk      */
#define NUM_SECTORS       12      /* wedges per disk (raise for rounder disks)  */
#define DISK_THICK        18      /* px height of a disk's side wall (was 7)     */
#define DISK_RIM          3       /* px darker shading band at the foot of a wall */
#define SPACING           DISK_THICK  /* FLUSH stack: the next disk starts exactly
                                         where this disk's wall ends (no gap)     */
#define SPINE_W           15      /* px width of the central cylinder (3 tiers) */
#define BALL_R            9       /* px radius of the Pebble                    */
#define BALL_Y            80      /* fixed screen Y of the ball at rest         */
#define TILT_PCT          30      /* ellipse height as % of its width           */
#define RING_PCT          62      /* ball radial position as % of disk radius   */
#define SEAM_OVERLAP      120     /* angle units of wedge overlap (hides seams) */
#define FRONT_ANGLE       (TRIG_MAX_ANGLE / 4)  /* angle that faces the viewer  */

/* ---------- physics (8.8 px per frame) ---------- */
#define GRAVITY_FP        154     /* ~0.60 px/frame^2                           */
#define BOUNCE_V_FP       1485    /* ~5.8 px/frame  -> ~28 px bounce            */
#define SMASH_V_FP        2048    /* 8 px/frame dive (~2.3 frames per disk)     */
#define CAM_LERP_FP       120     /* camera lerp factor, /256 (~0.47 / frame)   */
/* ---------- progression: colours first, speed last ----------
 * Level == disk index.  Tile colours live in main.c (safe_color / danger_color
 * and their wall shades). Palette "Sky, Gold & Spring":
 *   tier 1 (levels 1-8)    Sky blue     / Red
 *   tier 2 (levels 9-16)   + Gold       / Jazzberry
 *   tier 3 (levels 17+)    + Spring     / Maroon
 * Safe tiles are always the LIGHTER colours and Danger tiles the darker reds,
 * so a safe/danger mix-up is unlikely even when the screen washes out.        */
#define TIER2_DISK        8       /* from here: 2 colour pairs  (4 colours)       */
#define TIER3_DISK        16      /* from here: 3 colour pairs  (6 colours)       */
#define SPEED_DISK        24      /* from here: tower starts speeding up          */
#define SPIN_BASE         520     /* constant auto-spin until SPEED_DISK (angle/f) */
#define SPIN_PER_DISK     32      /* extra spin per disk beyond SPEED_DISK        */
#define SPIN_MAX          900     /* cap (~2.2 s per revolution)                  */
#define TOAST_FRAMES      45
#define DIVE_FRAMES_FP    ((SPACING * 256 * 256) / SMASH_V_FP)  /* 8.8 frames to dive one disk */

/* ---------- combo meter / fireball overdrive ---------- */
#define FIREBALL_COMBO    3                    /* consecutive smashes (SELECT held) to fill the meter */
#define FIREBALL_FRAMES   (3000 / FRAME_MS)    /* 3 seconds of overdrive                              */
#define FIREBALL_BLINK    24                   /* last frames: ball blinks back to normal as a warning */
#define FIRE_SHARDS       24                   /* shards per disk in fireball (normal mode: 12..16)   */

/* ---------- level generation ---------- */
#define MIN_SAFE_ARC      2                    /* every disk keeps >= this many ADJACENT safe sectors */
#define TAP_SMASH_FRAMES  10

/* ---------- particles ---------- */
#define MAX_PARTICLES     80
#define PART_GRAVITY_FP   64

/* ---------- persistence ---------- */
#define PERSIST_KEY_HIGH  1

/* =========================================================================
 *  BALL COLOURS  -  edit here, rebuild, look.
 *
 *  1) Pick a preset with BALL_STYLE (or build with  -DBALL_STYLE=2 ):
 *       0  Orange ball, red/orange fireball (the original look, no outline)
 *       1  Pearl: white ball, outlined fireball
 *       2  Steel: dark grey ball with white outline, outlined fireball
 *       3  Orange ball + outline, "white-hot" fireball (white core, no red border)
 *  2) Or change any single colour: every COL_* below is just a GColor.
 *     An outline is a ring of *_OUTLINE_PX pixels drawn around the ball
 *     (0 = off). It keeps a red fireball visible on a red tile.
 *
 *  Normal ball, back to front:  outline > RIM > BODY > MID > LIGHT > SPECULAR
 *  Fireball,    back to front:  outline > GLOW > BORDER > CORE > HOT
 * ========================================================================= */
#ifndef BALL_STYLE
#define BALL_STYLE 0
#endif

#if BALL_STYLE == 0              /* original */
  #define BALL_OUTLINE_PX    0
  #define COL_BALL_OUTLINE   GColorBlack
  #define COL_BALL_RIM       GColorWindsorTan
  #define COL_BALL_BODY      GColorOrange
  #define COL_BALL_MID       GColorChromeYellow
  #define COL_BALL_LIGHT     GColorYellow
  #define COL_BALL_SPECULAR  GColorWhite
  #define FIRE_OUTLINE_PX    0
  #define COL_FIRE_OUTLINE   GColorBlack
  #define COL_FIRE_GLOW      GColorOrange
  #define COL_FIRE_BORDER    GColorRed
  #define COL_FIRE_CORE      GColorYellow
  #define COL_FIRE_HOT       GColorWhite
#elif BALL_STYLE == 1            /* pearl */
  #define BALL_OUTLINE_PX    1
  #define COL_BALL_OUTLINE   GColorBlack
  #define COL_BALL_RIM       GColorLightGray
  #define COL_BALL_BODY      GColorWhite
  #define COL_BALL_MID       GColorWhite
  #define COL_BALL_LIGHT     GColorWhite
  #define COL_BALL_SPECULAR  GColorCeleste
  #define FIRE_OUTLINE_PX    1
  #define COL_FIRE_OUTLINE   GColorBlack
  #define COL_FIRE_GLOW      GColorOrange
  #define COL_FIRE_BORDER    GColorRed
  #define COL_FIRE_CORE      GColorYellow
  #define COL_FIRE_HOT       GColorWhite
#elif BALL_STYLE == 2            /* steel */
  #define BALL_OUTLINE_PX    1
  #define COL_BALL_OUTLINE   GColorWhite
  #define COL_BALL_RIM       GColorBlack
  #define COL_BALL_BODY      GColorDarkGray
  #define COL_BALL_MID       GColorDarkGray
  #define COL_BALL_LIGHT     GColorLightGray
  #define COL_BALL_SPECULAR  GColorWhite
  #define FIRE_OUTLINE_PX    1
  #define COL_FIRE_OUTLINE   GColorBlack
  #define COL_FIRE_GLOW      GColorOrange
  #define COL_FIRE_BORDER    GColorRed
  #define COL_FIRE_CORE      GColorYellow
  #define COL_FIRE_HOT       GColorWhite
#elif BALL_STYLE == 3            /* orange + white-hot fire */
  #define BALL_OUTLINE_PX    1
  #define COL_BALL_OUTLINE   GColorBlack
  #define COL_BALL_RIM       GColorWindsorTan
  #define COL_BALL_BODY      GColorOrange
  #define COL_BALL_MID       GColorChromeYellow
  #define COL_BALL_LIGHT     GColorYellow
  #define COL_BALL_SPECULAR  GColorWhite
  #define FIRE_OUTLINE_PX    1
  #define COL_FIRE_OUTLINE   GColorBlack
  #define COL_FIRE_GLOW      GColorOrange
  #define COL_FIRE_BORDER    GColorYellow
  #define COL_FIRE_CORE      GColorWhite
  #define COL_FIRE_HOT       GColorWhite
#else
  #error "BALL_STYLE must be 0, 1, 2 or 3"
#endif

/* ---------- data ---------- */
typedef struct {
  int32_t  angle_off;     /* per-disk base rotation (Pebble angle)             */
  uint16_t danger_mask;   /* bit k set -> sector k is a Danger "Tile"          */
  bool     finish;        /* checkered final disk, always safe                 */
  uint8_t  pairs;         /* colour pairs in use: 1 -> 2 colours, 2 -> 4, 3 -> 6 */
  uint8_t  seed;          /* rotates which colour each sector picks            */
} Disk;

typedef struct {
  int32_t x, y;           /* 8.8 screen position                               */
  int32_t vx, vy;         /* 8.8 px / frame                                    */
  int32_t angle;          /* Pebble angle                                      */
  int32_t spin;           /* angle units / frame                               */
  int32_t size;           /* 8.8 px "radius", shrinks every frame              */
  GColor  color;
  uint8_t life;           /* frames left, 0 = slot is free                     */
  uint8_t shrink;         /* size multiplier per frame, /256                   */
  uint8_t sides;          /* 3 = triangle shard, 4 = quad shard                */
} Particle;
