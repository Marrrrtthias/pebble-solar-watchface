// Shared sun model and scene rendering for the Solar watchface and the
// Solar Times watchapp. Both projects compile this file (see their wscript).
#pragma once
#include <pebble.h>

#define SOLAR_MAX_W 200
#define HORIZON_Y 140

#define SOLAR_DEFAULT_LAT 5000  // degrees x100 (used until the phone reports)
#define SOLAR_DEFAULT_LON 800

typedef enum { MK_SUNRISE, MK_NOON, MK_NOW, MK_SUNSET } MarkerKind;

typedef struct {
  MarkerKind kind;
  int min;  // local minute of the day
} Marker;

// Local time offset from UTC in minutes.
int solar_utc_offset_minutes(void);

// Recompute the sun path and the day's events if the day, timezone, width
// or location changed since the last call. Call before drawing or reading
// the event times.
void solar_update_day(int yday, int tz, int width);
// Force a recompute on the next solar_update_day().
void solar_invalidate(void);

// Event times in local minutes of the day. Sunrise/sunset use the standard
// -0.833 deg threshold and are -1 on polar days/nights.
int solar_rise_min(void);
int solar_noon_min(void);
int solar_set_min(void);

// Draw the full scene: background, glow, horizon, sun path, the given
// markers, and the sun. With `sel`, the sun sits on that marker and its name
// is drawn next to it; otherwise the sun is at `sun_min`.
void solar_draw(GContext *ctx, GRect bounds, int sun_min, const Marker *markers, int n,
                const Marker *sel);

// "H:MM" in the user's 12/24 h style. `ampm` appends AM/PM in 12 h mode.
void solar_format_hm(char *buf, size_t n, int minute, bool ampm);

// Location (degrees x100). Each app persists its own copy.
void solar_location_load(void);
void solar_location_set(int32_t lat100, int32_t lon100, bool persist);
void solar_location_request(void);
// Applies LAT/LON from a phone message. Returns true if the location changed.
bool solar_location_from_inbox(DictionaryIterator *iter);
