// Solar — a watchface that plots the sun's elevation over the day and
// shows a glowing sun at the current time. Built for emery (Pebble Time 2).
// The sun model and drawing live in shared/c/solar.c.
#include <pebble.h>
#include "../../shared/c/solar.h"

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

static Window *s_window;
static Layer *s_canvas;
static TextLayer *s_date_layer;
static TextLayer *s_time_layer;

static int s_now_min = 0;
#if DEMO_MODE
static int s_demo_min = 0;
#endif

static char s_date_buf[16];
static char s_time_buf[16];

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
#if DEMO_MODE
  solar_update_day(DEMO_YDAY, DEMO_TZ, bounds.size.w);
#else
  time_t now = time(NULL);
  solar_update_day(localtime(&now)->tm_yday, solar_utc_offset_minutes(), bounds.size.w);
#endif
  solar_draw(ctx, bounds, s_now_min, NULL, 0, NULL);
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

  solar_format_hm(s_time_buf, sizeof(s_time_buf), minute, false);
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

#if DEMO_MODE
static void demo_tap_handler(AccelAxisType axis, int32_t direction) {
  s_demo_min = (s_demo_min + DEMO_STEP_MIN) % 1440;
  update_time();
}
#endif

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  if (changed & HOUR_UNIT) solar_location_request();
  update_time();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  if (!DEMO_MODE && solar_location_from_inbox(iter)) layer_mark_dirty(s_canvas);
}

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
#if DEMO_MODE
  solar_location_set(DEMO_LAT, DEMO_LON, false);
  accel_tap_service_subscribe(demo_tap_handler);
#else
  solar_location_load();
#endif

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

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
