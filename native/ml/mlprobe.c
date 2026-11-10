// mlprobe: test tool for ml. Pairs if needed (prints the PIN), launches or resumes Desktop, receives video, writes
// Annex B, prints fps / bitrate / IDRs / arrival jitter. GPL-3.0 (links moonlight-common-c). Never sends input.
//
// usage: mlprobe [-H host] [-P http_port] [-d dir] [-t secs] [-o out.hevc] [-f fps] [-b kbps] [-p pin] [--reconnect] [--quit] [--touch-dry]
//        mlprobe --selftest | mlprobe [-H host] [-P port] [-d dir] --find
//   -t           seconds of video to receive (default 60; gives up after -t + 30 min); with --reconnect, wall-clock seconds
//   --reconnect  keep going through losses; print every state change and the video gap after each loss
//   --quit       end the Desktop app on the PC when done (default: leave it, so the next run resumes)
//   --touch-dry  feed a synthetic swipe through ml_touch every 2 s (logged only, input stays off)
//   -p           fixed pairing PIN (default: random), so the PIN can be given to the user before the run
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "Limelight.h"
#include "gs.h"
#include "ml.h"

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *out;
static int64_t first_rx, last_rx, prev_rx, lost_at, last_frame_at;
static long frames, idrs, bytes, losses, gaps;
static int *ivl, *lag, nivl, cap, live, after_loss, pin_given;
static double gap_sum, gap_max;
static volatile sig_atomic_t quit_sig;

static int video(void *ctx, const uint8_t *au, int len, uint32_t frame, int key, int codec, uint32_t host_pts_ms, int64_t rx_us) {
	int64_t now = now_us();
	pthread_mutex_lock(&mu);
	if (out) fwrite(au, 1, len, out);
	if (!frames) first_rx = rx_us;
	frames++, bytes += len, idrs += key;
	if (nivl == cap) cap = cap ? cap * 2 : 8192, ivl = realloc(ivl, cap * sizeof *ivl), lag = realloc(lag, cap * sizeof *lag);
	if (prev_rx) ivl[nivl] = (int)(rx_us - prev_rx), lag[nivl] = (int)(now - rx_us), nivl++;
	prev_rx = last_rx = rx_us;
	if (after_loss) {
		double gap = (now - last_frame_at) / 1000.0;
		printf("[%u] reconnect %ld: video gap %.0f ms (last frame before the loss to the first frame after; loss seen after %.0f ms)\n",
		       (unsigned)(now / 1000), losses, gap, (lost_at - last_frame_at) / 1000.0);
		gap_sum += gap, gap_max = gap > gap_max ? gap : gap_max, gaps++;
		after_loss = 0;
	} else if (last_frame_at && now - last_frame_at > 500000)  // a hiccup the session rode out
		printf("[%u] video stall %.0f ms (session survived)\n", (unsigned)(now / 1000), (now - last_frame_at) / 1000.0);
	last_frame_at = now;
	pthread_mutex_unlock(&mu);
	return 0;
}

static void st(void *ctx, int s, const char *detail) {
	pthread_mutex_lock(&mu);
	if (s == ML_PAIRING)  // the screen is the glass stand-in; a -p PIN is already known, so it isn't repeated
		printf("\n*** pairing: type %s into Sunshine's web UI (PIN page) ***\n\n", pin_given ? "the -p PIN" : detail);
	if (s == ML_LIVE) live = 1, prev_rx = 0;  // no interval across a gap
	if (s == ML_LOST && live) live = 0, losses++, lost_at = now_us(), after_loss = 1;
	pthread_mutex_unlock(&mu);
	fflush(stdout);
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static double pct(int *v, int n, double p) { return n ? v[(int)(p * (n - 1))] / 1000.0 : 0; }

#define CHECK(c) do { if (!(c)) { printf("selftest FAILED: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
static int selftest(void) {
	struct ml_contact a[] = {{1, 100, 200}}, b[] = {{1, 100, 200}, {2, 65535, 0}}, c[] = {{2, 65535, 10}};
	struct ml_touch_ev e[2 * ML_MAX_CONTACTS];
	char v[8];
	int n, bad = 0;
	n = ml_touch_diff(NULL, 0, a, 1, e);
	CHECK(n == 1 && e[0].type == LI_TOUCH_EVENT_DOWN && e[0].id == 1 && fabsf(e[0].x - 100 / 65535.f) < 1e-6f);
	CHECK(ml_touch_diff(a, 1, a, 1, e) == 0);  // held still: nothing
	n = ml_touch_diff(a, 1, b, 2, e);
	CHECK(n == 1 && e[0].type == LI_TOUCH_EVENT_DOWN && e[0].id == 2 && e[0].x == 1.f && e[0].y == 0.f);
	n = ml_touch_diff(b, 2, c, 1, e);
	CHECK(n == 2 && e[0].type == LI_TOUCH_EVENT_UP && e[0].id == 1 && e[1].type == LI_TOUCH_EVENT_MOVE && e[1].id == 2);
	n = ml_touch_diff(c, 1, NULL, 0, e);
	CHECK(n == 1 && e[0].type == LI_TOUCH_EVENT_UP && e[0].id == 2 && fabsf(e[0].y - 10 / 65535.f) < 1e-6f);
	n = ml_touch_diff(b, 2, NULL, -1, e);
	CHECK(n == 2 && e[0].type == LI_TOUCH_EVENT_CANCEL && e[1].type == LI_TOUCH_EVENT_CANCEL && e[1].id == 2);
	CHECK(ml_touch_diff(NULL, 0, NULL, 0, e) == 0);
	const char *x = "<root status_code=\"200\"><a>1</a><App><AppTitle>Desktop</AppTitle><ID>881448767</ID></App></root>";
	CHECK(gs_xml(x, "a", v, sizeof v) == 1 && !strcmp(v, "1"));
	CHECK(gs_xml(x, "ID", v, sizeof v) == -1);  // doesn't fit
	CHECK(gs_xml(x, "missing", v, sizeof v) == -1);
	printf("selftest %s\n", bad ? "FAILED" : "ok");
	return bad;
}

static void on_sig(int s) { quit_sig = 1; }

int main(int argc, char **argv) {
	struct ml_config cfg = {0};  // dir: ml default /data/local/tmp/tabd/ml unless -d
	const char *path = NULL;
	int secs = 60, reconnect = 0, quit = 0, touch = 0, find = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--selftest")) return selftest();
		else if (!strcmp(a, "--find")) find = 1;
		else if (!strcmp(a, "--reconnect")) reconnect = 1;
		else if (!strcmp(a, "--quit")) quit = 1;
		else if (!strcmp(a, "--touch-dry")) touch = 1;
		else if (v && !strcmp(a, "-H")) cfg.host = v, i++;
		else if (v && !strcmp(a, "-P")) cfg.port = atoi(v), i++;
		else if (v && !strcmp(a, "-d")) cfg.dir = v, i++;
		else if (v && !strcmp(a, "-t")) secs = atoi(v), i++;
		else if (v && !strcmp(a, "-o")) path = v, i++;
		else if (v && !strcmp(a, "-f")) cfg.fps = atoi(v), i++;
		else if (v && !strcmp(a, "-b")) cfg.bitrate_kbps = atoi(v), i++;
		else if (v && !strcmp(a, "-p")) cfg.pin = v, pin_given = 1, i++;
		else { fprintf(stderr, "usage: see the top of mlprobe.c\n"); return 2; }
	}
	if (find) {  // just locate the PC (saved address, else mDNS) and read serverinfo; no pairing, no stream
		struct gs *g = gs_open(cfg.dir ? cfg.dir : "/data/local/tmp/tabd/ml");
		struct gs_info si;
		if (!g || gs_find(g, cfg.host, cfg.port) || gs_serverinfo(g, &si)) { printf("find: %s\n", g ? gs_err(g) : "no client cert"); return 1; }
		printf("find: %s, Sunshine %s, paired %d, current app %d\n", gs_host(g), si.appversion, si.paired, si.currentgame);
		gs_close(g);
		return 0;
	}
	if (path && !(out = fopen(path, "wb"))) { perror(path); return 1; }
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct ml_callbacks cb = {.video = video, .state = st};
	int64_t t0 = now_us(), next_touch = t0;
	ml_start(&cfg, &cb);
	while (!quit_sig) {
		usleep(50000);
		pthread_mutex_lock(&mu);
		int64_t vid = frames ? last_rx - first_rx : 0;
		pthread_mutex_unlock(&mu);
		int64_t wall = now_us() - t0;
		if (reconnect ? wall >= secs * 1000000LL : vid >= secs * 1000000LL || wall >= (secs + 1800) * 1000000LL) break;  // +30 min for pairing
		if (touch && now_us() >= next_touch) {  // a 3-point swipe, then all up
			for (int k = 0; k <= 3; k++) {
				struct ml_contact ct = {7, (uint16_t)(16384 + k * 8192), 32768};
				ml_touch(&ct, k < 3 ? 1 : 0);
			}
			next_touch = now_us() + 2000000;
		}
	}
	ml_stop(quit);
	if (out) fclose(out);

	pthread_mutex_lock(&mu);
	double dur = (last_rx - first_rx) / 1e6;
	printf("frames %ld in %.1f s: %.2f fps, %.2f Mbps, %ld IDR\n", frames, dur, dur > 0 ? (frames - 1) / dur : 0,
	       dur > 0 ? bytes * 8 / dur / 1e6 : 0, idrs);
	if (nivl) {
		double m = 0, sd = 0;
		for (int i = 0; i < nivl; i++) m += ivl[i];
		m /= nivl;
		for (int i = 0; i < nivl; i++) sd += (ivl[i] - m) * (ivl[i] - m);
		sd = sqrt(sd / nivl);
		int late = 0;
		for (int i = 0; i < nivl; i++) late += ivl[i] > 2 * m;
		qsort(ivl, nivl, sizeof *ivl, cmp);
		qsort(lag, nivl, sizeof *lag, cmp);
		printf("arrival interval ms: mean %.2f sd %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f; %d intervals > 2x mean\n", m / 1000, sd / 1000,
		       pct(ivl, nivl, .5), pct(ivl, nivl, .95), pct(ivl, nivl, .99), pct(ivl, nivl, 1), late);
		printf("first packet -> callback ms: p50 %.2f p95 %.2f max %.2f\n", pct(lag, nivl, .5), pct(lag, nivl, .95), pct(lag, nivl, 1));
	}
	if (reconnect) printf("losses %ld, reconnects with video %ld, gap mean %.0f ms max %.0f ms\n", losses, gaps, gaps ? gap_sum / gaps : 0, gap_max);
	pthread_mutex_unlock(&mu);
	return frames ? 0 : 1;
}
