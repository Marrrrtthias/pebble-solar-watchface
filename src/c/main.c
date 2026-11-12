// Solar — a watchface that plots the sun's elevation over the day and
// shows a glowing sun at the current time. Built for emery (Pebble Time 2).
#include <pebble.h>

// Set to a minute of the day (0..1439) to freeze the clock for testing.
#define DEBUG_FAKE_MINUTE -1

// Demo mode for store assets: fixed date and location, each accelerometer
// tap (`pebble emu-tap`) advances the clock by DEMO_STEP_MIN minutes.
#define DEMO_MODE 0
#define DEMO_LAT 5252      // Berlin, degrees x100
#define DEMO_LON 1340
#define DEMO_TZ 120        // CEST, minutes east of UTC
#define DEMO_YDAY 248      // 6 September (non-leap year)
#define DEMO_DATE "SUN 6"
#define DEMO_STEP_MIN 20

// Emulator testing of the touch UI (the emulator cannot inject touches):
// `pebble emu-tap --direction x+` = swipe left (next marker),
// `x-` = swipe right (previous marker), `z+`/`z-` = tap (toggle).
#define DEBUG_TOUCH_VIA_TAP 0

#if DEMO_MODE && DEBUG_TOUCH_VIA_TAP
#error "DEMO_MODE and DEBUG_TOUCH_VIA_TAP both use the accelerometer tap"
#endif

#define PERSIST_LAT 1
#define PERSIST_LON 2
#define DEFAULT_LAT 5000  // degrees x100 (used until the phone reports)
#define DEFAULT_LON 800

#define HORIZON_Y 140
#define CURVE_AMP 62
#define SUN_R 7

// Fixed point: 1.0 == ONE (same scale as TRIG_MAX_RATIO)
#define ONE 65536
#define SIN_10_DEG 11380
#define SIN_8_DEG 9121
#define SIN_25_DEG 27697
#define SIN_SUNRISE (-953)  // sin(-0.833 deg): refraction + solar disc

#define IDLE_TIMEOUT_MS 30000

static Window *s_window;
static Layer *s_canvas;
static TextLayer *s_date_layer;
static TextLayer *s_time_layer;
static TextLayer *s_delta_layer;

static int32_t s_lat = DEFAULT_LAT;
static int32_t s_lon = DEFAULT_LON;
static int s_curve_day = -1;  // tm_yday the curve was computed for
static int16_t s_curve_y[200];
static int32_t s_curve_s[200];  // sin(elevation), Q16
static int s_curve_w = 0;
static int s_now_min = 0;
#if DEMO_MODE
static int s_demo_min = 0;
#endif

// Per-day sun constants (set by compute_curve, used by sin_elev_at)
static int32_t s_ss, s_cc, s_offs100;
// Marker times in local minutes of the day; -1 if the event doesn't happen
static int s_rise_min = -1, s_noon_min = 720, s_set_min = -1;

// Interactive mode
typedef enum { MK_SUNRISE, MK_NOON, MK_NOW, MK_SUNSET } MarkerKind;
typedef struct {
  MarkerKind kind;
  int min;
} Marker;
static bool s_interactive = false;
static Marker s_markers[4];
static int s_marker_n = 0;
static int s_sel = 0;
static AppTimer *s_idle_timer = NULL;

static char s_date_buf[16];
static char s_time_buf[16];
static char s_delta_buf[16];

// ---------------------------------------------------------------- palettes

static const uint8_t DAY_RAMP[] = {
  GColorBlackARGB8, GColorOxfordBlueARGB8, GColorDukeBlueARGB8,
  GColorCobaltBlueARGB8, GColorBlueMoonARGB8, GColorPictonBlueARGB8,
  GColorCelesteARGB8, GColorWhiteARGB8,
};
static const uint8_t WARM_RAMP[] = {
  GColorBlackARGB8, GColorBulgarianRoseARGB8, GColorDarkCandyAppleRedARGB8,
  GColorFollyARGB8, GColorBrilliantRoseARGB8, GColorMelonARGB8,
  GColorWhiteARGB8,
};
static const uint8_t COOL_RAMP[] = {
  GColorBlackARGB8, GColorOxfordBlueARGB8, GColorLibertyARGB8,
  GColorBabyBlueEyesARGB8, GColorWhiteARGB8,
};
#define RAMP(r) r, (int)(sizeof(r) / sizeof(r[0]))

// 4x4 Bayer matrix, values 0..15
static const uint8_t BAYER[4][4] = {
  {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

// Intensity in 0..ONE -> dithered ramp color
static inline uint8_t ramp_color(const uint8_t *ramp, int n, int32_t intensity, int x, int y) {
  if (intensity <= 0) return GColorBlackARGB8;
  if (intensity >= ONE) return ramp[n - 1];
  int32_t level = (intensity * (n - 1)) >> 12;  // in 1/16 steps
  int base = level >> 4;
  int frac = level & 15;
  if (frac > BAYER[y & 3][x & 3] && base < n - 1) base++;
  return ramp[base];
}

static inline int32_t clamp_one(int32_t v) {
  return v < 0 ? 0 : (v > ONE ? ONE : v);
}

// Smooth bell: (1 - d2/r2)^3 in Q16, 0 outside r
static inline int32_t bell(int32_t d2, int32_t r2) {
  if (d2 >= r2) return 0;
  int32_t t = ONE - (int32_t)(((int64_t)d2 << 16) / r2);
  int32_t t2 = (int32_t)(((int64_t)t * t) >> 16);
  return (int32_t)(((int64_t)t2 * t) >> 16);
}

// ---------------------------------------------------------------- sun math

static int32_t deg100_to_angle(int32_t deg100) {
  return (int32_t)(((int64_t)deg100 * TRIG_MAX_ANGLE) / 36000);
}

static int __attribute__((unused)) utc_offset_minutes(void) {
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  struct tm ut = *gmtime(&now);
  int off = (lt.tm_hour * 60 + lt.tm_min) - (ut.tm_hour * 60 + ut.tm_min);
  while (off > 840) off -= 1440;
  while (off < -720) off += 1440;
  return off;
}

// sin(elevation) in Q16 at local time `local100` (minutes x100).
// Uses the per-day constants set by compute_curve.
static int32_t sin_elev_at(int32_t local100) {
  int32_t solar100 = local100 + s_offs100;
  int32_t h = (int32_t)(((int64_t)(solar100 - 72000) * TRIG_MAX_ANGLE) / 144000);
  return s_ss + (int32_t)(((int64_t)s_cc * cos_lookup(h)) >> 16);
}

// Bisect a SIN_SUNRISE crossing between lo and hi (minutes x100).
// Returns the rounded local minute.
static int find_crossing(int32_t lo, int32_t hi, bool rising) {
  while (hi - lo > 10) {
    int32_t mid = (lo + hi) / 2;
    bool below = sin_elev_at(mid) < SIN_SUNRISE;
    if (below == rising) lo = mid;
    else hi = mid;
  }
  int m = ((lo + hi) / 2 + 50) / 100;
  return m >= 1440 ? 1439 : m;
}

// Compute sin(elevation) samples across the local day.
static void compute_curve(int yday, int width) {
  int32_t lat = deg100_to_angle(s_lat);
  int32_t sin_lat = sin_lookup(lat), cos_lat = cos_lookup(lat);

  // Declination: -23.44 deg * cos(360/365 * (doy + 10))
  int32_t a = (int32_t)(((int64_t)(yday + 1 + 10) * TRIG_MAX_ANGLE) / 365);
  int32_t decl = -(int32_t)(((int64_t)deg100_to_angle(2344) * cos_lookup(a)) / TRIG_MAX_RATIO);
  int32_t sin_d = sin_lookup(decl), cos_d = cos_lookup(decl);

  // Equation of time (minutes x100): 9.87 sin 2B - 7.53 cos B - 1.5 sin B
  int32_t b = (int32_t)(((int64_t)(yday + 1 - 81) * TRIG_MAX_ANGLE) / 365);
  int32_t eot100 = (987 * sin_lookup(2 * b) - 753 * cos_lookup(b) - 150 * sin_lookup(b)) / TRIG_MAX_RATIO;

#if DEMO_MODE
  int tz = DEMO_TZ;
#else
  int tz = utc_offset_minutes();
#endif
  s_ss = (int32_t)(((int64_t)sin_lat * sin_d) >> 16);
  s_cc = (int32_t)(((int64_t)cos_lat * cos_d) >> 16);
  s_offs100 = -tz * 100 + s_lon * 4 + eot100;  // local -> solar time

  for (int x = 0; x < width; x++) {
    int32_t s = sin_elev_at((int32_t)x * 144000 / width);
    s_curve_s[x] = s;
    s_curve_y[x] = HORIZON_Y - (int16_t)((s * CURVE_AMP) >> 16);
  }
  s_curve_w = width;
  s_curve_day = yday;

  // Solar noon: hour angle 0, i.e. solar time 12:00.
  int32_t noon100 = 72000 - s_offs100;
  while (noon100 < 0) noon100 += 144000;
  while (noon100 >= 144000) noon100 -= 144000;
  s_noon_min = (noon100 + 50) / 100;
  if (s_noon_min >= 1440) s_noon_min = 1439;

  // Sunrise / sunset: first threshold crossing upward / downward.
  s_rise_min = s_set_min = -1;
  // x == width closes the gap between the last column and midnight.
  for (int x = 1; x <= width; x++) {
    int32_t s0 = s_curve_s[x - 1];
    int32_t s1 = x < width ? s_curve_s[x] : sin_elev_at(144000);
    bool up = s0 < SIN_SUNRISE && s1 >= SIN_SUNRISE;
    bool down = s0 >= SIN_SUNRISE && s1 < SIN_SUNRISE;
    if ((up && s_rise_min < 0) || (down && s_set_min < 0)) {
      int m = find_crossing((int32_t)(x - 1) * 144000 / width, (int32_t)x * 144000 / width, up);
      if (up) s_rise_min = m;
      else s_set_min = m;
    }
  }
}

static int current_yday(void) {
#if DEMO_MODE
  return DEMO_YDAY;
#else
  time_t now = time(NULL);
  return localtime(&now)->tm_yday;
#endif
}

static int canvas_width(void) {
  int w = layer_get_bounds(s_canvas).size.w;
  return w > 200 ? 200 : w;
}

static void ensure_curve(void) {
  int yday = current_yday();
  int w = canvas_width();
  if (s_curve_day != yday || s_curve_w != w) compute_curve(yday, w);
}

// ---------------------------------------------------------------- drawing

static void draw_glow(GContext *ctx, GRect bounds, GPoint sun, int32_t s) {
  // Day glow strength rises from 0 at sin(el)=0 to full at 25 deg.
  int32_t day_k = clamp_one((int32_t)(((int64_t)s << 16) / SIN_25_DEG));
  // Twilight strength peaks at the horizon, gone at -8 and +10 deg.
  int32_t tw_k = s < 0 ? clamp_one(ONE + (int32_t)(((int64_t)s << 16) / SIN_8_DEG))
                       : clamp_one(ONE - (int32_t)(((int64_t)s << 16) / SIN_10_DEG));
  // Let twilight take over near the horizon.
  day_k = (int32_t)(((int64_t)day_k * (ONE - tw_k)) >> 16);
  if (day_k == 0 && tw_k == 0) return;

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  const int32_t DAY_R = 90, TW_RX = 85, TW_RY = 75, CORE_R = 22;
  int w = bounds.size.w, hgt = bounds.size.h;

  int x0 = 0, x1 = w - 1, y0 = 0, y1 = hgt - 1;
  if (tw_k == 0) {
    x0 = sun.x - DAY_R; x1 = sun.x + DAY_R;
    y0 = sun.y - DAY_R; y1 = sun.y + DAY_R;
  } else if (day_k == 0) {
    x0 = sun.x - TW_RX; x1 = sun.x + TW_RX;
    y0 = HORIZON_Y - TW_RY; y1 = HORIZON_Y;
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w - 1) x1 = w - 1;
  if (y1 > hgt - 1) y1 = hgt - 1;

  for (int y = y0; y <= y1; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int rx0 = x0 < row.min_x ? row.min_x : x0;
    int rx1 = x1 > row.max_x ? row.max_x : x1;
    int32_t dy = y - sun.y;
    int32_t above = HORIZON_Y - y;  // > 0 above the horizon

    // Vertical part of the twilight field (only above the horizon)
    int32_t tw_v = 0, warm = 0;
    if (tw_k && above > 0) {
      tw_v = bell(above * above, TW_RY * TW_RY);
      // Warm (red/pink) band near the horizon, cooler higher up
      warm = clamp_one(ONE - ((above - 5) << 16) / 8);
    }

    for (int x = rx0; x <= rx1; x++) {
      int32_t dx = x - sun.x;
      int32_t d2 = dx * dx + dy * dy;
      int32_t day_i = 0, tw_i = 0;

      if (day_k) {
        day_i = (int32_t)(((int64_t)bell(d2, DAY_R * DAY_R) * day_k * 7) >> 19);
        day_i += bell(d2, (SUN_R + 6) * (SUN_R + 6)) >> 1;
      }
      if (tw_v) {
        int32_t h = bell(dx * dx, TW_RX * TW_RX);
        tw_i = (int32_t)(((int64_t)h * tw_v) >> 16);
        tw_i = (int32_t)(((int64_t)tw_i * tw_k) >> 16);
        tw_i = (tw_i * 5) >> 3;
        tw_i += (int32_t)(((int64_t)bell(d2, CORE_R * CORE_R) * tw_k) >> 16) >> 1;
      }

      if (above <= 0 && tw_k) {
        // Ground blocks the glow as the sun approaches the horizon.
        day_i = (int32_t)(((int64_t)day_i * clamp_one(ONE - 3 * tw_k)) >> 16);
      }
      int32_t total = day_i + tw_i;
      uint8_t c;
      if (tw_i == 0 || day_i > 2 * tw_i) {
        c = ramp_color(RAMP(DAY_RAMP), total, x, y);
      } else if ((warm >> 12) > BAYER[y & 3][x & 3]) {
        c = ramp_color(RAMP(WARM_RAMP), total, x, y);
      } else {
        c = ramp_color(RAMP(COOL_RAMP), total, x, y);
      }
      row.data[x] = c;
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

// ---------------------------------------------------------------- markers

static int min_to_x(int min, int w) {
  int x = min * w / 1440;
  return x >= w ? w - 1 : (x < 0 ? 0 : x);
}

// Where a marker sits on the drawn path. Sunrise and sunset sit on the
// horizon line, the others on the curve.
static GPoint marker_point(const Marker *m, int w) {
  int x = min_to_x(m->min, w);
  bool on_horizon = m->kind == MK_SUNRISE || m->kind == MK_SUNSET;
  return GPoint(x, on_horizon ? HORIZON_Y : s_curve_y[x]);
}

static const char *marker_name(MarkerKind kind) {
  switch (kind) {
    case MK_SUNRISE: return "SUNRISE";
    case MK_NOON: return "NOON";
    case MK_SUNSET: return "SUNSET";
    default: return "NOW";
  }
}

static void draw_markers(GContext *ctx, int w) {
  for (int i = 0; i < s_marker_n; i++) {
    const Marker *m = &s_markers[i];
    GPoint p = marker_point(m, w);
    switch (m->kind) {
      case MK_SUNRISE:
      case MK_SUNSET:
        graphics_context_set_stroke_color(ctx, GColorWhite);
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_line(ctx, GPoint(p.x, p.y - 4), GPoint(p.x, p.y + 4));
        break;
      case MK_NOON:
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, p, 2);
        break;
      case MK_NOW:
        graphics_context_set_stroke_color(ctx, GColorLightGray);
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_circle(ctx, p, 3);
        break;
    }
  }
}

// Name of the selected marker, above the sun (below it if that would
// collide with the text at the top).
static void draw_label(GContext *ctx, const Marker *m, GPoint sun, int w) {
  const int LW = 64, LH = 18, TOP_LIMIT = 50;
  int x = sun.x - LW / 2;
  if (x < 2) x = 2;
  if (x > w - LW - 2) x = w - LW - 2;
  int y = sun.y - SUN_R - LH - 4;
  if (y < TOP_LIMIT) y = sun.y + SUN_R + 2;
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, marker_name(m->kind), fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(x, y, LW, LH), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int w = canvas_width();

  ensure_curve();

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  const Marker *sel = s_interactive ? &s_markers[s_sel] : NULL;
  int sx = min_to_x(sel ? sel->min : s_now_min, w);
  GPoint sun = sel ? marker_point(sel, w) : GPoint(sx, s_curve_y[sx]);
  int32_t s = s_curve_s[sx];

  draw_glow(ctx, bounds, sun, s);

  // Horizon
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(0, HORIZON_Y), GPoint(bounds.size.w - 1, HORIZON_Y));

  // Sun path
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  for (int x = 1; x < w; x++) {
    graphics_draw_line(ctx, GPoint(x - 1, s_curve_y[x - 1]), GPoint(x, s_curve_y[x]));
  }

  if (s_interactive) draw_markers(ctx, w);

  // Sun
  if (sun.y <= HORIZON_Y) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, sun, SUN_R);
  } else {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, sun, SUN_R);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, sun, SUN_R - 1);
  }

  if (sel) draw_label(ctx, sel, sun, w);
}

// ---------------------------------------------------------------- time

// `ampm` appends AM/PM in 12-hour mode (the clock itself omits it).
static void format_hm(char *buf, size_t n, int minute, bool ampm) {
  int h = minute / 60;
  if (clock_is_24h_style()) {
    snprintf(buf, n, "%d:%02d", h, minute % 60);
    return;
  }
  int h12 = h % 12 == 0 ? 12 : h % 12;
  snprintf(buf, n, ampm ? "%d:%02d %s" : "%d:%02d", h12, minute % 60, h < 12 ? "AM" : "PM");
}

// Collect the available markers in chronological order. Keeps the
// selected marker kind (NOW on a fresh build).
static void build_markers(void) {
  ensure_curve();
  MarkerKind keep = s_marker_n > 0 ? s_markers[s_sel].kind : MK_NOW;
  int n = 0;
  if (s_rise_min >= 0) s_markers[n++] = (Marker){MK_SUNRISE, s_rise_min};
  s_markers[n++] = (Marker){MK_NOON, s_noon_min};
  s_markers[n++] = (Marker){MK_NOW, s_now_min};
  if (s_set_min >= 0) s_markers[n++] = (Marker){MK_SUNSET, s_set_min};
  // Insertion sort by minute (n <= 4)
  for (int i = 1; i < n; i++) {
    Marker m = s_markers[i];
    int j = i - 1;
    while (j >= 0 && s_markers[j].min > m.min) {
      s_markers[j + 1] = s_markers[j];
      j--;
    }
    s_markers[j + 1] = m;
  }
  s_marker_n = n;
  s_sel = 0;
  for (int i = 0; i < n; i++) {
    if (s_markers[i].kind == keep) s_sel = i;
  }
}

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  int minute = t->tm_hour * 60 + t->tm_min;
#if DEBUG_FAKE_MINUTE >= 0
  minute = DEBUG_FAKE_MINUTE;
#endif
#if DEMO_MODE
  minute = s_demo_min;
#endif
  s_now_min = minute;

  format_hm(s_time_buf, sizeof(s_time_buf), minute, false);
  text_layer_set_text(s_time_layer, s_time_buf);

  if (s_interactive) {
    build_markers();
    const Marker *m = &s_markers[s_sel];
    format_hm(s_date_buf, sizeof(s_date_buf), m->min, true);
    int d = m->min - s_now_min;
    int ad = d < 0 ? -d : d;
    snprintf(s_delta_buf, sizeof(s_delta_buf), "%c %02d:%02d h", d < 0 ? '-' : '+', ad / 60, ad % 60);
    text_layer_set_text(s_delta_layer, s_delta_buf);
    layer_set_hidden(text_layer_get_layer(s_delta_layer), m->kind == MK_NOW);
  } else {
    strftime(s_date_buf, sizeof(s_date_buf), "%a %d", t);
    for (char *p = s_date_buf; *p; p++) {
      if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
#if DEMO_MODE
    snprintf(s_date_buf, sizeof(s_date_buf), "%s", DEMO_DATE);
#endif
  }
  text_layer_set_text(s_date_layer, s_date_buf);

  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------- interactive

static void idle_timeout(void *data);

static void set_layout(bool interactive) {
  GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  if (interactive) {
    layer_set_frame(text_layer_get_layer(s_time_layer), GRect(0, 0, bounds.size.w - 5, 30));
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
    text_layer_set_text_color(s_time_layer, GColorLightGray);
  } else {
    layer_set_frame(text_layer_get_layer(s_time_layer), GRect(0, 22, bounds.size.w - 5, 52));
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_LIGHT));
    text_layer_set_text_color(s_time_layer, GColorWhite);
  }
  layer_set_hidden(text_layer_get_layer(s_delta_layer), !interactive);
}

static void enter_interactive(void) {
  s_interactive = true;
  s_marker_n = 0;  // build_markers then selects NOW
  set_layout(true);
  s_idle_timer = app_timer_register(IDLE_TIMEOUT_MS, idle_timeout, NULL);
  update_time();
}

static void exit_interactive(void) {
  if (!s_interactive) return;
  s_interactive = false;
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
  set_layout(false);
  update_time();
}

static void idle_timeout(void *data) {
  s_idle_timer = NULL;
  exit_interactive();
}

static void reset_idle_timer(void) {
  if (s_idle_timer) app_timer_reschedule(s_idle_timer, IDLE_TIMEOUT_MS);
}

// Don't gate entry on TouchEvent.non_navigational: on the real watch that
// blocked every tap (it seems to be set for all idle-watchface contact).
static void handle_tap(void) {
  if (s_interactive) exit_interactive();
  else enter_interactive();
}

// step > 0: later marker (swipe left), step < 0: earlier (swipe right)
static void handle_swipe(int step) {
  if (!s_interactive) return;
  reset_idle_timer();
  int sel = s_sel + step;
  if (sel < 0 || sel >= s_marker_n) return;
  s_sel = sel;
  update_time();
}

// Raw touch stream: logging only. On the real watch it delivered nothing to
// the watchface, so gestures come from the recognizers below.
static void touch_log_handler(const TouchEvent *e, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "raw touch type=%d %d,%d nn=%d", e->type, e->x, e->y,
          e->non_navigational);
}

static void tap_recognized(const Recognizer *r, RecognizerEvent event) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "tap recognizer event=%d", event);
  if (event == RecognizerEvent_Completed) handle_tap();
}

static void swipe_recognized(const Recognizer *r, RecognizerEvent event) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "swipe recognizer event=%d", event);
  if (event != RecognizerEvent_Completed) return;
  SwipeDirection dir = swipe_recognizer_get_direction(r);
  if (dir == SwipeDirection_Left) handle_swipe(1);
  else if (dir == SwipeDirection_Right) handle_swipe(-1);
}

#if DEBUG_TOUCH_VIA_TAP
static void debug_tap_handler(AccelAxisType axis, int32_t direction) {
  if (axis == ACCEL_AXIS_X) handle_swipe(direction > 0 ? 1 : -1);
  else if (axis == ACCEL_AXIS_Z) handle_tap();
}
#endif

static void request_location(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_LOC, 1);
    app_message_outbox_send();
  }
}

#if DEMO_MODE
static void demo_tap_handler(AccelAxisType axis, int32_t direction) {
  s_demo_min = (s_demo_min + DEMO_STEP_MIN) % 1440;
  update_time();
}
#endif

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  if (changed & HOUR_UNIT) {
    s_curve_day = -1;  // pick up DST / timezone changes
    request_location();
  }
  if ((changed & DAY_UNIT) && s_interactive) {
    exit_interactive();  // also updates the time
    return;
  }
  update_time();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *lat = dict_find(iter, MESSAGE_KEY_LAT);
  Tuple *lon = dict_find(iter, MESSAGE_KEY_LON);
  if (lat && lon && !DEMO_MODE) {
    s_lat = lat->value->int32;
    s_lon = lon->value->int32;
    persist_write_int(PERSIST_LAT, s_lat);
    persist_write_int(PERSIST_LON, s_lon);
    APP_LOG(APP_LOG_LEVEL_INFO, "Location: %ld, %ld", (long)s_lat, (long)s_lon);
    s_curve_day = -1;
    update_time();  // rebuilds markers and text in interactive mode
  }
}

// ---------------------------------------------------------------- window

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);

  s_date_layer = text_layer_create(GRect(5, 0, bounds.size.w - 10, 30));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  s_time_layer = text_layer_create(GRect(0, 22, bounds.size.w - 5, 52));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_LIGHT));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentRight);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_delta_layer = text_layer_create(GRect(5, 24, bounds.size.w - 10, 24));
  text_layer_set_background_color(s_delta_layer, GColorClear);
  text_layer_set_text_color(s_delta_layer, GColorLightGray);
  text_layer_set_font(s_delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  layer_set_hidden(text_layer_get_layer(s_delta_layer), true);
  layer_add_child(root, text_layer_get_layer(s_delta_layer));

  // Take touches away from the system's button emulation and hand them to
  // our recognizers. The window owns and destroys them.
  window_set_touch_bridge_disabled(window, true);
  window_attach_recognizer(window, tap_recognizer_create(tap_recognized, NULL));
  window_attach_recognizer(window, swipe_recognizer_create(swipe_recognized, NULL,
                                                           SwipeDirection_Left | SwipeDirection_Right));

  update_time();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_delta_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_canvas);
}

static void init(void) {
  if (persist_exists(PERSIST_LAT) && persist_exists(PERSIST_LON)) {
    s_lat = persist_read_int(PERSIST_LAT);
    s_lon = persist_read_int(PERSIST_LON);
  }

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

#if DEMO_MODE
  s_lat = DEMO_LAT;
  s_lon = DEMO_LON;
  accel_tap_service_subscribe(demo_tap_handler);
#endif

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  touch_service_subscribe(touch_log_handler, NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "touch enabled: %d", touch_service_is_enabled());
#if DEBUG_TOUCH_VIA_TAP
  accel_tap_service_subscribe(debug_tap_handler);
#endif

  app_message_register_inbox_received(inbox_received);
  app_message_open(64, 32);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  touch_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
