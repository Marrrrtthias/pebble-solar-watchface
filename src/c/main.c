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

static Window *s_window;
static Layer *s_canvas;
static TextLayer *s_date_layer;
static TextLayer *s_time_layer;

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

static char s_date_buf[16];
static char s_time_buf[16];

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
  int32_t ss = (int32_t)(((int64_t)sin_lat * sin_d) >> 16);
  int32_t cc = (int32_t)(((int64_t)cos_lat * cos_d) >> 16);

  for (int x = 0; x < width; x++) {
    // local minutes x100 at this column
    int32_t local100 = (int32_t)x * 144000 / width;
    int32_t solar100 = local100 - tz * 100 + s_lon * 4 + eot100;
    int32_t h = (int32_t)(((int64_t)(solar100 - 72000) * TRIG_MAX_ANGLE) / 144000);
    int32_t s = ss + (int32_t)(((int64_t)cc * cos_lookup(h)) >> 16);
    s_curve_s[x] = s;
    s_curve_y[x] = HORIZON_Y - (int16_t)((s * CURVE_AMP) >> 16);
  }
  s_curve_w = width;
  s_curve_day = yday;
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

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int w = bounds.size.w;
  if (w > 200) w = 200;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
#if DEMO_MODE
  (void)t;
  int yday = DEMO_YDAY;
#else
  int yday = t->tm_yday;
#endif
  if (s_curve_day != yday || s_curve_w != w) compute_curve(yday, w);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int sx = s_now_min * w / 1440;
  if (sx >= w) sx = w - 1;
  GPoint sun = GPoint(sx, s_curve_y[sx]);
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
}

// ---------------------------------------------------------------- time

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

  if (clock_is_24h_style()) {
    snprintf(s_time_buf, sizeof(s_time_buf), "%d:%02d", minute / 60, minute % 60);
  } else {
    int h12 = (minute / 60) % 12;
    snprintf(s_time_buf, sizeof(s_time_buf), "%d:%02d", h12 == 0 ? 12 : h12, minute % 60);
  }
  text_layer_set_text(s_time_layer, s_time_buf);

  strftime(s_date_buf, sizeof(s_date_buf), "%a %d", t);
  for (char *p = s_date_buf; *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
#if DEMO_MODE
  snprintf(s_date_buf, sizeof(s_date_buf), "%s", DEMO_DATE);
#endif
  text_layer_set_text(s_date_layer, s_date_buf);

  layer_mark_dirty(s_canvas);
}

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
    layer_mark_dirty(s_canvas);
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

  update_time();
}

static void window_unload(Window *window) {
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

  app_message_register_inbox_received(inbox_received);
  app_message_open(64, 32);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
