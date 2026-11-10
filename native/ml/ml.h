// ml.h: native Moonlight (GameStream/Sunshine) client for tabd. Independent of tabd internals.
// LICENCE: links moonlight-common-c (GPL-3.0), so any binary linking this (tabd included) is GPL-3.0.
//
// One session per process (moonlight-common-c keeps global state). ml_start() runs a background thread that
// finds the PC, pairs if needed, launches or resumes the "Desktop" app (never any other app), streams, and
// reconnects with backoff (0.25 s doubling to 1 s, 0.5 s connect timeout) until ml_stop().
#pragma once
#include <stdint.h>

#define ML_MAX_CONTACTS 10

enum ml_codec { ML_HEVC = 1, ML_H264 = 2 };

enum ml_state {
	ML_CONNECTING = 1,  // finding the PC / launching / RTSP handshake
	ML_PAIRING,         // detail = the 4-digit PIN the user types into Sunshine's web UI
	ML_LIVE,            // stream running
	ML_LOST,            // session ended or failed; detail = reason; the loop retries after a backoff
};

// One full-state touch frame contact: stable id while down,
// x,y normalised 0..65535 across the streamed picture, origin top-left.
struct ml_contact { uint8_t id; uint16_t x, y; };

struct ml_callbacks {
	// One complete Annex B access unit (VPS/SPS/PPS included on keyframes). au is valid only during the call.
	// frame = host frame number; key = IDR; codec = enum ml_codec;
	// host_pts_ms = host capture time in ms (Sunshine's presentation clock, epoch = first frame of this session).
	//   NOT the tablet's clock: time frames with rx_us.
	// rx_us = CLOCK_MONOTONIC us when the first packet of this frame arrived.
	// Return 0, or -1 to ask the host for an IDR (decoder lost sync).
	int (*video)(void *ctx, const uint8_t *au, int len, uint32_t frame, int key, int codec, uint32_t host_pts_ms, int64_t rx_us);
	// Optional. Called from the session thread; detail may be NULL.
	void (*state)(void *ctx, int state, const char *detail);
	void *ctx;
};

struct ml_config {
	const char *dir;      // client cert/key + saved PC address, created 0700 (files 0600). Default /data/local/tmp/tabd/ml
	const char *host;     // PC address. NULL = saved address from the last good session, then mDNS _nvstream._tcp
	int port;             // Sunshine HTTP port, default 47989 (HTTPS, RTSP and stream ports are discovered from it)
	int width, height;    // default 1920x1080
	int fps;              // default 60
	int refresh_x100;     // clientRefreshRateX100: the display's refresh x100, default 6000 (tabd passes its panel's)
	int bitrate_kbps;     // default 18000 (includes FEC), under common 20 Mbps decoder limits; raise it per device
	int colorspace;       // 709 (default) or 601; limited range either way
	int input;            // 0 = touch is diffed and logged, never sent to the PC (tabd: input = 0; mlprobe --touch-dry)
	int sops;             // 1 = ask Sunshine to apply width x height to its capture display (only safe when Sunshine's
	                      // output_name is the tablet's own virtual monitor); 0 (default) = never touch a PC display
	int relaunch;         // 1 = if Desktop is already running, quit it once before the first connect, so the session is
	                      // a fresh launch (Sunshine applies the new resolution and runs its prep commands only then)
	const char *name;     // client name in Sunshine's paired-devices list, default "tablet"
	const char *pin;      // NULL = a random 4-digit pairing PIN, one per process (ML_PAIRING); tests may fix it.
	                      // ml never logs the PIN; it only goes to the state callback.
};

// Starts the session thread. cfg and cb are copied. Returns 0, or -1 if already running.
int ml_start(const struct ml_config *cfg, const struct ml_callbacks *cb);
// Stops streaming and joins the thread. quit = 1 also ends the Desktop app on the host (/cancel), 0 leaves it
// running so the next ml_start() resumes it.
void ml_stop(int quit);
// Ask the host for a keyframe soon (decoder reset outside the video callback). No-op when not live.
void ml_request_idr(void);
// End the current session now and reconnect (a connect in progress is interrupted), e.g. when its network path just
// went away: the stream's own loss detection takes ~10 s. Thread-safe; a no-op when not running.
void ml_reconnect(void);
// Full-state touch frame from the touchscreen: all current contacts, n = 0 when every finger is up, n < 0 to
// abandon (CANCEL) every contact. Diffed into DOWN/MOVE/UP/CANCEL for Sunshine. Thread-safe; dropped when not live.
void ml_touch(const struct ml_contact *c, int n);

// --- below: for mlprobe and tests

// One touch event the differ produced (type = LI_TOUCH_EVENT_*; x,y 0..1 as sent to Sunshine).
struct ml_touch_ev { uint8_t type; uint8_t id; float x, y; };
// Pure diff of two touch frames into events (at most 2*ML_MAX_CONTACTS). Returns the event count.
int ml_touch_diff(const struct ml_contact *prev, int np, const struct ml_contact *cur, int nc, struct ml_touch_ev *out);
