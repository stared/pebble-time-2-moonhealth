#include <pebble.h>

// MoonPulse — 24h time, heart rate, hourly step chart, moon phase.
// Designed for emery (Pebble Time 2), 200x228, 64 colors.

#define MOON_CX 38
#define MOON_CY 44
#define MOON_R  26

#define CHART_LEFT 16
#define CHART_BAR_SLOT 7
#define CHART_BAR_WIDTH 5

#define HR_BASELINE 176
#define HR_MAX_HEIGHT 26
#define HR_FLOOR_BPM 40

#define STEPS_BASELINE 206
#define STEPS_MAX_HEIGHT 24

// Reference new moon: 2000-01-06 18:14 UTC. Synodic month ~29.530589 days.
#define NEW_MOON_EPOCH 947182440
#define SYNODIC_SECONDS 2551443

static Window *s_window;
static Layer *s_canvas_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_phase_layer;
static TextLayer *s_bpm_layer;
static TextLayer *s_steps_layer;

static char s_time_buf[8];
static char s_date_buf[16];
static char s_bpm_buf[16];
static char s_steps_buf[20];

static uint32_t s_moon_phase_seconds;  // position within synodic month
static uint16_t s_hourly_steps[24];
static uint8_t s_hourly_hr[24];  // avg bpm per hour, 0 = no data
static int s_current_hour;

static const char *PHASE_NAMES[8] = {
  "NEW MOON", "WAXING CRESCENT", "FIRST QUARTER", "WAXING GIBBOUS",
  "FULL MOON", "WANING GIBBOUS", "LAST QUARTER", "WANING CRESCENT",
};

static int32_t isqrt32(int32_t v) {
  int32_t r = 0;
  while ((r + 1) * (r + 1) <= v) {
    r++;
  }
  return r;
}

static void update_moon_phase(void) {
  time_t now = time(NULL);
  s_moon_phase_seconds = (uint32_t)((now - NEW_MOON_EPOCH) % SYNODIC_SECONDS);
  int idx = (int)(((uint64_t)s_moon_phase_seconds * 8 + SYNODIC_SECONDS / 2)
                  / SYNODIC_SECONDS) % 8;
  text_layer_set_text(s_phase_layer, PHASE_NAMES[idx]);
}

static void draw_moon(GContext *ctx) {
  int32_t angle = (int32_t)((uint64_t)s_moon_phase_seconds * TRIG_MAX_ANGLE
                            / SYNODIC_SECONDS);
  int32_t c = cos_lookup(angle);  // -TRIG_MAX_RATIO..TRIG_MAX_RATIO
  bool waxing = s_moon_phase_seconds < SYNODIC_SECONDS / 2;

  for (int dy = -MOON_R; dy <= MOON_R; dy++) {
    int32_t w = isqrt32(MOON_R * MOON_R - dy * dy);
    int y = MOON_CY + dy;
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_draw_line(ctx, GPoint(MOON_CX - w, y), GPoint(MOON_CX + w, y));

    int32_t xt = (w * c) / TRIG_MAX_RATIO;  // terminator offset
    int32_t lit_start = waxing ? xt : -w;
    int32_t lit_end = waxing ? w : -xt;
    if (lit_start < lit_end) {
      graphics_context_set_stroke_color(ctx, GColorPastelYellow);
      graphics_draw_line(ctx, GPoint(MOON_CX + lit_start, y),
                         GPoint(MOON_CX + lit_end, y));
    }
  }
}

static void draw_heart(GContext *ctx, GPoint center) {
  graphics_context_set_fill_color(ctx, GColorRed);
  graphics_fill_circle(ctx, GPoint(center.x - 4, center.y - 3), 5);
  graphics_fill_circle(ctx, GPoint(center.x + 4, center.y - 3), 5);
  GPathInfo tri_info = {
    .num_points = 3,
    .points = (GPoint[]) {{-8, 0}, {8, 0}, {0, 10}},
  };
  GPath *tri = gpath_create(&tri_info);
  gpath_move_to(tri, GPoint(center.x, center.y - 1));
  gpath_draw_filled(ctx, tri);
  gpath_destroy(tri);
}

static void draw_chart_baseline(GContext *ctx, int baseline) {
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(CHART_LEFT, baseline + 1),
                     GPoint(CHART_LEFT + 24 * CHART_BAR_SLOT - 2, baseline + 1));
}

static void draw_hr_chart(GContext *ctx) {
  uint32_t top_bpm = 120;  // scale headroom; grows if the day went higher
  for (int h = 0; h < 24; h++) {
    if (s_hourly_hr[h] > top_bpm) {
      top_bpm = s_hourly_hr[h];
    }
  }

  draw_chart_baseline(ctx, HR_BASELINE);

  for (int h = 0; h < 24; h++) {
    if (s_hourly_hr[h] < HR_FLOOR_BPM) {
      continue;  // 0 = no reading that hour
    }
    int x = CHART_LEFT + h * CHART_BAR_SLOT;
    int height = (s_hourly_hr[h] - HR_FLOOR_BPM) * HR_MAX_HEIGHT
                 / (int)(top_bpm - HR_FLOOR_BPM);
    if (height < 2) {
      height = 2;
    }
    graphics_context_set_fill_color(
        ctx, h == s_current_hour ? GColorWhite : GColorSunsetOrange);
    graphics_fill_rect(ctx, GRect(x, HR_BASELINE - height, CHART_BAR_WIDTH, height),
                       0, GCornerNone);
  }
}

static void draw_step_chart(GContext *ctx) {
  uint32_t max_steps = 500;  // floor so a quiet day doesn't look spiky
  for (int h = 0; h < 24; h++) {
    if (s_hourly_steps[h] > max_steps) {
      max_steps = s_hourly_steps[h];
    }
  }

  draw_chart_baseline(ctx, STEPS_BASELINE);

  for (int h = 0; h < 24; h++) {
    int x = CHART_LEFT + h * CHART_BAR_SLOT;
    int height = (int)((uint32_t)s_hourly_steps[h] * STEPS_MAX_HEIGHT / max_steps);
    if (h <= s_current_hour && height < 1) {
      height = 1;  // show a tick for elapsed hours with no steps
    }
    if (height > 0) {
      graphics_context_set_fill_color(
          ctx, h == s_current_hour ? GColorWhite : GColorTiffanyBlue);
      graphics_fill_rect(
          ctx, GRect(x, STEPS_BASELINE - height, CHART_BAR_WIDTH, height),
          0, GCornerNone);
    }
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  draw_moon(ctx);
  draw_heart(ctx, GPoint(74, 134));
  draw_hr_chart(ctx);
  draw_step_chart(ctx);
}

static void update_time_and_date(struct tm *tick_time) {
  strftime(s_time_buf, sizeof(s_time_buf), "%H:%M", tick_time);
  text_layer_set_text(s_time_layer, s_time_buf);
  strftime(s_date_buf, sizeof(s_date_buf), "%a %d %b", tick_time);
  for (char *p = s_date_buf; *p; p++) {
    if (*p >= 'a' && *p <= 'z') {
      *p -= 32;
    }
  }
  text_layer_set_text(s_date_layer, s_date_buf);
  s_current_hour = tick_time->tm_hour;
}

static void format_thousands(char *buf, size_t size, int value) {
  if (value >= 1000) {
    snprintf(buf, size, "%d,%03d steps", value / 1000, value % 1000);
  } else {
    snprintf(buf, size, "%d steps", value);
  }
}

static void update_health_displays(void) {
  HealthValue bpm = health_service_peek_current_value(HealthMetricHeartRateBPM);
  if (bpm > 0) {
    snprintf(s_bpm_buf, sizeof(s_bpm_buf), "%d bpm", (int)bpm);
  } else {
    snprintf(s_bpm_buf, sizeof(s_bpm_buf), "-- bpm");
  }
  text_layer_set_text(s_bpm_layer, s_bpm_buf);

  HealthValue steps = health_service_sum_today(HealthMetricStepCount);
  format_thousands(s_steps_buf, sizeof(s_steps_buf), (int)steps);
  text_layer_set_text(s_steps_layer, s_steps_buf);
}

// Sum minute-level step history into hourly buckets. full=false only
// refreshes the current hour (cheap); full=true rebuilds the whole day.
static void update_step_chart_data(bool full) {
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  int cur_hour = lt->tm_hour;
  struct tm midnight = *lt;
  midnight.tm_hour = 0;
  midnight.tm_min = 0;
  midnight.tm_sec = 0;
  time_t day_start = mktime(&midnight);

  if (full) {
    memset(s_hourly_steps, 0, sizeof(s_hourly_steps));
    memset(s_hourly_hr, 0, sizeof(s_hourly_hr));
  }

  static HealthMinuteData minute_data[60];
  int first_hour = full ? 0 : cur_hour;
  for (int h = first_hour; h <= cur_hour; h++) {
    time_t start = day_start + (time_t)h * 3600;
    time_t end = start + 3600;
    if (start >= now) {
      break;
    }
    if (end > now) {
      end = now;
    }
    uint32_t n = health_service_get_minute_history(minute_data, 60, &start, &end);
    uint32_t step_sum = 0;
    uint32_t hr_sum = 0;
    uint32_t hr_count = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (minute_data[i].is_invalid) {
        continue;
      }
      step_sum += minute_data[i].steps;
      if (minute_data[i].heart_rate_bpm > 0) {
        hr_sum += minute_data[i].heart_rate_bpm;
        hr_count++;
      }
    }
    s_hourly_steps[h] = step_sum;
    s_hourly_hr[h] = hr_count > 0 ? (uint8_t)(hr_sum / hr_count) : 0;
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time_and_date(tick_time);
  bool full = (tick_time->tm_min % 10 == 0) || (units_changed & DAY_UNIT);
  update_step_chart_data(full);
  update_moon_phase();
  update_health_displays();
  layer_mark_dirty(s_canvas_layer);
}

static void health_handler(HealthEventType event, void *context) {
  switch (event) {
    case HealthEventHeartRateUpdate:
    case HealthEventMovementUpdate:
      update_health_displays();
      update_step_chart_data(false);
      break;
    case HealthEventSignificantUpdate:
      update_health_displays();
      update_step_chart_data(true);
      break;
    default:
      break;
  }
  layer_mark_dirty(s_canvas_layer);
}

static TextLayer *make_text_layer(Layer *root, GRect frame, const char *font_key,
                                  GColor color, GTextAlignment align) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, color);
  text_layer_set_font(layer, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(layer, align);
  layer_add_child(root, text_layer_get_layer(layer));
  return layer;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(root, s_canvas_layer);

  s_date_layer = make_text_layer(root, GRect(72, 22, 122, 24),
                                 FONT_KEY_GOTHIC_18_BOLD, GColorWhite,
                                 GTextAlignmentRight);
  s_phase_layer = make_text_layer(root, GRect(72, 46, 122, 20),
                                  FONT_KEY_GOTHIC_14, GColorLightGray,
                                  GTextAlignmentRight);
  s_time_layer = make_text_layer(root, GRect(0, 68, bounds.size.w, 50),
                                 FONT_KEY_LECO_42_NUMBERS, GColorWhite,
                                 GTextAlignmentCenter);
  s_bpm_layer = make_text_layer(root, GRect(88, 120, 100, 26),
                                FONT_KEY_GOTHIC_24_BOLD, GColorWhite,
                                GTextAlignmentLeft);
  s_steps_layer = make_text_layer(root, GRect(0, 208, bounds.size.w, 18),
                                  FONT_KEY_GOTHIC_14, GColorLightGray,
                                  GTextAlignmentCenter);

  time_t now = time(NULL);
  update_time_and_date(localtime(&now));
  update_moon_phase();
  update_step_chart_data(true);
  update_health_displays();
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_phase_layer);
  text_layer_destroy(s_bpm_layer);
  text_layer_destroy(s_steps_layer);
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  health_service_events_subscribe(health_handler, NULL);
}

static void deinit(void) {
  health_service_events_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
