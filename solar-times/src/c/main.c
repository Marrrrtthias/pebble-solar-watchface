// Solar Times — the interactive companion to the Solar watchface. Shows the
// sun path with markers for sunrise, solar noon, now and sunset; Up/Down move
// the sun between them. Open it from the watchface via Quick Launch.
// The sun model and drawing live in shared/c/solar.c.
#include <pebble.h>
#include "../../../shared/c/solar.h"

// Set to a minute of the day (0..1439) to freeze the clock for testing.
#define DEBUG_FAKE_MINUTE -1

#define IDLE_TIMEOUT_MS 30000

static Window *s_window;
static Layer *s_canvas;
static TextLayer *s_marker_time_layer;  // top left: selected marker's time
static TextLayer *s_delta_layer;        // below it: offset from now
static TextLayer *s_time_layer;         // top right: current time

static int s_now_min = 0;
static Marker s_markers[4];
static int s_marker_n = 0;
static int s_sel = 0;
static AppTimer *s_idle_timer = NULL;

static char s_marker_time_buf[16];
static char s_delta_buf[16];
static char s_time_buf[16];

static void update_day(void) {
  time_t now = time(NULL);
  solar_update_day(localtime(&now)->tm_yday, solar_utc_offset_minutes(),
                   layer_get_bounds(s_canvas).size.w);
}

// Collect the day's markers in chronological order. Keeps the selected
// marker kind (NOW on the first build).
static void build_markers(void) {
  update_day();
  MarkerKind keep = s_marker_n > 0 ? s_markers[s_sel].kind : MK_NOW;
  int n = 0;
  if (solar_rise_min() >= 0) s_markers[n++] = (Marker){MK_SUNRISE, solar_rise_min()};
  s_markers[n++] = (Marker){MK_NOON, solar_noon_min()};
  s_markers[n++] = (Marker){MK_NOW, s_now_min};
  if (solar_set_min() >= 0) s_markers[n++] = (Marker){MK_SUNSET, solar_set_min()};
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

static void canvas_update(Layer *layer, GContext *ctx) {
  update_day();
  solar_draw(ctx, layer_get_bounds(layer), s_now_min, s_markers, s_marker_n, &s_markers[s_sel]);
}

static void update(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  s_now_min = t->tm_hour * 60 + t->tm_min;
#if DEBUG_FAKE_MINUTE >= 0
  s_now_min = DEBUG_FAKE_MINUTE;
#endif

  build_markers();
  const Marker *m = &s_markers[s_sel];

  solar_format_hm(s_time_buf, sizeof(s_time_buf), s_now_min, false);
  text_layer_set_text(s_time_layer, s_time_buf);

  solar_format_hm(s_marker_time_buf, sizeof(s_marker_time_buf), m->min, true);
  text_layer_set_text(s_marker_time_layer, s_marker_time_buf);

  int d = m->min - s_now_min;
  int ad = d < 0 ? -d : d;
  snprintf(s_delta_buf, sizeof(s_delta_buf), "%c %02d:%02d h", d < 0 ? '-' : '+', ad / 60, ad % 60);
  text_layer_set_text(s_delta_layer, s_delta_buf);
  layer_set_hidden(text_layer_get_layer(s_delta_layer), m->kind == MK_NOW);

  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------- input

static void idle_timeout(void *data) {
  s_idle_timer = NULL;
  window_stack_pop_all(true);  // back to the watchface
}

static void reset_idle_timer(void) {
  if (!app_timer_reschedule(s_idle_timer, IDLE_TIMEOUT_MS)) {
    s_idle_timer = app_timer_register(IDLE_TIMEOUT_MS, idle_timeout, NULL);
  }
}

static void select_marker(int sel) {
  reset_idle_timer();
  if (sel < 0 || sel >= s_marker_n || sel == s_sel) return;
  s_sel = sel;
  update();
}

// Up = earlier, Down = later (like the timeline); Select = back to now.
static void up_click(ClickRecognizerRef rec, void *context) {
  select_marker(s_sel - 1);
}

static void down_click(ClickRecognizerRef rec, void *context) {
  select_marker(s_sel + 1);
}

static void select_click(ClickRecognizerRef rec, void *context) {
  for (int i = 0; i < s_marker_n; i++) {
    if (s_markers[i].kind == MK_NOW) select_marker(i);
  }
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

// ---------------------------------------------------------------- app

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  update();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  if (solar_location_from_inbox(iter)) update();
}

static TextLayer *make_text(GRect frame, const char *font, GColor color, GTextAlignment align) {
  TextLayer *l = text_layer_create(frame);
  text_layer_set_background_color(l, GColorClear);
  text_layer_set_text_color(l, color);
  text_layer_set_font(l, fonts_get_system_font(font));
  text_layer_set_text_alignment(l, align);
  return l;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int w = bounds.size.w;

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);

  s_marker_time_layer = make_text(GRect(5, 0, w - 10, 30), FONT_KEY_GOTHIC_24_BOLD, GColorWhite,
                                  GTextAlignmentLeft);
  s_delta_layer = make_text(GRect(5, 24, w - 10, 24), FONT_KEY_GOTHIC_18, GColorLightGray,
                            GTextAlignmentLeft);
  s_time_layer = make_text(GRect(0, 0, w - 5, 30), FONT_KEY_GOTHIC_24, GColorLightGray,
                           GTextAlignmentRight);
  layer_add_child(root, text_layer_get_layer(s_marker_time_layer));
  layer_add_child(root, text_layer_get_layer(s_delta_layer));
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  update();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_delta_layer);
  text_layer_destroy(s_marker_time_layer);
  layer_destroy(s_canvas);
}

static void init(void) {
  solar_location_load();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  s_idle_timer = app_timer_register(IDLE_TIMEOUT_MS, idle_timeout, NULL);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(64, 32);
}

static void deinit(void) {
  if (s_idle_timer) app_timer_cancel(s_idle_timer);
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
