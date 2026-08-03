#include <pebble.h>

// MoonPulse — 24h time, heart rate, hourly step chart, moon phase.
// Designed for emery (Pebble Time 2), 200x228, 64 colors.

#define MOON_CX 32
#define MOON_CY 40
#define MOON_R  19

#define CHART_LEFT 6
#define CHART_BAR_SLOT 7
#define CHART_BAR_WIDTH 6
#define CHART_WIDTH (24 * CHART_BAR_SLOT - 1)
#define LABEL_X (CHART_LEFT + CHART_WIDTH + 4)  // right-side scale labels

#define HR_BASELINE 182
#define HR_MAX_HEIGHT 28
#define HR_BUCKETS 144  // 10-minute resolution across 24h
#define HR_PER_HOUR (HR_BUCKETS / 24)

#define STEPS_BASELINE 220
#define STEPS_MAX_HEIGHT 30
#define STEPS_REF 1000  // "1k" reference line

// Reference new moon: 2000-01-06 18:14 UTC. Synodic month ~29.530589 days.
#define NEW_MOON_EPOCH 947182440
#define SYNODIC_SECONDS 2551443

static Window *s_window;
static Layer *s_canvas_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_bed_layer;
static TextLayer *s_wake_layer;
static TextLayer *s_bpm_layer;
static TextLayer *s_steps_layer;

static char s_time_buf[8];
static char s_date_buf[16];
static char s_bpm_buf[16];
static char s_steps_buf[20];

static uint32_t s_moon_phase_seconds;  // position within synodic month
static uint16_t s_hourly_steps[24];
static uint8_t s_hr_buckets[HR_BUCKETS];  // avg bpm per 10 min, 0 = no data
static int s_current_hour;

#define SLEEP_MAX_RESTFUL 8
#define SLEEP_STRIPE_LEFT 48
#define SLEEP_STRIPE_RIGHT 152
#define SLEEP_STRIPE_TOP 64
#define SLEEP_STRIPE_HEIGHT 7
static time_t s_sleep_start, s_sleep_end;  // last night, 0 = no data
static time_t s_restful_start[SLEEP_MAX_RESTFUL];
static time_t s_restful_end[SLEEP_MAX_RESTFUL];
static int s_restful_count;
static char s_bed_buf[8];
static char s_wake_buf[8];

static char s_phase_buf[24];

static int32_t isqrt32(int32_t v) {
  int32_t r = 0;
  while ((r + 1) * (r + 1) <= v) {
    r++;
  }
  return r;
}

// Countdown to the nearest of new/full moon, drawn on the moon disc:
// "-3.4" = days until it, "+3.4" = days past. The disc shows which one.
static void update_moon_phase(void) {
  time_t now = time(NULL);
  s_moon_phase_seconds = (uint32_t)((now - NEW_MOON_EPOCH) % SYNODIC_SECONDS);
  uint32_t p = s_moon_phase_seconds;

  uint32_t since_new = p;
  uint32_t until_new = SYNODIC_SECONDS - p;
  uint32_t dist_new = since_new < until_new ? since_new : until_new;
  uint32_t dist_full = p < SYNODIC_SECONDS / 2 ? SYNODIC_SECONDS / 2 - p
                                               : p - SYNODIC_SECONDS / 2;

  bool past;
  uint32_t dist;
  if (dist_full < dist_new) {
    past = p >= SYNODIC_SECONDS / 2;
    dist = dist_full;
  } else {
    past = since_new < until_new;
    dist = dist_new;
  }

  uint32_t tenths = (dist * 10 + 43200) / 86400;  // days, rounded to 0.1
  snprintf(s_phase_buf, sizeof(s_phase_buf), "%s%lu.%lu", past ? "+" : "-",
           (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
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

  // Countdown overlaid on the disc; black on the lit center, white on dark.
  graphics_context_set_text_color(ctx, c < 0 ? GColorBlack : GColorWhite);
  graphics_draw_text(ctx, s_phase_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(MOON_CX - 24, MOON_CY - 10, 48, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
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
                     GPoint(CHART_LEFT + CHART_WIDTH, baseline + 1));
}

static void draw_ref_line(GContext *ctx, int y) {
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(CHART_LEFT, y),
                     GPoint(CHART_LEFT + CHART_WIDTH, y));
}


static bool sleep_activity_cb(HealthActivity activity, time_t start, time_t end,
                              void *context) {
  if (s_sleep_start == 0 || start < s_sleep_start) {
    s_sleep_start = start;
  }
  if (end > s_sleep_end) {
    s_sleep_end = end;
  }
  if (activity == HealthActivityRestfulSleep &&
      s_restful_count < SLEEP_MAX_RESTFUL) {
    s_restful_start[s_restful_count] = start;
    s_restful_end[s_restful_count] = end;
    s_restful_count++;
  }
  return true;
}

static void update_sleep(void) {
  s_sleep_start = 0;
  s_sleep_end = 0;
  s_restful_count = 0;
  time_t now = time(NULL);
  health_service_activities_iterate(
      HealthActivitySleep | HealthActivityRestfulSleep, now - 20 * 3600, now,
      HealthIterationDirectionPast, sleep_activity_cb, NULL);

#ifdef DEMO_DATA
  struct tm *lt = localtime(&now);
  struct tm midnight = *lt;
  midnight.tm_hour = 0;
  midnight.tm_min = 0;
  midnight.tm_sec = 0;
  time_t day_start = mktime(&midnight);
  s_sleep_start = day_start - 25 * 60;         // 23:35
  s_sleep_end = day_start + 7 * 3600 + 10 * 60;  // 07:10
  s_restful_count = 2;
  s_restful_start[0] = day_start + 40 * 60;
  s_restful_end[0] = day_start + 2 * 3600;
  s_restful_start[1] = day_start + 4 * 3600;
  s_restful_end[1] = day_start + 5 * 3600 + 30 * 60;
#endif

  if (s_sleep_end > s_sleep_start) {
    struct tm *t = localtime(&s_sleep_start);
    strftime(s_bed_buf, sizeof(s_bed_buf), "%H:%M", t);
    t = localtime(&s_sleep_end);
    strftime(s_wake_buf, sizeof(s_wake_buf), "%H:%M", t);
  } else {
    s_bed_buf[0] = '\0';
    s_wake_buf[0] = '\0';
  }
}

// Last night as a horizontal stripe: bed time -> wake time, restful
// (deep) sleep segments brighter.
static void draw_sleep_stripe(GContext *ctx) {
  if (s_sleep_end <= s_sleep_start) {
    return;
  }
  int span = SLEEP_STRIPE_RIGHT - SLEEP_STRIPE_LEFT;
  int32_t dur = s_sleep_end - s_sleep_start;

  graphics_context_set_fill_color(ctx, GColorPurple);
  graphics_fill_rect(ctx, GRect(SLEEP_STRIPE_LEFT, SLEEP_STRIPE_TOP, span,
                                SLEEP_STRIPE_HEIGHT), 2, GCornersAll);

  graphics_context_set_fill_color(ctx, GColorVividViolet);
  for (int i = 0; i < s_restful_count; i++) {
    int x0 = SLEEP_STRIPE_LEFT + (int)((s_restful_start[i] - s_sleep_start) * span / dur);
    int x1 = SLEEP_STRIPE_LEFT + (int)((s_restful_end[i] - s_sleep_start) * span / dur);
    if (x1 > x0) {
      graphics_fill_rect(ctx, GRect(x0, SLEEP_STRIPE_TOP, x1 - x0,
                                    SLEEP_STRIPE_HEIGHT), 0, GCornerNone);
    }
  }
}

// Bare left footprint: ball of the foot tapering to the heel, big toe
// inside, smaller toes fanning out.
static void draw_foot(GContext *ctx, GPoint center) {
  graphics_context_set_fill_color(ctx, GColorTiffanyBlue);
  graphics_fill_circle(ctx, GPoint(center.x - 1, center.y - 2), 4);  // ball
  graphics_fill_circle(ctx, GPoint(center.x, center.y + 5), 3);     // heel
  graphics_fill_rect(ctx, GRect(center.x - 3, center.y - 2, 7, 7), 0, GCornerNone);
  graphics_fill_circle(ctx, GPoint(center.x - 4, center.y - 8), 2);  // big toe
  graphics_fill_circle(ctx, GPoint(center.x - 1, center.y - 9), 1);
  graphics_fill_circle(ctx, GPoint(center.x + 1, center.y - 8), 1);
  graphics_fill_circle(ctx, GPoint(center.x + 3, center.y - 7), 1);
}

static void draw_scale_text(GContext *ctx, const char *text, int top_y) {
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(LABEL_X, top_y, 200 - LABEL_X - 2, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_scale_label(GContext *ctx, int value, int top_y) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  draw_scale_text(ctx, buf, top_y);
}

static void draw_hr_chart(GContext *ctx) {
  int min_bpm = 250;
  int max_bpm = 0;
  for (int i = 0; i < HR_BUCKETS; i++) {
    if (s_hr_buckets[i] > 0) {  // 0 = no measurement, not a heart rate
      if (s_hr_buckets[i] < min_bpm) {
        min_bpm = s_hr_buckets[i];
      }
      if (s_hr_buckets[i] > max_bpm) {
        max_bpm = s_hr_buckets[i];
      }
    }
  }

  draw_chart_baseline(ctx, HR_BASELINE);
  if (max_bpm == 0) {
    return;  // no readings yet today
  }

  // Scale spans the day's actual range, padded so a flat line isn't a wall.
  int lo = min_bpm;
  int hi = max_bpm;
  if (hi - lo < 10) {
    lo -= (10 - (hi - lo)) / 2;
    hi = lo + 10;
  }

  draw_ref_line(
      ctx, HR_BASELINE - (max_bpm - lo) * HR_MAX_HEIGHT / (hi - lo));

  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_stroke_width(ctx, 1);
  bool has_prev = false;
  GPoint prev = GPointZero;
  for (int i = 0; i < HR_BUCKETS; i++) {
    if (s_hr_buckets[i] == 0) {
      has_prev = false;  // gap in readings breaks the line
      continue;
    }
    GPoint pt = GPoint(
        CHART_LEFT + i * CHART_WIDTH / (HR_BUCKETS - 1),
        HR_BASELINE - (s_hr_buckets[i] - lo) * HR_MAX_HEIGHT / (hi - lo));
    if (has_prev) {
      graphics_draw_line(ctx, prev, pt);
    } else {
      graphics_draw_pixel(ctx, pt);
    }
    prev = pt;
    has_prev = true;
  }

  draw_scale_label(ctx, max_bpm, HR_BASELINE - HR_MAX_HEIGHT - 6);
  draw_scale_label(ctx, min_bpm, HR_BASELINE - 12);
}

static void draw_step_chart(GContext *ctx) {
  uint32_t max_steps = STEPS_REF;  // floor keeps the 1k line on-chart
  for (int h = 0; h < 24; h++) {
    if (s_hourly_steps[h] > max_steps) {
      max_steps = s_hourly_steps[h];
    }
  }

  draw_chart_baseline(ctx, STEPS_BASELINE);

  int ref_y = STEPS_BASELINE - STEPS_REF * STEPS_MAX_HEIGHT / (int)max_steps;
  draw_ref_line(ctx, ref_y);  // 1k reference line
  draw_scale_text(ctx, "1k", ref_y - 8);

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
  draw_heart(ctx, GPoint(18, 134));
  draw_foot(ctx, GPoint(189, 134));
  draw_sleep_stripe(ctx);
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
    snprintf(buf, size, "%d,%03d", value / 1000, value % 1000);
  } else {
    snprintf(buf, size, "%d", value);
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
    memset(s_hr_buckets, 0, sizeof(s_hr_buckets));
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
    uint32_t hr_sum[HR_PER_HOUR] = {0};
    uint32_t hr_count[HR_PER_HOUR] = {0};
    for (uint32_t i = 0; i < n; i++) {
      if (minute_data[i].is_invalid) {
        continue;
      }
      step_sum += minute_data[i].steps;
      if (minute_data[i].heart_rate_bpm > 0 && i / 10 < HR_PER_HOUR) {
        hr_sum[i / 10] += minute_data[i].heart_rate_bpm;
        hr_count[i / 10]++;
      }
    }
    s_hourly_steps[h] = step_sum;
    for (int b = 0; b < HR_PER_HOUR; b++) {
      s_hr_buckets[h * HR_PER_HOUR + b] =
          hr_count[b] > 0 ? (uint8_t)(hr_sum[b] / hr_count[b]) : 0;
    }
  }

#ifdef DEMO_DATA  // synthetic data for emulator layout checks
  for (int i = 0; i < cur_hour * HR_PER_HOUR; i++) {
    int base = (i < 42) ? 52 : 68;                     // sleep vs day
    int wiggle = (i * 7) % 11 - 5;
    int spike = (i >= 78 && i < 84) ? 45 : 0;          // a workout
    s_hr_buckets[i] = (i % 17 == 0) ? 0 : base + wiggle + spike;
    s_hourly_steps[i / HR_PER_HOUR] = (i < 42) ? 0 : 300 + (i * 131) % 900;
  }
#endif
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time_and_date(tick_time);
  bool full = (tick_time->tm_min % 10 == 0) || (units_changed & DAY_UNIT);
  update_step_chart_data(full);
  if (full) {
    update_sleep();
  }
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
  s_bed_layer = make_text_layer(root, GRect(0, 58, 44, 16),
                                FONT_KEY_GOTHIC_14, GColorLightGray,
                                GTextAlignmentRight);
  s_wake_layer = make_text_layer(root, GRect(156, 58, 44, 16),
                                 FONT_KEY_GOTHIC_14, GColorLightGray,
                                 GTextAlignmentLeft);
  text_layer_set_text(s_bed_layer, s_bed_buf);
  text_layer_set_text(s_wake_layer, s_wake_buf);
  s_time_layer = make_text_layer(root, GRect(0, 72, bounds.size.w, 50),
                                 FONT_KEY_LECO_42_NUMBERS, GColorWhite,
                                 GTextAlignmentCenter);
  s_bpm_layer = make_text_layer(root, GRect(32, 118, 90, 26),
                                FONT_KEY_GOTHIC_24_BOLD, GColorWhite,
                                GTextAlignmentLeft);
  s_steps_layer = make_text_layer(root, GRect(84, 118, 96, 26),
                                  FONT_KEY_GOTHIC_24_BOLD, GColorWhite,
                                  GTextAlignmentRight);

  time_t now = time(NULL);
  update_time_and_date(localtime(&now));
  update_moon_phase();
  update_sleep();
  update_step_chart_data(true);
  update_health_displays();
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_bed_layer);
  text_layer_destroy(s_wake_layer);
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
