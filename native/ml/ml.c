// ml.c: the stream client on moonlight-common-c. See ml.h. GPL-3.0 (links moonlight-common-c).
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "Limelight.h"
#include "gs.h"
#include "ml.h"

static struct ml_config cfg;
static struct ml_callbacks cb;
static char dirbuf[200], hostbuf[80], namebuf[64], pinbuf[8];
static pthread_t thr;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;  // guards the flags below
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int running, stopping, quit_on_stop, connecting, ended, end_err, kicked;
static char stage_err[96];
static struct gs *cur_g;
static int codec;                                        // decoder thread
static uint8_t *au;
static int au_cap;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;  // guards live + the touch state
static int live, nprev, warned_touch;
static struct ml_contact prev[ML_MAX_CONTACTS];

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static void L(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	fprintf(stderr, "[%u] ml: ", (unsigned)(now_us() / 1000));
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
}
static void state(int s, const char *d) {
	static const char *names[] = {"?", "connecting", "pairing", "live", "lost"};
	if (s != ML_PAIRING) L("%s%s%s", names[s], d ? ": " : "", d ? d : "");  // the PIN is the caller's to show
	if (cb.state) cb.state(cb.ctx, s, d);
}

// ---------------------------------------------------------------- moonlight-common-c callbacks
static int dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) {
	codec = fmt & VIDEO_FORMAT_MASK_H265 ? ML_HEVC : ML_H264;
	L("video %dx%d@%d %s", w, h, rate, codec == ML_HEVC ? "HEVC" : "H.264");
	return 0;
}
static int dr_submit(PDECODE_UNIT du) {
	if (du->fullLength > au_cap) au = realloc(au, au_cap = du->fullLength * 2);
	int n = 0;
	for (PLENTRY e = du->bufferList; e; e = e->next) memcpy(au + n, e->data, e->length), n += e->length;
	int64_t rx = now_us() - (int64_t)(LiGetMicroseconds() - du->receiveTimeUs);
	return cb.video(cb.ctx, au, n, du->frameNumber, du->frameType == FRAME_TYPE_IDR, codec,
	                (uint32_t)(du->presentationTimeUs / 1000), rx) ? DR_NEED_IDR : DR_OK;
}
static void cl_started(void) {
	pthread_mutex_lock(&tmu);
	live = 1;
	nprev = 0;  // fingers already down re-DOWN in the new session
	pthread_mutex_unlock(&tmu);
}
static void cl_terminated(int err) {
	pthread_mutex_lock(&tmu);
	live = 0;
	pthread_mutex_unlock(&tmu);
	pthread_mutex_lock(&mu);
	ended = 1;
	end_err = err;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}
static void cl_stage_failed(int stage, int err) { snprintf(stage_err, sizeof stage_err, "%s failed (%d)", LiGetStageName(stage), err); }
static void cl_status(int s) { L("connection %s", s == CONN_STATUS_POOR ? "poor" : "ok"); }
static void cl_log(const char *fmt, ...) {  // moonlight-common-c's own lines carry their newlines
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}

// ---------------------------------------------------------------- session loop
// Waits for stop, the session's end (until_end), or ms (< 0: no limit). Returns 1 when stopping.
static int wait_for(int ms, int until_end) {
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += ms / 1000;
	t.tv_nsec += ms % 1000 * 1000000L;
	if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
	pthread_mutex_lock(&mu);
	int r = 0;
	while (!stopping && !(until_end && ended) && r != ETIMEDOUT) r = ms < 0 ? pthread_cond_wait(&cv, &mu) : pthread_cond_timedwait(&cv, &mu, &t);
	int s = stopping;
	pthread_mutex_unlock(&mu);
	return s;
}

static const char *end_reason(int e, char *b, int n) {
	switch (e) {
	case ML_ERROR_GRACEFUL_TERMINATION: return "the PC ended the session";
	case ML_ERROR_NO_VIDEO_TRAFFIC: return "no video from the PC";
	case ML_ERROR_NO_VIDEO_FRAME: return "no complete video frames";
	case ML_ERROR_UNEXPECTED_EARLY_TERMINATION: return "the PC ended the session early";
	default: snprintf(b, n, "connection lost (%d)", e); return b;
	}
}

// One streaming session. Returns 1 if it went live; why gets the reason it ended.
static int stream(struct gs *g, const struct gs_info *si, const unsigned char key[16], const unsigned char iv[16], const char *url,
                  char *why, int whysz) {
	SERVER_INFORMATION s;
	STREAM_CONFIGURATION c;
	CONNECTION_LISTENER_CALLBACKS l;
	DECODER_RENDERER_CALLBACKS d;
	LiInitializeServerInformation(&s);
	s.address = gs_host(g);
	s.serverInfoAppVersion = si->appversion;
	s.serverInfoGfeVersion = si->gfeversion;
	s.rtspSessionUrl = *url ? url : NULL;
	s.serverCodecModeSupport = si->codecs;
	LiInitializeStreamConfiguration(&c);
	c.width = cfg.width;
	c.height = cfg.height;
	c.fps = cfg.fps;
	c.bitrate = cfg.bitrate_kbps;
	c.packetSize = 1392;
	c.streamingRemotely = STREAM_CFG_LOCAL;
	c.audioConfiguration = AUDIO_CONFIGURATION_STEREO;  // negotiated, received and discarded (NULL audio callbacks)
	c.supportedVideoFormats = VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264;  // HEVC whenever the PC has it
	c.clientRefreshRateX100 = cfg.refresh_x100;
	c.colorSpace = cfg.colorspace == 601 ? COLORSPACE_REC_601 : COLORSPACE_REC_709;
	c.colorRange = COLOR_RANGE_LIMITED;
	c.encryptionFlags = ENCFLG_ALL;  // Sunshine decides (its LAN default is unencrypted video); input is always encrypted
	memcpy(c.remoteInputAesKey, key, 16);
	memcpy(c.remoteInputAesIv, iv, 16);
	LiInitializeConnectionCallbacks(&l);
	l.stageFailed = cl_stage_failed;
	l.connectionStarted = cl_started;
	l.connectionTerminated = cl_terminated;
	l.logMessage = cl_log;
	l.connectionStatusUpdate = cl_status;
	LiInitializeVideoCallbacks(&d);
	d.setup = dr_setup;
	d.submitDecodeUnit = dr_submit;

	pthread_mutex_lock(&mu);
	ended = kicked = 0;
	connecting = !stopping;
	stage_err[0] = 0;
	pthread_mutex_unlock(&mu);
	int r = connecting ? LiStartConnection(&s, &c, &l, &d, NULL, NULL, 0, NULL, 0) : -1;
	pthread_mutex_lock(&mu);
	connecting = 0;
	pthread_mutex_unlock(&mu);
	if (r) {
		snprintf(why, whysz, "%s", stage_err[0] ? stage_err : "connection failed");
		return 0;
	}
	gs_save_host(g);
	state(ML_LIVE, NULL);
	wait_for(-1, 1);
	pthread_mutex_lock(&tmu);
	live = 0;
	pthread_mutex_unlock(&tmu);
	LiStopConnection();
	char b[48];
	snprintf(why, whysz, "%s", kicked ? "reconnecting (network change)" : ended ? end_reason(end_err, b, sizeof b) : "stopped");
	return 1;
}

static void *session(void *arg) {
	struct gs *g = NULL;
	int backoff = 250;
	char why[200], url[256], pin[8] = "";
	int relaunch = cfg.relaunch;
	if (cfg.pin) snprintf(pin, sizeof pin, "%s", cfg.pin);
	while (!stopping) {
		struct gs_info si;
		int appid, went_live = 0;
		state(ML_CONNECTING, NULL);
		snprintf(why, sizeof why, "%s", "?");
		if (!g && (g = gs_open(cfg.dir))) pthread_mutex_lock(&mu), cur_g = g, pthread_mutex_unlock(&mu);
		if (!g) snprintf(why, sizeof why, "can't set up the client certificate in %s", cfg.dir);
		else if (gs_find(g, cfg.host, cfg.port) || gs_serverinfo(g, &si)) snprintf(why, sizeof why, "%s", gs_err(g));
		else if (!si.paired) {
			if (!*pin) {  // one PIN per process, so a PIN on glass (or relayed to the user) survives retries
				unsigned v = 0;
				if (getentropy(&v, sizeof v)) v = (unsigned)now_us();
				snprintf(pin, sizeof pin, "%04u", v % 10000);
			}
			L("not paired with %s: showing a PIN", gs_host(g));
			state(ML_PAIRING, pin);
			if (!gs_pair(g, pin, cfg.name)) { L("paired with %s", gs_host(g)); continue; }
			snprintf(why, sizeof why, "pairing: %s", gs_err(g));
		} else if ((appid = gs_app_id(g, "Desktop")) < 0) snprintf(why, sizeof why, "%s", gs_err(g));
		else if (si.currentgame && si.currentgame != appid)
			snprintf(why, sizeof why, "another app (id %d) is running on the PC; only Desktop is allowed", si.currentgame);
		else {
			unsigned char key[16], iv[16];
			if (getentropy(key, sizeof key) || getentropy(iv, sizeof iv)) abort();  // never stream with a guessable input key
			int rikeyid = (int)((uint32_t)iv[0] << 24 | iv[1] << 16 | iv[2] << 8 | iv[3]);
			if (relaunch && si.currentgame == appid) {  // Desktop only: any other app was refused above
				L("quitting the running Desktop first, so the new resolution is a fresh launch");
				if (gs_cancel(g)) L("quit: %s", gs_err(g));
				else si.currentgame = 0;
			}
			relaunch = 0;  // once per process; later reconnects resume as usual
			int resume = si.currentgame == appid;
			L("%s Desktop on %s (Sunshine %s)", resume ? "resuming" : "launching", gs_host(g), si.appversion);
			if (gs_launch(g, appid, resume, cfg.width, cfg.height, cfg.fps, SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(AUDIO_CONFIGURATION_STEREO),
			              cfg.sops, key, rikeyid, url, sizeof url))
				snprintf(why, sizeof why, "%s", gs_err(g));
			else
				went_live = stream(g, &si, key, iv, url, why, sizeof why);
			memset(key, 0, sizeof key);
		}
		if (stopping) break;
		state(ML_LOST, why);
		if (went_live) backoff = 250;
		wait_for(backoff, 0);
		backoff = backoff * 2 > 1000 ? 1000 : backoff * 2;  // capped low: video back within ~1 s of the link
	}
	if (quit_on_stop && g && gs_cancel(g)) L("quit: %s", gs_err(g));
	pthread_mutex_lock(&mu);
	cur_g = NULL;
	pthread_mutex_unlock(&mu);
	gs_close(g);
	return NULL;
}

// ---------------------------------------------------------------- public
int ml_start(const struct ml_config *c, const struct ml_callbacks *b) {
	pthread_mutex_lock(&mu);
	if (running || !b->video) { pthread_mutex_unlock(&mu); return -1; }
	running = 1;
	stopping = 0;
	pthread_mutex_unlock(&mu);
	cfg = *c;
	cb = *b;
	snprintf(dirbuf, sizeof dirbuf, "%s", c->dir ? c->dir : "/data/local/tmp/tabd/ml");
	snprintf(hostbuf, sizeof hostbuf, "%s", c->host ? c->host : "");
	snprintf(namebuf, sizeof namebuf, "%s", c->name && *c->name ? c->name : "tablet");
	snprintf(pinbuf, sizeof pinbuf, "%s", c->pin ? c->pin : "");
	cfg.dir = dirbuf;
	cfg.host = hostbuf;
	cfg.name = namebuf;
	cfg.pin = *pinbuf ? pinbuf : NULL;
	if (!cfg.width) cfg.width = 1920, cfg.height = 1080;
	if (!cfg.fps) cfg.fps = 60;
	if (!cfg.refresh_x100) cfg.refresh_x100 = 6000;
	if (!cfg.bitrate_kbps) cfg.bitrate_kbps = 18000;
	pthread_create(&thr, NULL, session, NULL);
	return 0;
}

void ml_stop(int quit) {
	pthread_mutex_lock(&mu);
	if (!running) { pthread_mutex_unlock(&mu); return; }
	stopping = 1;
	quit_on_stop = quit;
	pthread_cond_broadcast(&cv);
	if (connecting) LiInterruptConnection();
	else if (cur_g) gs_abort(cur_g);  // e.g. a pairing request waiting for the PIN
	pthread_mutex_unlock(&mu);
	pthread_join(thr, NULL);
	running = 0;
}

void ml_reconnect(void) {
	pthread_mutex_lock(&mu);
	if (running && !stopping) {
		kicked = ended = 1;  // a live stream() wakes and stops; the session loop reconnects after 250 ms
		pthread_cond_broadcast(&cv);
		if (connecting) LiInterruptConnection();
	}
	pthread_mutex_unlock(&mu);
}

void ml_request_idr(void) {
	pthread_mutex_lock(&tmu);
	if (live) LiRequestIdrFrame();
	pthread_mutex_unlock(&tmu);
}

int ml_touch_diff(const struct ml_contact *p, int np, const struct ml_contact *c, int nc, struct ml_touch_ev *out) {
	int n = 0;
	for (int i = 0; i < np; i++) {  // gone: UP (or CANCEL when the caller abandons every contact)
		int j = 0;
		while (j < nc && c[j].id != p[i].id) j++;
		if (nc < 0 || j == nc) out[n++] = (struct ml_touch_ev){nc < 0 ? LI_TOUCH_EVENT_CANCEL : LI_TOUCH_EVENT_UP, p[i].id, p[i].x / 65535.f, p[i].y / 65535.f};
	}
	for (int j = 0; j < nc; j++) {  // new: DOWN; moved: MOVE; still: nothing (Sunshine re-injects held contacts)
		int i = 0;
		while (i < np && p[i].id != c[j].id) i++;
		if (i == np || p[i].x != c[j].x || p[i].y != c[j].y)
			out[n++] = (struct ml_touch_ev){i == np ? LI_TOUCH_EVENT_DOWN : LI_TOUCH_EVENT_MOVE, c[j].id, c[j].x / 65535.f, c[j].y / 65535.f};
	}
	return n;
}

void ml_touch(const struct ml_contact *c, int n) {
	static const char *names[] = {"HOVER", "DOWN", "UP", "MOVE", "CANCEL"};
	struct ml_touch_ev ev[2 * ML_MAX_CONTACTS];
	if (n > ML_MAX_CONTACTS) n = ML_MAX_CONTACTS;
	pthread_mutex_lock(&tmu);
	if (!live) { nprev = 0; pthread_mutex_unlock(&tmu); return; }
	int k = ml_touch_diff(prev, nprev, c, n, ev);
	nprev = n < 0 ? 0 : n;
	memcpy(prev, c, nprev * sizeof *c);
	for (int i = 0; i < k; i++) {
		if (!cfg.input) {  // dry run: never reaches Windows
			if (ev[i].type != LI_TOUCH_EVENT_MOVE) L("touch (not sent) %s id %u %.4f,%.4f", names[ev[i].type], ev[i].id, ev[i].x, ev[i].y);
		} else if (LiSendTouchEvent(ev[i].type, ev[i].id, ev[i].x, ev[i].y, 0.f, 0.f, 0.f, LI_ROT_UNKNOWN) == LI_ERR_UNSUPPORTED && !warned_touch++)
			L("the PC doesn't support touch events");
	}
	pthread_mutex_unlock(&tmu);
}
