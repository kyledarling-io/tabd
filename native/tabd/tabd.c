// tabd: the tablet half of a PC touchscreen monitor. Sunshine streams a virtual monitor through libml (Moonlight,
// native/ml) -> Rockchip MPP hardware decode -> zero-copy NV12 scanout on the panel (eDP, DSI, LVDS or DPI) via atomic
// KMS (RGA turns the frames when the tablet isn't in the panel's native orientation); touchscreen -> Sunshine. Without a live
// picture the ARGB overlay shows the glass states (connecting, PIN, no signal, asleep). On-glass brightness: a finger
// resting in the picture's bottom-right corner plus a two-finger vertical swipe (gest_feed).
// Runs as root with Android's display stack stopped (see run.sh). Linking libml (moonlight-common-c) makes this binary
// GPL-3.0.
//
// Threads: main = housekeeping (watchdog tick, log, config, IDR requests, stall recovery); out = decoder output;
//          disp = KMS flips, vblank events and glass states; touch = evdev reader; libml's own threads feed video_in.
//
// Everything tabd keeps on the tablet is in /data/local/tmp/tabd (D below): tabd.conf, ml/ (the pairing identity),
// state/ (brightness, orient), log/.
// Settings: D/tabd.conf, "key = value" lines, # comments, read once at start (restart tabd after editing it; see
// native/tabd/tabd.conf.example). A line tabd doesn't understand stops it (exit 2), like an unknown option, so a
// wrong setting shows at once and boot.sh keeps retrying.
//   pc = auto | HOST[:PORT] | :PORT   the Sunshine PC (auto: the saved address, then mDNS; port default 47989)
//   orient = 0|90|180|270|auto        the tablet's orientation, locked at start (default 0; auto: the accelerometer,
//                                     else the last one used); portrait asks Sunshine for 1080x1920, RGA turns frames
//   input = 1|0                       touches reach Windows (default 1; 0 logs them only)
//   bitrate = KBPS                    the stream's bitrate (default libml's, 18000)
//   connector = eDP|DSI|LVDS|DPI      the panel's connector type (default: the first connected one of these)
//   touch_driver = NAME, touch_parent = PATH-END   narrow the touchscreen search (default: by capabilities)
//   backlight = NAME                  /sys/class/backlight/NAME (default: the first there, by name)
//   links, composer, fixed_mac, adb_allow, wifi_country, wifi_freq, wifi_bssid: the scripts' keys, accepted here
// usage: tabd [--conf FILE] [--log FILE] [--margin MS] [-d N] [--fault put|hang]
//        tabd --selftest | --detect | --glass-test
//   --conf      another settings file (tests)
//   --margin    commit held frames this long before the vblank (default 1.0 ms)
//   --log       write and rotate our own log (1 MB, one old copy)
//   -d          dump decoded frame N to D/frame.nv12 (visible size NV12, no stride padding)
//   --fault     simulated decoder failures, to prove the recovery paths
//   --selftest  check the orientation and brightness-gesture logic and exit
//   --detect    print the orientation the accelerometer reads and exit
//   --glass-test  render the glass states into D/glass-*.argb and exit
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include "ml/ml.h"
#include "rk_mpi.h"

#define P(x) ((uint64_t)(uintptr_t)(x))
#define ADD(x, n) __atomic_fetch_add(&(x), (n), __ATOMIC_RELAXED)
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static uint32_t now_ms(void) { return (uint32_t)(now_us() / 1000); }
#define L(...) do { fprintf(stderr, "[%u] ", now_ms()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LR(...) do { static uint32_t lr_; if (now_ms() - lr_ >= 1000) { lr_ = now_ms(); L(__VA_ARGS__); } } while (0)  // 1/s: a repeating error must not fill /data

static void rt(int prio) { struct sched_param sp = {.sched_priority = prio}; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }

// ---------------------------------------------------------------- shared state (mu guards everything below unless atomic)
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int opt_dump = -1;
static int need_key = 1;       // atomic: waiting for a keyframe
static uint32_t key_frame;     // frame number of the last keyframe sent to the decoder
static uint32_t shown_total, dropped_total;
static uint32_t keyreqs;
static int64_t stall_since;    // atomic: first decoder input with no output since (0 = none)
static uint32_t reset_ms;      // when the stream last (re)started, until its first frame is on glass (0 = logged)
static uint32_t main_tick, disp_tick;  // atomic: last loop pass of the main and display threads (self-watchdog)
static const char *opt_fault = "";     // --fault put|hang: simulated decoder failures, to prove the recovery paths
static const char *opt_log;            // --log FILE: tabd writes and rotates its own log (1 MB, one old copy)
#define D "/data/local/tmp/tabd"
static const char *ml_host;            // pc: NULL = saved address, then mDNS
static int ml_port;                    // 0 = libml's default (47989)
static int ml_live;                    // atomic: the libml session is streaming (IDR requests only then)
static int ml_relaunch;                // the screen shape changed since the last run: quit Desktop once, launch fresh
static int opt_input = 1;              // input: touches reach Windows (libml cfg.input)
static int opt_bitrate;                // bitrate: kbps, 0 = libml's default
static char opt_orient[8] = "0";       // orient: 0|90|180|270|auto, locked at start
static uint32_t last_flip_ms;          // atomic: when a frame last reached the glass (ghost-touch guard)
static int ml_cancelled;               // atomic: contacts were cancelled because the glass went stale
static int panel_wmm = 305, panel_hmm = 171;  // the panel's size in mm: DRM's, else the mode at ~160 dpi (here 1920x1080's)
static int refresh_x100;               // the panel's refresh from its mode, x100 (Sunshine's clientRefreshRateX100)
static int opt_connector;              // connector: only this DRM_MODE_CONNECTOR_* type (0: eDP, DSI, LVDS or DPI)
static char opt_touch_driver[32], opt_touch_parent[48], opt_backlight[32];
static char bl_power[112], bl_bright[112], bl_maxp[112];  // the backlight's sysfs files (bl_find)

// 1-second windows for the status line
#define RN 256
struct ring { uint32_t t[RN]; int32_t v[RN]; unsigned n; };
static struct ring r_flip, r_rxg, r_rga;  // flips; rxg: first packet -> glass (ms); rga: time to turn a frame (us)
static void rpush(struct ring *r, int32_t v) { r->t[r->n % RN] = now_ms(); r->v[r->n % RN] = v; r->n++; }
static int rwin(const struct ring *r, uint32_t now, int32_t *out) {
	int k = 0;
	for (unsigned i = 0; i < RN && i < r->n; i++) {
		unsigned j = (r->n - 1 - i) % RN;
		if (now - r->t[j] > 1000) break;
		out[k++] = r->v[j];
	}
	return k;
}
static int cmp32(const void *a, const void *b) { return *(const int32_t *)a - *(const int32_t *)b; }
static uint16_t c16(int32_t v) { return v < 1 ? 1 : v > 65535 ? 65535 : v; }
static void pct(const struct ring *r, uint32_t now, uint16_t *p50, uint16_t *p95) {
	int32_t v[RN]; int n = rwin(r, now, v);
	*p50 = *p95 = 0;
	if (!n) return;
	qsort(v, n, sizeof *v, cmp32);
	*p50 = c16(v[n / 2]); *p95 = c16(v[n * 95 / 100]);
}

// ---------------------------------------------------------------- MPP (vendor /vendor/lib64/libmpp.so via dlopen)
#define MPPF(X) \
	X(MPP_RET, mpp_create, (MppCtx *, MppApi **)) X(MPP_RET, mpp_init, (MppCtx, MppCtxType, MppCodingType)) \
	X(MPP_RET, mpp_destroy, (MppCtx)) X(MPP_RET, mpp_packet_init, (MppPacket *, void *, size_t)) \
	X(MPP_RET, mpp_packet_deinit, (MppPacket *)) X(void, mpp_packet_set_pts, (MppPacket, RK_S64)) \
	X(MPP_RET, mpp_frame_deinit, (MppFrame *)) X(RK_U32, mpp_frame_get_info_change, (const MppFrame)) \
	X(RK_U32, mpp_frame_get_width, (const MppFrame)) X(RK_U32, mpp_frame_get_height, (const MppFrame)) \
	X(RK_U32, mpp_frame_get_hor_stride, (const MppFrame)) X(RK_U32, mpp_frame_get_ver_stride, (const MppFrame)) \
	X(RK_U32, mpp_frame_get_errinfo, (const MppFrame)) X(RK_U32, mpp_frame_get_discard, (const MppFrame)) \
	X(RK_S64, mpp_frame_get_pts, (const MppFrame)) X(MppBuffer, mpp_frame_get_buffer, (const MppFrame)) \
	X(MppFrameFormat, mpp_frame_get_fmt, (MppFrame)) X(MppFrameColorSpace, mpp_frame_get_colorspace, (const MppFrame)) \
	X(MppFrameColorRange, mpp_frame_get_color_range, (const MppFrame)) \
	X(int, mpp_buffer_get_fd_with_caller, (MppBuffer, const char *)) \
	X(void *, mpp_buffer_get_ptr_with_caller, (MppBuffer, const char *))
#define DECL(r, n, a) static r(*f_##n) a;
MPPF(DECL)
static int mpp_load(void) {
	void *lib = dlopen("libmpp.so", RTLD_NOW);
	if (!lib) lib = dlopen("/vendor/lib64/libmpp.so", RTLD_NOW);
	if (!lib) { L("dlopen libmpp: %s", dlerror()); return -1; }
#define LOAD(r, n, a) if (!(f_##n = (r(*) a)dlsym(lib, #n))) { L("dlsym %s missing", #n); return -1; }
	MPPF(LOAD)
	return 0;
}

static MppCtx dec;  // written only by the main thread, under mu; the out thread holds dec_busy while using it
static MppApi *mpi;
static int dec_codec, dec_busy;
static struct { uint32_t frame; int64_t rx_us; } meta[64];  // per decoder input: when its first packet arrived

// ---------------------------------------------------------------- KMS (raw ioctls; plane 59-style NV12 overlay on the eDP CRTC)
static int drm = -1, drm_ready, modeset = 1;
static uint32_t ovl_plane, ovl_fb, ovl_pitch, p_zpos, p_zpos_ovl;  // win2: the glass-state overlay above the video
static uint32_t *ovl_map;
static int ovl_shown;  // mu: the overlay plane is on glass
static int bl_off;     // mu: the backlight is off (glass state asleep)
static void backlight(int on);
static int64_t period_us = 17393;  // vblank period: from the mode timings, then refined from flip events
static int64_t margin_us = 1000;   // --margin: commit this long before the target vblank (measured: commits within
                                   // ~0.5 ms of the vblank stamp miss it; the display thread wakes up to ~0.5 ms late)
static int64_t hold_until;         // >0: the pending frame waits until this time (next vblank - margin), newest wins
static int kick_fd = -1;           // eventfd: wakes the display thread when another thread sets hold_until
static int64_t last_vbl;           // timestamp of the latest flip event: the vblank phase
static unsigned late_flips;        // flips that landed a vblank later than the commit aimed for
static int64_t next_vblank(int64_t t) {  // predicted first vblank after t, 0 if the phase is stale
	if (!last_vbl || t - last_vbl > 1000000) return 0;
	return last_vbl + ((t - last_vbl) / period_us + 1) * period_us;
}
static uint32_t crtc, conn, plane, blob, hdisp, vdisp, others[16], nothers;
static uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_rot, p_cs, p_mode, p_active, p_conn_crtc;

static uint32_t prop(uint32_t obj, uint32_t type, const char *name) {
	uint32_t ids[64]; uint64_t vals[64];
	struct drm_mode_obj_get_properties op = {.obj_id = obj, .obj_type = type};
	if (ioctl(drm, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || op.count_props > 64) return 0;
	op.props_ptr = P(ids); op.prop_values_ptr = P(vals);
	if (ioctl(drm, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op)) return 0;
	for (unsigned i = 0; i < op.count_props; i++) {
		struct drm_mode_get_property gp = {.prop_id = ids[i]};
		if (!ioctl(drm, DRM_IOCTL_MODE_GETPROPERTY, &gp) && !strcmp(gp.name, name)) return ids[i];
	}
	return 0;
}

static const char *conn_name(uint32_t t) {  // the panel connector types tabd takes, "?" for any other
	return t == DRM_MODE_CONNECTOR_eDP ? "eDP" : t == DRM_MODE_CONNECTOR_DSI ? "DSI" : t == DRM_MODE_CONNECTOR_LVDS ? "LVDS"
	     : t == DRM_MODE_CONNECTOR_DPI ? "DPI" : "?";
}
static int drm_setup(void) {
	static int said;
	if (drm < 0 && (drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC)) < 0) return -1;
	if (ioctl(drm, DRM_IOCTL_SET_MASTER, 0)) {
		if (!said++) L("drm: not master yet (Android's composer still runs: boot.sh stops it, or stop it by hand)");
		return -1;
	}
	struct drm_set_client_cap cc = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
	ioctl(drm, DRM_IOCTL_SET_CLIENT_CAP, &cc);
	cc.capability = DRM_CLIENT_CAP_ATOMIC;
	if (ioctl(drm, DRM_IOCTL_SET_CLIENT_CAP, &cc)) { L("drm: no atomic"); return -1; }

	uint32_t crtcs[8], conns[8];
	struct drm_mode_card_res r = {0};
	if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &r) || r.count_crtcs > 8 || r.count_connectors > 8) return -1;
	r.count_fbs = r.count_encoders = 0;
	r.crtc_id_ptr = P(crtcs); r.connector_id_ptr = P(conns);
	if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &r)) return -1;

	struct drm_mode_modeinfo modes[16], mode;
	struct drm_mode_get_connector c;
	conn = 0;
	for (unsigned i = 0; i < r.count_connectors && !conn; i++) {
		c = (struct drm_mode_get_connector){.connector_id = conns[i]};
		if (ioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
		if (!(opt_connector ? c.connector_type == (uint32_t)opt_connector : conn_name(c.connector_type)[0] != '?') ||
		    c.connection != 1 || !c.count_modes || c.count_modes > 16) continue;
		c = (struct drm_mode_get_connector){.connector_id = conns[i], .count_modes = 16, .modes_ptr = P(modes)};
		if (ioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
		mode = modes[0];
		for (unsigned k = 0; k < c.count_modes && k < 16; k++)
			if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { mode = modes[k]; break; }
		conn = conns[i];
	}
	if (!conn) {  // once per run: the display thread retries every 0.5 s
		static int said_conn;
		if (!said_conn++) L("drm: no connected %s", opt_connector ? conn_name(opt_connector) : "panel (eDP, DSI, LVDS or DPI)");
		return -1;
	}
	if (c.mm_width && c.mm_height) panel_wmm = c.mm_width, panel_hmm = c.mm_height;
	else panel_wmm = mode.hdisplay * 127 / 800, panel_hmm = mode.vdisplay * 127 / 800;  // 25.4 mm / 160 px
	struct drm_mode_get_encoder e = {.encoder_id = c.encoder_id};
	if (!c.encoder_id || ioctl(drm, DRM_IOCTL_MODE_GETENCODER, &e)) { L("drm: no encoder"); return -1; }
	int ci = -1;
	for (unsigned i = 0; i < r.count_crtcs; i++)
		if (e.crtc_id ? crtcs[i] == e.crtc_id : (e.possible_crtcs >> i) & 1) { ci = i; break; }
	if (ci < 0) return -1;
	crtc = crtcs[ci];

	uint32_t planes[32];
	struct drm_mode_get_plane_res pr = {0};
	if (ioctl(drm, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || pr.count_planes > 32) return -1;
	pr.plane_id_ptr = P(planes);
	ioctl(drm, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
	plane = 0; nothers = 0; ovl_plane = 0;
	for (unsigned i = 0; i < pr.count_planes; i++) {
		uint32_t f[64];
		struct drm_mode_get_plane pl = {.plane_id = planes[i], .count_format_types = 64, .format_type_ptr = P(f)};
		if (ioctl(drm, DRM_IOCTL_MODE_GETPLANE, &pl) || !((pl.possible_crtcs >> ci) & 1)) continue;
		int nv12 = 0, argb = 0;
		for (unsigned k = 0; k < pl.count_format_types && k < 64; k++) nv12 |= f[k] == DRM_FORMAT_NV12, argb |= f[k] == DRM_FORMAT_ARGB8888;
		if (nv12 && !plane) plane = planes[i];
		else if (argb && !nv12 && !ovl_plane) ovl_plane = planes[i];  // win2 (RGB, no scaler): the glass-state overlay
		else if (nothers < 16) others[nothers++] = planes[i];
	}
	if (!plane) { L("drm: no NV12 plane on crtc %u", crtc); return -1; }

	struct drm_mode_create_blob b = {.data = P(&mode), .length = sizeof mode};
	if (ioctl(drm, DRM_IOCTL_MODE_CREATEPROPBLOB, &b)) return -1;
	blob = b.blob_id; hdisp = mode.hdisplay; vdisp = mode.vdisplay;
	if (mode.clock && mode.htotal && mode.vtotal) {
		int64_t px = (int64_t)mode.htotal * mode.vtotal;
		period_us = px * 1000 / mode.clock;
		refresh_x100 = (int)(((int64_t)mode.clock * 100000 + px / 2) / px);
	}
	p_fb = prop(plane, DRM_MODE_OBJECT_PLANE, "FB_ID"); p_crtc = prop(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
	p_sx = prop(plane, DRM_MODE_OBJECT_PLANE, "SRC_X"); p_sy = prop(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
	p_sw = prop(plane, DRM_MODE_OBJECT_PLANE, "SRC_W"); p_sh = prop(plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
	p_cx = prop(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"); p_cy = prop(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
	p_cw = prop(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"); p_ch = prop(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
	p_rot = prop(plane, DRM_MODE_OBJECT_PLANE, "rotation"); p_cs = prop(plane, DRM_MODE_OBJECT_PLANE, "COLOR_SPACE");
	p_mode = prop(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"); p_active = prop(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	p_conn_crtc = prop(conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
	if (!p_fb || !p_crtc || !p_mode || !p_active || !p_conn_crtc) { L("drm: missing props"); return -1; }
	p_zpos = prop(plane, DRM_MODE_OBJECT_PLANE, "ZPOS");
	if (ovl_plane && !ovl_fb) {  // one full-panel ARGB buffer, drawn in place (no flips for the spinner)
		struct drm_mode_create_dumb cd = {.height = vdisp, .width = hdisp, .bpp = 32};
		struct drm_mode_fb_cmd2 c = {.width = hdisp, .height = vdisp, .pixel_format = DRM_FORMAT_ARGB8888};
		struct drm_mode_map_dumb md = {0};
		if (!ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd) && (c.handles[0] = md.handle = cd.handle, c.pitches[0] = cd.pitch,
		    !ioctl(drm, DRM_IOCTL_MODE_ADDFB2, &c)) && !ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md) &&
		    (ovl_map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset)) != MAP_FAILED)
			ovl_fb = c.fb_id, ovl_pitch = cd.pitch, p_zpos_ovl = prop(ovl_plane, DRM_MODE_OBJECT_PLANE, "ZPOS");
		else { ovl_map = NULL; L("glass: no overlay buffer (%s): glass states off", strerror(errno)); }
	}
	L("drm: conn %u (%s) crtc %u plane %u mode %ux%u@%u %dx%d mm (period %lld us, margin %lld us), disabling %u other planes",
	  conn, conn_name(c.connector_type), crtc, plane, hdisp, vdisp, mode.vrefresh, panel_wmm, panel_hmm, (long long)period_us,
	  (long long)margin_us, nothers);
	return 0;
}

static int commit(uint32_t fb, uint32_t w, uint32_t h, uint32_t cs) {
	uint32_t objs[24], cnt[24], ids[96]; uint64_t vals[96]; unsigned no = 0, np = 0;
#define OBJ(o) (objs[no] = (o), cnt[no++] = 0)
#define PUT(p, v) (ids[np] = (p), vals[np++] = (v), cnt[no - 1]++)
	OBJ(plane);
	PUT(p_fb, fb); PUT(p_crtc, crtc);
	PUT(p_sx, 0); PUT(p_sy, 0); PUT(p_sw, (uint64_t)w << 16); PUT(p_sh, (uint64_t)h << 16);
	PUT(p_cx, 0); PUT(p_cy, 0); PUT(p_cw, hdisp); PUT(p_ch, vdisp);
	if (p_rot) PUT(p_rot, DRM_MODE_ROTATE_0);
	if (p_cs) PUT(p_cs, cs);
	if (p_zpos) PUT(p_zpos, 0);                                         // under the glass-state overlay
	if (ovl_shown) { OBJ(ovl_plane); PUT(p_fb, 0); PUT(p_crtc, 0); }  // a new frame: the overlay goes in the same commit
	if (modeset) {
		OBJ(conn); PUT(p_conn_crtc, crtc);
		OBJ(crtc); PUT(p_mode, blob); PUT(p_active, 1);
		for (unsigned i = 0; i < nothers; i++) { OBJ(others[i]); PUT(p_fb, 0); PUT(p_crtc, 0); }
	}
	struct drm_mode_atomic a = {
		.flags = DRM_MODE_PAGE_FLIP_EVENT | (modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : DRM_MODE_ATOMIC_NONBLOCK),
		.count_objs = no, .objs_ptr = P(objs), .count_props_ptr = P(cnt), .props_ptr = P(ids), .prop_values_ptr = P(vals)};
	return ioctl(drm, DRM_IOCTL_MODE_ATOMIC, &a) ? -errno : 0;
}

// framebuffer cache: one KMS fb per MPP buffer; "dead" entries (old decoder generation) go once nothing scans them out
#define FN 32
static struct fbe { MppBuffer buf; int fd, refs, dead; uint32_t fb; } fbc[FN];
static void rmfb(int i) { ioctl(drm, DRM_IOCTL_MODE_RMFB, &fbc[i].fb); memset(&fbc[i], 0, sizeof fbc[i]); }
static void fb_flush(void) { for (int i = 0; i < FN; i++) if (fbc[i].fb) { fbc[i].dead = 1; if (!fbc[i].refs) rmfb(i); } }
static void fb_put(int i) { if (i >= 0 && !--fbc[i].refs && fbc[i].dead) rmfb(i); }

// A slot holds a decoded frame; rb >= 0 when it is shown through a turned panel buffer instead of zero-copy.
struct slot { MppFrame f; int e, rb; uint32_t frame, w, h, hs, vs, cs; int64_t rx_us; };
static struct slot shown = {.e = -1, .rb = -1}, flipping = {.e = -1, .rb = -1}, pending = {.e = -1, .rb = -1};
static int64_t flip_t0;

static int fb_get(struct slot *s) {
	MppBuffer b = f_mpp_frame_get_buffer(s->f);
	int fd = f_mpp_buffer_get_fd_with_caller(b, "tabd"), fr = -1;
	for (int i = 0; i < FN; i++) {
		if (fbc[i].fb && !fbc[i].dead && fbc[i].buf == b && fbc[i].fd == fd) { fbc[i].refs++; return i; }
		if (!fbc[i].fb && fr < 0) fr = i;
	}
	for (int i = 0; i < FN && fr < 0; i++) if (!fbc[i].refs) { rmfb(i); fr = i; }
	if (fr < 0) return -1;
	struct drm_prime_handle ph = {.fd = fd};
	if (ioctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph)) { L("drm: prime import: %s", strerror(errno)); return -1; }
	struct drm_mode_fb_cmd2 c = {.width = s->w, .height = s->h, .pixel_format = DRM_FORMAT_NV12,
		.handles = {ph.handle, ph.handle}, .pitches = {s->hs, s->hs}, .offsets = {0, s->hs * s->vs}};
	int r = ioctl(drm, DRM_IOCTL_MODE_ADDFB2, &c);
	struct drm_gem_close gc = {.handle = ph.handle};
	ioctl(drm, DRM_IOCTL_GEM_CLOSE, &gc);  // the fb keeps its own reference
	if (r) { L("drm: addfb2: %s", strerror(errno)); return -1; }
	fbc[fr] = (struct fbe){.buf = b, .fd = fd, .refs = 1, .fb = c.fb_id};
	return fr;
}
static void slot_free(struct slot *s) {
	if (s->f) { fb_put(s->e); f_mpp_frame_deinit(&s->f); }
	memset(s, 0, sizeof *s);
	s->e = s->rb = -1;
}

// ---------------------------------------------------------------- locked orientation: RGA turns frames into panel buffers
// The VOP can't rotate NV12, so for 90/180/270 each decoded (upright) frame is turned by RGA into one of RB panel-native
// 1920x1080 NV12 dumb buffers. Declarations below: airockchip/librga 7fd8d48 (v1.8.1; the vendor librga.so is
// "1.8.1_[4]"), include/im2d_type.h + im2d.h, Apache-2.0. Only what tabd uses, laid out exactly as the vendor build.
typedef uint32_t rga_buffer_handle_t;
typedef struct { int max, min; } im_colorkey_range;
typedef struct { int scale_r, scale_g, scale_b, offset_r, offset_g, offset_b; } im_nn_t;
typedef struct {
	void *vir_addr, *phy_addr;
	int fd, width, height, wstride, hstride, format, color_space_mode, global_alpha, rd_mode, color;
	im_colorkey_range colorkey_range;
	im_nn_t nn;
	int rop_code;
	rga_buffer_handle_t handle;
} rga_buffer_t;
typedef struct { uint32_t width, height, format; } im_handle_param_t;
#define RK_FORMAT_NV12 (0xa << 8)  // RK_FORMAT_YCbCr_420_SP
#define IM_ROT_90 1                // IM_HAL_TRANSFORM_ROT_90 (clockwise); _180 = 2, _270 = 4
static rga_buffer_handle_t (*rga_import)(int fd, im_handle_param_t *p);
static int (*rga_release)(rga_buffer_handle_t h);
static rga_buffer_t (*rga_wrap)(rga_buffer_handle_t h, int w, int hgt, int ws, int hs, int fmt);
static int (*rga_rotate)(const rga_buffer_t src, rga_buffer_t dst, int rotation, int sync);
static const char *(*rga_err)(int status);
static int orient;  // ml mode, locked at start: the tablet turned 0/90/180/270 degrees clockwise from native landscape
#define RB 4        // on glass, flipping, pending, being written
static struct { uint32_t fb, pitch; int fd; rga_buffer_handle_t rh; uint8_t *map; } rbuf[RB];
static int rga_ready;  // 0 not yet, 1 ready, -1 failed

static int rga_setup(void) {  // mu held, DRM ready
	void *lib = dlopen("librga.so", RTLD_NOW);
	if (!lib) lib = dlopen("/vendor/lib64/librga.so", RTLD_NOW);
	if (!lib || !(rga_import = dlsym(lib, "importbuffer_fd")) || !(rga_release = dlsym(lib, "releasebuffer_handle")) ||
	    !(rga_wrap = dlsym(lib, "wrapbuffer_handle_t")) || !(rga_rotate = dlsym(lib, "imrotate_t")) || !(rga_err = dlsym(lib, "imStrError_t"))) {
		L("rga: can't load librga: %s", dlerror());
		return -1;
	}
	for (int i = 0; i < RB; i++) {
		struct drm_mode_create_dumb cd = {.height = vdisp * 3 / 2, .width = hdisp, .bpp = 8};
		if (ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { L("rga: dumb buffer: %s", strerror(errno)); return -1; }
		struct drm_mode_fb_cmd2 c = {.width = hdisp, .height = vdisp, .pixel_format = DRM_FORMAT_NV12,
			.handles = {cd.handle, cd.handle}, .pitches = {cd.pitch, cd.pitch}, .offsets = {0, cd.pitch * vdisp}};
		struct drm_prime_handle ph = {.handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
		struct drm_mode_map_dumb md = {.handle = cd.handle};
		if (ioctl(drm, DRM_IOCTL_MODE_ADDFB2, &c) || ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) ||
		    ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) { L("rga: panel buffer setup: %s", strerror(errno)); return -1; }
		im_handle_param_t pp = {cd.pitch, vdisp, RK_FORMAT_NV12};
		rbuf[i].fb = c.fb_id; rbuf[i].pitch = cd.pitch; rbuf[i].fd = ph.fd; rbuf[i].rh = rga_import(ph.fd, &pp);
		rbuf[i].map = mmap(NULL, cd.size, PROT_READ, MAP_SHARED, drm, md.offset);
		if (!rbuf[i].rh) { L("rga: can't import panel buffer %d", i); return -1; }
	}
	L("rga: %d panel buffers %ux%u pitch %u, turning frames %d degrees clockwise", RB, hdisp, vdisp, rbuf[0].pitch, (360 - orient) % 360);
	return 0;
}
// Turn the slot's decoded frame into panel buffer i. The frame is the upright picture; the tablet is turned `orient`
// clockwise, so the picture turns the other way: 90 -> RGA 270, 180 -> 180, 270 -> RGA 90.
static int rga_turn(struct slot *s, int i) {
	MppBuffer b = f_mpp_frame_get_buffer(s->f);
	im_handle_param_t p = {s->hs, s->vs, RK_FORMAT_NV12};
	rga_buffer_handle_t h = rga_import(f_mpp_buffer_get_fd_with_caller(b, "tabd"), &p);
	if (!h) { LR("rga: can't import the decoded frame"); return -1; }
	rga_buffer_t src = rga_wrap(h, s->w, s->h, s->hs, s->vs, RK_FORMAT_NV12);
	rga_buffer_t dst = rga_wrap(rbuf[i].rh, hdisp, vdisp, rbuf[i].pitch, vdisp, RK_FORMAT_NV12);
	int st = rga_rotate(src, dst, orient == 90 ? IM_ROT_90 << 2 : orient == 270 ? IM_ROT_90 : IM_ROT_90 << 1, 1);
	rga_release(h);
	if (st != 1 && st != 2) { LR("rga: rotate %d: %s", orient, rga_err(st)); return -1; }  // IM_STATUS_SUCCESS / NOERROR
	return 0;
}

// Flip policy: a frame that arrives with nothing in flight is committed at once (it shows at the next vblank).
// A frame that arrives while a flip is in flight is held, and when that flip lands, it is NOT committed straight away:
// committing right after a vblank locks in that frame for the NEXT vblank, so every newer frame arriving meanwhile would
// wait an extra period (60 fps into a 57.5 Hz panel = +1 period on every frame). Instead it waits until just before the
// next vblank (hold_until) and the newest frame by then goes out.
static void do_flip(void) {  // mu held: put the pending frame on the next vblank if no flip is in flight
	static uint32_t last_err;
	if (!drm_ready || flipping.f || !pending.f) return;
	if (pending.rb < 0 && pending.e < 0 && (pending.e = fb_get(&pending)) < 0) { slot_free(&pending); ADD(dropped_total, 1); return; }
	int r = pending.rb >= 0 ? commit(rbuf[pending.rb].fb, hdisp, vdisp, pending.cs) : commit(fbc[pending.e].fb, pending.w, pending.h, pending.cs);
	if (!r) {
		flipping = pending; memset(&pending, 0, sizeof pending); pending.e = -1; flip_t0 = now_us(); modeset = 0;
		ovl_shown = 0;
		if (bl_off) backlight(1);  // a picture again: wake the panel with it
		return;
	}
	if (r == -EBUSY) { hold_until = now_us() + 1000; return; }  // retry in 1 ms
	if (now_ms() - last_err > 1000) { last_err = now_ms(); L("drm: commit (modeset %d): %s", modeset, strerror(-r)); }
	slot_free(&pending); ADD(dropped_total, 1);
	modeset = 1;
	if (r == -EACCES || r == -EPERM) drm_ready = 0;  // lost master: re-acquire
}

static void flip_done(int64_t vbl_us) {  // mu held
	if (!flipping.f) return;
	if (vbl_us - flip_t0 > period_us + 200) late_flips++;  // missed its vblank
	if (last_vbl) {  // refine the period from the spacing of flip events (whole periods apart)
		int64_t d = vbl_us - last_vbl, n = (d + period_us / 2) / period_us;
		if (n >= 1 && n <= 8 && llabs(d - n * period_us) < 1000) period_us += (d / n - period_us) / 16;
	}
	last_vbl = vbl_us;
	hold_until = pending.f ? vbl_us + period_us - margin_us : 0;
	slot_free(&shown);
	shown = flipping; memset(&flipping, 0, sizeof flipping); flipping.e = -1;
	shown_total++;
	__atomic_store_n(&last_flip_ms, now_ms(), __ATOMIC_RELAXED);
	rpush(&r_flip, 1);
	if (reset_ms) { L("video: first frame on glass %u ms after the stream (re)started", now_ms() - reset_ms); reset_ms = 0; }
	if (shown.rx_us) rpush(&r_rxg, (int32_t)((vbl_us - shown.rx_us) / 1000));  // the tablet's own clock, end to end
}

// ---------------------------------------------------------------- glass states: what the panel shows without a live picture
// On the win2 overlay, above the video: connecting (small spinner), PC silent 1 s (dim the frozen frame + spinner, so a
// frozen frame never looks live), 5 s "No signal", 30 s backlight off; pairing shows the PIN. A new frame takes the overlay
// off in its own commit and wakes the backlight; a touch while asleep wakes it and is consumed. Drawn upright for the
// locked orientation, in a small 5x7 font scaled like a monitor's OSD.
enum { G_LIVE, G_CONNECTING, G_PIN, G_SILENT, G_NOSIGNAL, G_ASLEEP };
static const char *g_name[] = {"live", "connecting", "PIN", "silent: dimmed", "no signal", "asleep: backlight off"};
static int g_state = -1, pin_active;  // atomic
static char pin_text[8];
static uint32_t start_ms, wake_ms;   // atomic
static void backlight(int on) {      // bl_power keeps the user's brightness level
	int fd = *bl_power ? open(bl_power, O_WRONLY | O_CLOEXEC) : -1;
	if (*bl_power && (fd < 0 || write(fd, on ? "0" : "4", 1) != 1)) L("glass: backlight: %s", strerror(errno));
	if (fd >= 0) close(fd);
	bl_off = !on;
	L("glass: backlight %s", on ? "on" : "off");
}
static int glass_state(uint32_t now) {
	if (__atomic_load_n(&pin_active, __ATOMIC_RELAXED)) return G_PIN;
	uint32_t lf = __atomic_load_n(&last_flip_ms, __ATOMIC_RELAXED), ref = lf ? lf : start_ms, wk = __atomic_load_n(&wake_ms, __ATOMIC_RELAXED);
	if (lf && now - lf < 1000) return G_LIVE;
	if (now - ((int32_t)(wk - ref) > 0 ? wk : ref) >= 30000) return G_ASLEEP;  // 30 s since the last picture or wake touch
	if (!lf) return G_CONNECTING;
	return now - lf < 5000 ? G_SILENT : G_NOSIGNAL;
}
static const struct { char c; const char *r[7]; } font[] = {
	{'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}}, {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
	{'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}}, {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
	{'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}}, {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
	{'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}}, {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
	{'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}}, {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
	{'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}}, {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
	{'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}, {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
	{'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}}, {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
	{'a', {"     ", "     ", " ### ", "    #", " ####", "#   #", " ####"}}, {'c', {"     ", "     ", " ### ", "#    ", "#    ", "#   #", " ### "}},
	{'e', {"     ", "     ", " ### ", "#   #", "#####", "#    ", " ### "}}, {'g', {"     ", "     ", " ####", "#   #", " ####", "    #", " ### "}},
	{'h', {"#    ", "#    ", "#### ", "#   #", "#   #", "#   #", "#   #"}}, {'i', {"  #  ", "     ", " ##  ", "  #  ", "  #  ", "  #  ", " ### "}},
	{'l', {" ##  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}, {'n', {"     ", "     ", "#### ", "#   #", "#   #", "#   #", "#   #"}},
	{'o', {"     ", "     ", " ### ", "#   #", "#   #", "#   #", " ### "}}, {'r', {"     ", "     ", "# ## ", "##  #", "#    ", "#    ", "#    "}},
	{'s', {"     ", "     ", " ####", "#    ", " ### ", "    #", "#### "}}, {'t', {"  #  ", "  #  ", "#####", "  #  ", "  #  ", "  #  ", "   ##"}},
	{'u', {"     ", "     ", "#   #", "#   #", "#   #", "#  ##", " ## #"}},
};
static int gor, gw, gh;  // orientation and upright size of the current drawing
static void gpix(int x, int y, uint32_t c) {  // upright (x, y) -> panel pixel, for the locked orientation
	int px = x, py = y;
	if (gor == 90) px = y, py = (int)vdisp - 1 - x;
	else if (gor == 180) px = (int)hdisp - 1 - x, py = (int)vdisp - 1 - y;
	else if (gor == 270) px = (int)hdisp - 1 - y, py = x;
	if (px >= 0 && py >= 0 && px < (int)hdisp && py < (int)vdisp) ovl_map[py * (ovl_pitch / 4) + px] = c;
}
static void gbox(int x, int y, int w, int h, uint32_t c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) gpix(x + i, y + j, c); }
static void gtext(int cx, int y, const char *t, int sc, uint32_t c) {  // centred on cx
	for (int x = cx - ((int)strlen(t) * 6 * sc - sc) / 2; *t; t++, x += 6 * sc)
		for (unsigned g = 0; g < sizeof font / sizeof *font; g++)
			if (font[g].c == *t)
				for (int ry = 0; ry < 7; ry++)
					for (int rx = 0; rx < 5; rx++)
						if (font[g].r[ry][rx] == '#') gbox(x + rx * sc, y + ry * sc, sc, sc, c);
}
static void gspinner(int cx, int cy, int phase, uint32_t bg) {  // 8 dots, the head brightest
	const int R = 34, r = 7;
	gbox(cx - R - r - 1, cy - R - r - 1, 2 * (R + r + 1), 2 * (R + r + 1), bg);
	for (int k = 0; k < 8; k++) {
		uint32_t v = 255 - ((phase - k) & 7) * 26;
		int dx = (int)lrintf(R * cosf(k * 0.7853982f)), dy = (int)lrintf(R * sinf(k * 0.7853982f));
		for (int j = -r; j <= r; j++)
			for (int i = -r; i <= r; i++)
				if (i * i + j * j <= r * r) gpix(cx + dx + i, cy + dy + j, 0xff000000u | v << 16 | v << 8 | v);
	}
}
#define G_DIM 0xa0000000u     // 63% black over the frozen frame
#define G_DARK 0xc8000000u    // 78%: no signal
static void glass_draw(int st, int phase, int full) {
	gor = orient;
	gw = gor % 180 ? (int)vdisp : (int)hdisp; gh = gor % 180 ? (int)hdisp : (int)vdisp;
	uint32_t bg = st == G_CONNECTING ? 0 : st == G_SILENT ? G_DIM : st == G_PIN ? 0xff000000u : G_DARK;
	if (full)
		for (uint32_t y = 0; y < vdisp; y++)
			for (uint32_t x = 0, *row = ovl_map + y * (ovl_pitch / 4); x < hdisp; x++) row[x] = bg;
	if (st == G_CONNECTING) { gspinner(gw / 2, gh / 2 - 40, phase, bg); if (full) gtext(gw / 2, gh / 2 + 30, "Connecting", 4, 0xffa0a0a0u); }
	else if (st == G_SILENT) gspinner(gw / 2, gh / 2, phase, bg);
	else if (full && st == G_PIN) {
		gtext(gw / 2, gh / 2 - 170, "Enter this PIN in Sunshine", 5, 0xffa0a0a0u);
		gtext(gw / 2, gh / 2 - 50, pin_text, 20, 0xffffffffu);
	} else if (full) gtext(gw / 2, gh / 2 - 28, "No signal", 8, 0xffffffffu);
}
static int ovl_commit(void) {  // mu held: the overlay on glass (blocking, no event); a modeset first if nothing is on yet
	uint32_t objs[24], cnt[24], ids[96]; uint64_t vals[96]; unsigned no = 0, np = 0;
	OBJ(ovl_plane);
	PUT(p_fb, ovl_fb); PUT(p_crtc, crtc);
	PUT(p_sx, 0); PUT(p_sy, 0); PUT(p_sw, (uint64_t)hdisp << 16); PUT(p_sh, (uint64_t)vdisp << 16);
	PUT(p_cx, 0); PUT(p_cy, 0); PUT(p_cw, hdisp); PUT(p_ch, vdisp);
	if (p_zpos_ovl) PUT(p_zpos_ovl, 1);
	if (p_zpos) { OBJ(plane); PUT(p_zpos, 0); }
	if (modeset) {
		OBJ(conn); PUT(p_conn_crtc, crtc);
		OBJ(crtc); PUT(p_mode, blob); PUT(p_active, 1);
		for (unsigned i = 0; i < nothers; i++) { OBJ(others[i]); PUT(p_fb, 0); PUT(p_crtc, 0); }
	}
	struct drm_mode_atomic a = {.flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, .count_objs = no, .objs_ptr = P(objs),
		.count_props_ptr = P(cnt), .props_ptr = P(ids), .prop_values_ptr = P(vals)};
	int r = ioctl(drm, DRM_IOCTL_MODE_ATOMIC, &a) ? -errno : 0;
	if (r) LR("glass: overlay commit: %s", strerror(-r));
	else modeset = 0;
	return r;
}
static void glass_update(void) {  // display thread, every loop pass
	static int phase;
	static uint32_t anim;
	if (!ovl_map || !drm_ready) return;
	uint32_t now = now_ms();
	int st = glass_state(now), was = __atomic_load_n(&g_state, __ATOMIC_RELAXED);
	if (st != was) L("glass: %s", g_name[st]);
	__atomic_store_n(&g_state, st, __ATOMIC_RELAXED);
	if (st == G_LIVE) return;  // the video commit took the overlay off and woke the backlight
	if (st != was) glass_draw(st, phase, 1);
	else if ((st == G_SILENT || st == G_CONNECTING) && now - anim >= 100) anim = now, phase = (phase + 1) & 7, glass_draw(st, phase, 0);
	pthread_mutex_lock(&mu);
	if (!ovl_shown && !flipping.f && !ovl_commit()) ovl_shown = 1;
	if ((st == G_ASLEEP) != bl_off) backlight(st != G_ASLEEP);
	pthread_mutex_unlock(&mu);
}

static void *disp_main(void *arg) {
	(void)arg; rt(10);
	for (;;) {
		__atomic_store_n(&disp_tick, now_ms(), __ATOMIC_RELAXED);
		if (!drm_ready) {
			pthread_mutex_lock(&mu);
			if (!drm_setup()) { __atomic_store_n(&drm_ready, 1, __ATOMIC_RELEASE); modeset = 1; do_flip(); }  // main waits for it
			pthread_mutex_unlock(&mu);
			if (!drm_ready) { usleep(500000); continue; }
		}
		pthread_mutex_lock(&mu);
		int64_t wait = hold_until ? hold_until - now_us() : 50000;
		pthread_mutex_unlock(&mu);
		wait = wait < 0 ? 0 : wait > 50000 ? 50000 : wait;
		struct timespec ts = {0, wait * 1000};
		struct pollfd p[2] = {{drm, POLLIN, 0}, {kick_fd, POLLIN, 0}};
		ppoll(p, 2, &ts, NULL);
		if (p[1].revents & POLLIN) { uint64_t n; if (read(kick_fd, &n, sizeof n) < 0) n = 0; }  // a new hold: recompute the wait
		if (p[0].revents & POLLIN) {
			char buf[1024];
			ssize_t len = read(drm, buf, sizeof buf);
			for (ssize_t o = 0; o + (ssize_t)sizeof(struct drm_event) <= len;) {
				struct drm_event *ev = (void *)(buf + o);
				if (ev->type == DRM_EVENT_FLIP_COMPLETE) {
					struct drm_event_vblank *v = (void *)ev;
					pthread_mutex_lock(&mu); flip_done(v->tv_sec * 1000000LL + v->tv_usec); pthread_mutex_unlock(&mu);
				}
				if (!ev->length) break;
				o += ev->length;
			}
		}
		pthread_mutex_lock(&mu);
		if (flipping.f && now_us() - flip_t0 > 500000) { L("drm: flip event missing, forcing modeset"); modeset = 1; flip_done(now_us()); }
		if (!hold_until || now_us() >= hold_until) { hold_until = 0; do_flip(); }
		pthread_mutex_unlock(&mu);
		glass_update();
	}
	return NULL;
}

// ---------------------------------------------------------------- decoder output
static int64_t fault_t0;  // first successful decoder input (fault injection clock)
static int fault_after(int ms) { return *opt_fault && fault_t0 && now_us() - fault_t0 >= ms * 1000LL; }

static void on_frame(MppCtx c, MppApi *a, MppFrame f) {
	stall_since = 0;
	if (f_mpp_frame_get_info_change(f)) {
		L("decoder: %ux%u stride %ux%u fmt 0x%x", f_mpp_frame_get_width(f), f_mpp_frame_get_height(f),
		  f_mpp_frame_get_hor_stride(f), f_mpp_frame_get_ver_stride(f), f_mpp_frame_get_fmt(f));
		a->control(c, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
		pthread_mutex_lock(&mu); fb_flush(); pthread_mutex_unlock(&mu);
		f_mpp_frame_deinit(&f);
		return;
	}
	struct slot s = {.f = f, .e = -1, .rb = -1, .frame = (uint32_t)f_mpp_frame_get_pts(f), .w = f_mpp_frame_get_width(f),
		.h = f_mpp_frame_get_height(f), .hs = f_mpp_frame_get_hor_stride(f), .vs = f_mpp_frame_get_ver_stride(f)};
	MppBuffer b = f_mpp_frame_get_buffer(f);
	if (f_mpp_frame_get_errinfo(f) || f_mpp_frame_get_discard(f) || !b || !s.w || !s.h ||
	    (f_mpp_frame_get_fmt(f) & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP) {
		f_mpp_frame_deinit(&f);
		ADD(dropped_total, 1);
		if ((int32_t)(s.frame - __atomic_load_n(&key_frame, __ATOMIC_RELAXED)) >= 0) __atomic_store_n(&need_key, 1, __ATOMIC_RELAXED);
		return;
	}
	MppFrameColorSpace sp = f_mpp_frame_get_colorspace(f);
	s.cs = f_mpp_frame_get_color_range(f) == MPP_FRAME_RANGE_JPEG ? 7 /* V4L2 JPEG: 601 full */
	     : sp == MPP_FRAME_SPC_BT470BG || sp == MPP_FRAME_SPC_SMPTE170M ? 1 /* 601 limited */ : 3 /* 709 limited */;

	pthread_mutex_lock(&mu);
	unsigned m = s.frame & 63;
	if (meta[m].frame == s.frame) s.rx_us = meta[m].rx_us;
	pthread_mutex_unlock(&mu);

	static int n;
	uint8_t *y;
	if (opt_dump >= 0 && !orient && n++ == opt_dump && (y = f_mpp_buffer_get_ptr_with_caller(b, "tabd"))) {  // -d: CPU read
		int fd = f_mpp_buffer_get_fd_with_caller(b, "tabd");
		struct dma_buf_sync sy = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy);
		FILE *o = fopen(D "/frame.nv12", "wb");
		for (uint32_t r = 0; o && r < s.h; r++) fwrite(y + r * s.hs, 1, s.w, o);
		for (uint32_t r = 0; o && r < s.h / 2; r++) fwrite(y + s.hs * s.vs + r * s.hs, 1, s.w, o);
		if (o) fclose(o);
		L("dumped frame %u (%ux%u)", s.frame, s.w, s.h);
		sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy);
	}

	if (orient) {  // turn the upright picture into a panel buffer (orientation 0 stays zero-copy)
		int want_w = orient == 180 ? hdisp : vdisp, want_h = orient == 180 ? vdisp : hdisp, i = -1;
		int64_t t_rga = now_us();
		pthread_mutex_lock(&mu);
		if (!rga_ready && drm_ready) rga_ready = rga_setup() ? -1 : 1;
		for (int k = 0; k < RB && rga_ready == 1 && i < 0; k++)
			if (shown.rb != k && flipping.rb != k && pending.rb != k) i = k;
		pthread_mutex_unlock(&mu);
		if (s.w != (uint32_t)want_w || s.h != (uint32_t)want_h)
			LR("rga: stream is %ux%u, orientation %d wants %dx%d: shown unturned", s.w, s.h, orient, want_w, want_h);
		else if (i < 0 || rga_turn(&s, i)) { slot_free(&s); ADD(dropped_total, 1); return; }
		else {
			s.rb = i;
			pthread_mutex_lock(&mu); rpush(&r_rga, (int32_t)(now_us() - t_rga)); pthread_mutex_unlock(&mu);
			static int dn;
			if (opt_dump >= 0 && dn++ == opt_dump && rbuf[i].map != MAP_FAILED) {  // the panel buffer as scanned out
				FILE *o = fopen(D "/frame.nv12", "wb");
				for (uint32_t r = 0; o && r < vdisp * 3 / 2; r++) fwrite(rbuf[i].map + r * rbuf[i].pitch, 1, hdisp, o);
				if (o) fclose(o);
				L("dumped turned frame %u (%ux%u from %ux%u, orientation %d)", s.frame, hdisp, vdisp, s.w, s.h, orient);
			}
		}
	}

	pthread_mutex_lock(&mu);
	if (pending.f) { slot_free(&pending); ADD(dropped_total, 1); }  // superseded before scanout
	pending = s;
	if (!hold_until) {  // otherwise the display thread sends the newest frame at hold_until
		int64_t t = now_us(), nv = next_vblank(t);
		if (nv && nv - t < margin_us) {  // too close to make this vblank: aim at the next, and wake the display thread for it
			uint64_t one = 1;
			hold_until = nv + period_us - margin_us;
			if (write(kick_fd, &one, sizeof one) < 0) hold_until = 0, do_flip();
		}
		else do_flip();
	}
	pthread_mutex_unlock(&mu);
}

static void *out_main(void *arg) {
	(void)arg; rt(10);
	for (;;) {
		pthread_mutex_lock(&mu);
		while (!dec) pthread_cond_wait(&cv, &mu);
		MppCtx c = dec; MppApi *a = mpi;
		dec_busy = 1;
		pthread_mutex_unlock(&mu);
		MppFrame f = NULL;
		int64_t t0 = now_us();
		a->decode_get_frame(c, &f);  // blocks up to MPP_SET_OUTPUT_TIMEOUT
		if (fault_after(5000) && !strcmp(opt_fault, "hang")) { L("fault: decode_get_frame never returns"); for (;;) pause(); }
		if (f) on_frame(c, a, f);
		pthread_mutex_lock(&mu); dec_busy = 0; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
		if (!f && now_us() - t0 < 2000) usleep(2000);
	}
	return NULL;
}

static void dec_close(void) {
	pthread_mutex_lock(&mu);
	MppCtx c = dec;
	dec = NULL;
	while (dec_busy) pthread_cond_wait(&cv, &mu);
	fb_flush();
	pthread_mutex_unlock(&mu);
	if (c) f_mpp_destroy(c);  // MPP keeps buffers still held by the display slots alive until they are released
	stall_since = 0;
}

static int dec_open(int codec) {
	MppCtx c; MppApi *a; RK_U32 on = 1; RK_S64 timeout = 100;
	if (f_mpp_create(&c, &a)) return -1;
	a->control(c, MPP_DEC_SET_ENABLE_FAST_PLAY, &on);  // must precede mpp_init
	if (f_mpp_init(c, MPP_CTX_DEC, codec == ML_H264 ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingHEVC)) { f_mpp_destroy(c); return -1; }
	a->control(c, MPP_DEC_SET_IMMEDIATE_OUT, &on);
	a->control(c, MPP_SET_OUTPUT_TIMEOUT, &timeout);
	pthread_mutex_lock(&mu);
	dec = c; mpi = a; dec_codec = codec;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	L("decoder: %s ready", codec == ML_H264 ? "h264" : "hevc");
	return 0;
}

// ---------------------------------------------------------------- decoder input
// Fed by libml's session thread (Sunshine); in_mu serialises it with the main loop's stall recovery. Returns -1 when
// the AU is dropped waiting for a keyframe.
struct vin { const uint8_t *data; uint32_t len, frame; int codec, key; int64_t rx_us; };
static pthread_mutex_t in_mu = PTHREAD_MUTEX_INITIALIZER;
static int video_in(const struct vin *v) {
	int ret = -1;
	pthread_mutex_lock(&in_mu);
	if (__atomic_load_n(&need_key, __ATOMIC_RELAXED) && !v->key) { ADD(dropped_total, 1); goto out; }
	if (!dec || dec_codec != v->codec) {
		if (!v->key) { ADD(dropped_total, 1); __atomic_store_n(&need_key, 1, __ATOMIC_RELAXED); goto out; }
		dec_close();
		if (dec_open(v->codec)) { L("decoder: init failed"); ADD(dropped_total, 1); __atomic_store_n(&need_key, 1, __ATOMIC_RELAXED); goto out; }
	}
	pthread_mutex_lock(&mu);
	unsigned m = v->frame & 63;
	meta[m].frame = v->frame; meta[m].rx_us = v->rx_us;
	pthread_mutex_unlock(&mu);
	MppPacket pk;
	f_mpp_packet_init(&pk, (void *)v->data, v->len);
	f_mpp_packet_set_pts(pk, v->frame);
	MPP_RET r = MPP_NOK;
	int fault = !strcmp(opt_fault, "put") && fault_after(5000) && !fault_after(8500);  // 3.5 s of failing puts
	for (int i = 0; i < 5; i++) {  // MPP copies the data; a full input queue gets 5 ms, then the AU is dropped
		if ((r = fault ? MPP_ERR_BUFFER_FULL : mpi->decode_put_packet(dec, pk)) == MPP_OK) break;
		usleep(1000);
	}
	f_mpp_packet_deinit(&pk);
	if (!stall_since) stall_since = now_us();  // armed on failure too: a decoder that keeps refusing input gets reopened
	if (r != MPP_OK) { ADD(dropped_total, 1); __atomic_store_n(&need_key, 1, __ATOMIC_RELAXED); goto out; }
	if (!fault_t0) fault_t0 = now_us();
	if (v->key) { __atomic_store_n(&key_frame, v->frame, __ATOMIC_RELAXED); __atomic_store_n(&need_key, 0, __ATOMIC_RELAXED); }
	ret = 0;
out:
	pthread_mutex_unlock(&in_mu);
	return ret;
}

// ---------------------------------------------------------------- input devices, found by name (event numbers can move)
// Input devices are found WITHOUT asking any driver for a string: on this kernel the goodix-ts input device's "phys"
// pointer dangles (EVIOCGPHYS once panicked the kernel), so no EVIOCGNAME/PHYS/UNIQ, no sysfs name/phys,
// no /proc/bus/input/devices. Instead: the driver symlink and parent device path (kernfs names), an optional attribute
// directory, then the event capabilities (EVIOCGBIT/EVIOCGPROP: bitmaps, no strings). An empty driver or parent matches
// any device.
static int bit(const uint8_t *b, int n) { return b[n / 8] >> (n % 8) & 1; }
static int input_open(const char *driver, const char *parent, const char *attrdir, int (*want)(int fd), char path[32]) {
	for (int i = 0; i < 32; i++) {
		char sys[96], link[PATH_MAX];
		snprintf(sys, sizeof sys, "/sys/class/input/event%d/device/device/driver", i);
		ssize_t n = readlink(sys, link, sizeof link - 1);
		if (*driver) {
			if (n <= 0) continue;
			link[n] = 0;
			if (strcmp(strrchr(link, '/') ? strrchr(link, '/') + 1 : link, driver)) continue;
		}
		snprintf(sys, sizeof sys, "/sys/class/input/event%d/device/device", i);
		if (*parent) {
			if (!realpath(sys, link)) continue;  // the link itself is relative ("../../../4-006a"): resolve it
			size_t lp = strlen(parent), ll = strlen(link);
			if (ll < lp || strcmp(link + ll - lp, parent)) continue;
		}
		struct stat st;
		snprintf(sys, sizeof sys, "/sys/class/input/event%d/device/%s", i, attrdir ? attrdir : ".");
		if (stat(sys, &st)) continue;
		snprintf(path, 32, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0) continue;
		if (want(fd)) return fd;
		close(fd);
	}
	return -1;
}
// The touchscreen: multi-touch X and Y, and INPUT_PROP_DIRECT (on the screen); else, as some vendor drivers never set
// that property, multi-touch that isn't INPUT_PROP_POINTER (a touchpad). touch_driver / touch_parent narrow it.
static int touch_pass;  // 0: direct, 1: not a pointer
static int want_touch(int fd) {
	uint8_t abs[ABS_MAX / 8 + 1] = {0}, props[INPUT_PROP_MAX / 8 + 1] = {0};
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0 || !bit(abs, ABS_MT_POSITION_X) || !bit(abs, ABS_MT_POSITION_Y)) return 0;
	if (ioctl(fd, EVIOCGPROP(sizeof props), props) < 0) return touch_pass == 1;
	return touch_pass ? !bit(props, INPUT_PROP_POINTER) : bit(props, INPUT_PROP_DIRECT);
}
static int touch_find(char path[32]) {  // touch_pass: the pass that found it
	for (touch_pass = 0; touch_pass < 2; touch_pass++) {
		int fd = input_open(opt_touch_driver, opt_touch_parent, NULL, want_touch, path);
		if (fd >= 0) return fd;
	}
	return -1;
}
// orient = auto: the reference board's hook (rk3399-tve1030g), an LSM6DS3 found by its driver and I2C address. It has
// accel/ (the gyro has the same MSC bits) and reports MSC_SERIAL. Another board needs its own hook, or a fixed orient.
static int want_accel(int fd) { uint8_t b[MSC_MAX / 8 + 1] = {0}; return ioctl(fd, EVIOCGBIT(EV_MSC, sizeof b), b) >= 0 && bit(b, MSC_SERIAL); }
#define ACCEL_OPEN(path) input_open("lsm6ds3", "i2c-4/4-006a", "accel", want_accel, path)

// ---------------------------------------------------------------- orientation from the LSM6DS3 accelerometer (orient = auto;
// the reference board's hook, above)
// The driver reports x/y/z in micro-g as MSC_SERIAL/MSC_PULSELED/MSC_GESTURE. Android called this pose 180:
// x = -0.54 g, y ~ 0, z = +0.87 g (tablet leaning back ~30 degrees, upside down).
static void accel_enable(const char *path) {  // Android's sensor HAL enables it; after a boot straight into tabd it may be off
	char en[80], v[4] = "";
	snprintf(en, sizeof en, "/sys/class/input/%s/device/accel/enable", path + 11);
	int e = open(en, O_RDWR | O_CLOEXEC);
	if (e >= 0 && read(e, v, 1) == 1 && v[0] == '0' && pwrite(e, "1", 1, 0) == 1) L("accel: enabled %s", en);
	if (e >= 0) close(e);
}
// Orientation at start: the tablet turned 0/90/180/270 clockwise from native landscape, or -1 when gravity
// isn't clear (lying flat, near a diagonal, moving). Raw +x is the panel's native top edge (checked against Android's
// sensor service), raw +y its native left edge (handedness: raw (x, z) = Android (Y, Z) makes raw y = -Android X).
static int classify4(int x, int y, int z) {
	int64_t g2 = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;
	int ax = abs(x), ay = abs(y);
	if (g2 < 700000LL * 700000 || g2 > 1300000LL * 1300000) return -1;
	if (ax >= ay) return ax < 400000 || ax < 2 * ay ? -1 : x > 0 ? 0 : 180;
	return ay < 400000 || ay < 2 * ax ? -1 : y > 0 ? 90 : 270;
}
static int detect_orient(void) {
	char path[32];
	int fd = ACCEL_OPEN(path), x = 0, y = 0, z = 0, n = 0;
	int64_t sx = 0, sy = 0, sz = 0, t0 = now_us();
	if (fd < 0) { L("orient: no accelerometer"); return -1; }
	accel_enable(path);
	while (n < 8 && now_us() - t0 < 1500000) {  // ~0.3 s of samples at 26 Hz
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, 100) <= 0) continue;
		struct input_event ev[32];
		ssize_t r = read(fd, ev, sizeof ev);
		for (ssize_t k = 0; k < r / (ssize_t)sizeof *ev; k++) {
			if (ev[k].type == EV_MSC && ev[k].code == MSC_SERIAL) x = ev[k].value;
			else if (ev[k].type == EV_MSC && ev[k].code == MSC_PULSELED) y = ev[k].value;
			else if (ev[k].type == EV_MSC && ev[k].code == MSC_GESTURE) z = ev[k].value;
			else if (ev[k].type == EV_SYN && ev[k].code == SYN_REPORT && (x || y || z)) sx += x, sy += y, sz += z, n++;
		}
	}
	close(fd);
	if (!n) { L("orient: no accelerometer samples in 1.5 s"); return -1; }
	int o = classify4(sx / n, sy / n, sz / n);
	L("orient: gravity x %+.2f y %+.2f z %+.2f g (%d samples) -> %s", sx / n / 1e6, sy / n / 1e6, sz / n / 1e6, n,
	  o == 0 ? "0 (landscape)" : o == 180 ? "180 (landscape, upside down)" : o == 90 ? "90 (portrait, native left edge up)"
	  : o == 270 ? "270 (portrait, native right edge up)" : "not clear");
	return o;
}
// ---------------------------------------------------------------- brightness gesture (watches touch, never holds it back)
// One finger rests in the bottom-right corner of the picture as the user sees it (a ~2 cm square in map() coordinates,
// so it follows the orientation and stays square), then two more fingers swipe vertically: down dims, up brightens, one
// picture height = 100 points, kept to 5..100 % so the panel never looks dead. It fires only with exactly 3 contacts
// down, the corner one down >= 500 ms and within 4 mm of where it landed, and the other two landed >= 500 ms after it:
// an ordinary three-finger gesture lands its fingers together, so it never fires. Until it fires every frame goes to
// the PC as usual; from then on touch_main consumes everything until all fingers are up.
#define GZ_MM 20   // the corner zone, each side
#define GS_MM 4    // the corner finger stays within this of where it landed
#define GH_MS 500  // the corner finger's hold, and how much later the other two land
enum { GEST_NO, GEST_START, GEST_ON, GEST_END };  // pass the frame on | cancel all at the PC | consume | all up: save
struct gest {
	int wmm, hmm;               // the upright picture's size in mm (set by the caller)
	int n, on, b0, y0, s0, s1;  // tracked contacts; active; brightness and the swipers' mean y at the start; their ids
	struct gc { int id, x0, y0, far; uint32_t t0; } c[ML_MAX_CONTACTS];
};
// One full-state frame (n = 0: all fingers up). *pct: in, the current brightness; out, the one to show (GEST_ON).
static int gest_feed(struct gest *g, const struct ml_contact *f, int n, uint32_t t, int *pct) {
	if (n <= 0) { int was = g->on; g->n = g->on = 0; return was ? GEST_END : GEST_NO; }
	struct gc k[ML_MAX_CONTACTS];
	int was = g->n;  // contacts in the frame before
	for (int i = 0; i < n; i++) {  // each contact's landing time and place, carried over by tracking id
		int j = 0;
		while (j < g->n && g->c[j].id != f[i].id) j++;
		k[i] = j < g->n ? g->c[j] : (struct gc){f[i].id, f[i].x, f[i].y, 0, t};
		int64_t dx = (int64_t)(f[i].x - k[i].x0) * g->wmm, dy = (int64_t)(f[i].y - k[i].y0) * g->hmm;
		if (dx * dx + dy * dy > (int64_t)GS_MM * GS_MM * 65535 * 65535) k[i].far = 1;
	}
	memcpy(g->c, k, n * sizeof *k);
	g->n = n;
	if (g->on) {  // follow the two swipers' mean y while both are down (one lifted: hold)
		int y = 0, m = 0;
		for (int i = 0; i < n; i++) if (f[i].id == g->s0 || f[i].id == g->s1) y += f[i].y, m++;
		if (m == 2) {
			int b = g->b0 - (y / 2 - g->y0) * 100 / 65535;
			*pct = b < 5 ? 5 : b > 100 ? 100 : b;
		}
		return GEST_ON;
	}
	// only the frame that grows to 3 (from 2, or from 1: two fingers can land in one frame), never 4+ dropping to 3
	// mid-touch, which would cancel a Windows gesture already under way
	if (n != 3 || was >= 3) return GEST_NO;
	for (int i = 0; i < 3; i++) {
		int a = (i + 1) % 3, b = (i + 2) % 3;
		if (k[i].far || t - k[i].t0 < GH_MS || f[i].x < 65535 - GZ_MM * 65535 / g->wmm || f[i].y < 65535 - GZ_MM * 65535 / g->hmm) continue;
		if ((int32_t)(k[a].t0 - k[i].t0) < GH_MS || (int32_t)(k[b].t0 - k[i].t0) < GH_MS) continue;
		g->on = 1; g->s0 = f[a].id; g->s1 = f[b].id; g->y0 = (f[a].y + f[b].y) / 2; g->b0 = *pct;
		return GEST_START;
	}
	return GEST_NO;
}
static int gfeed(struct gest *g, uint32_t t, int *pct, int n, ...) {  // selftest: n contacts as id, x, y
	struct ml_contact f[ML_MAX_CONTACTS] = {{0}};
	va_list ap;
	va_start(ap, n);
	for (int i = 0; i < n; i++) f[i].id = va_arg(ap, int), f[i].x = va_arg(ap, int), f[i].y = va_arg(ap, int);
	va_end(ap);
	return gest_feed(g, f, n, t, pct);
}
static int bright = 100, bright_save;  // atomic: the backlight level in %; the gesture ended (main saves state/brightness)
static void brightness_set(int pct, int quiet);
static int conf_parse(FILE *f, const char *path);
static int conf_str(const char *s) { FILE *f = fmemopen((void *)s, strlen(s), "r"); int r = conf_parse(f, "test"); fclose(f); return r; }

static int selftest(void) {
	int bad = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad = 1; } } while (0)
	CHECK(classify4(-540000, 5000, 870000) == 180);        // the pose Android called 180
	CHECK(classify4(900000, 20000, 400000) == 0);
	CHECK(classify4(30000, 900000, 400000) == 90);         // native left edge up
	CHECK(classify4(-30000, -900000, 400000) == 270);      // native right edge up
	CHECK(classify4(100000, 50000, 990000) == -1);         // flat on a desk
	CHECK(classify4(600000, 600000, 0) == -1);             // on a diagonal
	CHECK(classify4(0, 1600000, 0) == -1);                 // shaken

	// brightness gesture, landscape 346 x 194 mm: the corner zone is x >= 61748, y >= 58779; 4 mm = 758 x, 1351 y
	struct gest g = {.wmm = 346, .hmm = 194};
	int b, fired;
#define F(t, n, ...) (fired |= gfeed(&g, t, &b, n, __VA_ARGS__) != GEST_NO)
#define NOFIRE(seq) do { b = 50; fired = 0; seq; fired |= gfeed(&g, 9000, &b, 0) != GEST_NO; CHECK(!fired && b == 50); } while (0)
	NOFIRE(for (uint32_t t = 0; t <= 100; t += 10) F(t, 1, 0, 32000, 32000));   // a tap
	NOFIRE(for (uint32_t t = 0; t <= 100; t += 10) F(t, 1, 0, 64000, 62000));   // a tap in the corner
	NOFIRE(for (uint32_t t = 0; t <= 3000; t += 10) F(t, 1, 0, 64000, 62000));  // a corner long-press alone
	NOFIRE(for (uint32_t t = 0; t <= 1000; t += 10) F(t, 2, 0, 20000, 10000 + t * 30, 1, 30000, 10000 + t * 30));  // 2-finger scroll
	NOFIRE(for (uint32_t t = 0; t <= 1000; t += 10) F(t, 2, 0, 30000 - t * 10, 30000, 1, 34000 + t * 10, 30000));   // pinch
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // a three-finger swipe whose fingers land together, one in the corner
		F(t, 3, 0, 64000, 62000, 1, 20000, 10000 + t * 20, 2, 30000, 10000 + t * 20));
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // the same, landing 100 ms apart
		t < 100 ? F(t, 1, 0, 64000, 62000) : t < 200 ? F(t, 2, 0, 64000, 62000, 1, 20000, 10000 + t * 20)
		: F(t, 3, 0, 64000, 62000, 1, 20000, 10000 + t * 20, 2, 30000, 10000 + t * 20));
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // the corner finger held 300 ms, then a two-finger scroll
		t < 300 ? F(t, 1, 0, 64000, 62000) : F(t, 3, 0, 64000, 62000, 1, 20000, 10000 + t * 20, 2, 30000, 10000 + t * 20));
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // the corner finger slid 6 mm (inside the zone), then two land
		t < 600 ? F(t, 1, 0, 64000, 60000 + t * 2 < 62000 ? 60000 + t * 2 : 62000)
		: F(t, 3, 0, 64000, 62000, 1, 20000, 30000, 2, 30000, 30000));
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // 6 % in from the right: outside the landscape zone
		t < 600 ? F(t, 1, 0, 60000, 63000) : F(t, 3, 0, 60000, 63000, 1, 20000, 30000, 2, 30000, 30000));
	NOFIRE(for (uint32_t t = 0; t <= 1500; t += 10)  // corner held, three more land 600 ms later, one lifts: 4 -> 3
		t < 600 ? F(t, 1, 0, 64000, 62000) : t < 800 ? F(t, 4, 0, 64000, 62000, 1, 20000, 30000, 2, 30000, 30000, 3, 40000, 30000)
		: F(t, 3, 0, 64000, 62000, 1, 20000, 30000, 2, 30000, 30000));
	b = 50;  // corner held, then the two fingers land in separate frames (2 -> 3): fires on the third
	for (uint32_t t = 0; t < 700; t += 20) CHECK(gfeed(&g, t, &b, t < 600 ? 1 : 2, 0, 64000, 62000, 1, 20000, 30000) == GEST_NO);
	CHECK(gfeed(&g, 700, &b, 3, 0, 64000, 62000, 1, 20000, 30000, 2, 30000, 30000) == GEST_START);
	CHECK(gfeed(&g, 720, &b, 0) == GEST_END);
	// the gesture: corner finger 600 ms with < 1 mm of jitter, two fingers land, swipe; the corner finger lifts first
	b = 50; fired = 0;
	for (uint32_t t = 0; t < 600; t += 20) F(t, 1, 7, 64000 + t % 40 * 3, 62000);
	CHECK(!fired && gfeed(&g, 600, &b, 3, 7, 64000, 62000, 1, 20000, 40000, 2, 30000, 40000) == GEST_START && b == 50);
	CHECK(gfeed(&g, 620, &b, 3, 7, 64000, 62000, 1, 20000, 46554, 2, 30000, 46554) == GEST_ON && b == 40);  // down 10 %: dims
	CHECK(gfeed(&g, 640, &b, 3, 7, 64000, 62000, 1, 20000, 33446, 2, 30000, 33446) == GEST_ON && b == 60);  // up 10 %: brightens
	CHECK(gfeed(&g, 660, &b, 2, 1, 20000, 33446, 2, 30000, 33446) == GEST_ON && b == 60);      // the corner finger lifts: still on
	CHECK(gfeed(&g, 680, &b, 2, 1, 20000, 679, 2, 30000, 679) == GEST_ON && b == 100);          // up 60 %: clamped at 100
	CHECK(gfeed(&g, 700, &b, 1, 2, 30000, 60000) == GEST_ON && b == 100);                       // one swiper left: holds
	CHECK(gfeed(&g, 720, &b, 0) == GEST_END && b == 100);
	CHECK(gfeed(&g, 740, &b, 0) == GEST_NO);
	b = 20;  // dim past the bottom: clamped at 5
	for (uint32_t t = 0; t < 600; t += 20) gfeed(&g, t, &b, 1, 0, 64000, 62000);
	CHECK(gfeed(&g, 600, &b, 3, 0, 64000, 62000, 3, 20000, 40000, 4, 30000, 40000) == GEST_START);
	CHECK(gfeed(&g, 620, &b, 3, 0, 64000, 62000, 3, 20000, 65000, 4, 30000, 65000) == GEST_ON && b == 5);
	CHECK(gfeed(&g, 640, &b, 0) == GEST_END && b == 5);
	g = (struct gest){.wmm = 194, .hmm = 346};  // portrait: the zone is x >= 58779, y >= 61748, so 6 % in from the right fires
	b = 50;
	for (uint32_t t = 0; t < 600; t += 20) gfeed(&g, t, &b, 1, 0, 60000, 63000);
	CHECK(gfeed(&g, 600, &b, 3, 0, 60000, 63000, 1, 20000, 30000, 2, 30000, 30000) == GEST_START);
	CHECK(gfeed(&g, 620, &b, 0) == GEST_END);
#undef F
#undef NOFIRE

	// tabd.conf: every key, comments, blank lines; a line tabd doesn't understand fails the whole file
	CHECK(conf_str("# c\n\n pc = studio.local:47990 # PC\norient=auto\ninput = 0\nbitrate = 15000\nlinks = eth0 wlan0\n") == 0);
	CHECK(ml_host && !strcmp(ml_host, "studio.local") && ml_port == 47990 && !strcmp(opt_orient, "auto"));
	CHECK(opt_input == 0 && opt_bitrate == 15000);
	CHECK(conf_str("pc = auto\norient = 270\ninput = 1\n") == 0 && !ml_host && !ml_port && !strcmp(opt_orient, "270") && opt_input);
	CHECK(conf_str("pc = :48000\n") == 0 && !ml_host && ml_port == 48000);
	CHECK(conf_str("pc = studio:65535\n") == 0 && !strcmp(ml_host, "studio") && ml_port == 65535);
	CHECK(conf_str("pc = studio\n") == 0 && !strcmp(ml_host, "studio") && ml_port == 0);
	for (const char **p = (const char *[]){"pc =\n", "pc = studio:\n", "pc = studio:0\n", "pc = studio:65536\n",
			"pc = studio:4799x\n", "pc = studio:-1\n", "pc = a:b:47989\n", "pc = ::1\n", NULL}; *p; p++)
		if (conf_str(*p) == 0) { printf("FAIL: %s", *p); bad = 1; }
	CHECK(!strcmp(ml_host, "studio") && ml_port == 0);  // a refused value changes nothing
	CHECK(conf_str("orient = 45\n") < 0 && !strcmp(opt_orient, "270"));
	CHECK(conf_str("input = yes\n") < 0);
	CHECK(conf_str("bitrate = 18000k\n") < 0);
	CHECK(conf_str("brightness = 50\n") < 0);  // the old key: the gesture keeps it in state/ now
	CHECK(conf_str("connector = dsi\nbacklight = panel0\ntouch_driver = Goodix-TS\ntouch_parent = i2c-2/2-0014\n") == 0);
	CHECK(opt_connector == DRM_MODE_CONNECTOR_DSI && !strcmp(opt_backlight, "panel0") && !strcmp(opt_touch_parent, "i2c-2/2-0014"));
	CHECK(conf_str("links = eth0 wlan0\ncomposer = auto\nfixed_mac = 1\nadb_allow = auto\nwifi_country = XX\nwifi_freq = 5180\n") == 0);
	CHECK(conf_str("connector = HDMI\n") < 0 && conf_str("backlight = ../x\n") < 0 && conf_str("links2 = eth0\n") < 0);
	CHECK(conf_str("pc\n") < 0);
	printf("selftest %s\n", bad ? "FAILED" : "ok");
	return bad;
}

// ---------------------------------------------------------------- touch (goodix-ts: multi-touch protocol A with tracking ids)
static int xmax = 1280, ymax = 800;
static struct ml_contact map(int x, int y, int id) {  // raw -> picture coordinates of the frame on glass
	x = x < 0 ? 0 : x > xmax ? xmax : x;
	y = y < 0 ? 0 : y > ymax ? ymax : y;
	uint16_t nx = (uint32_t)x * 65535u / xmax, ny = (uint32_t)y * 65535u / ymax;
	uint16_t lx = nx, ly = ny;  // into the upright picture: the tablet is turned `orient` clockwise
	if (orient == 90) lx = 65535 - ny, ly = nx;
	else if (orient == 270) lx = ny, ly = 65535 - nx;
	else if (orient == 180) lx = 65535 - nx, ly = 65535 - ny;
	return (struct ml_contact){.id = (uint8_t)id, .x = lx, .y = ly};
}
// Ghost guard: a contact that hasn't moved more than 2 raw units for 60 s is dropped (a finger always jitters).
static int still60(int id, int x, int y) {
	static struct { int x, y; uint32_t moved, seen; } st[256];
	uint32_t t = now_ms();
	id &= 255;
	if (t - st[id].seen > 500 || abs(x - st[id].x) > 2 || abs(y - st[id].y) > 2)  // new contact, or it moved
		st[id].x = x, st[id].y = y, st[id].moved = t;
	st[id].seen = t;
	if (t - st[id].moved < 60000) return 0;
	LR("touch: dropping contact %d, still for %u s (ghost guard)", id, (t - st[id].moved) / 1000);
	return 1;
}
static void touch_send(const struct ml_contact *c, int n) {
	// full-state frame to Sunshine (ml diffs it; with input off it only logs). Not while the glass is stale: the user
	// can't see what they touch, and the main loop has cancelled every contact (a new frame re-sends them as DOWN).
	if (now_ms() - __atomic_load_n(&last_flip_ms, __ATOMIC_RELAXED) > 2000) return;
	ml_touch(c, n);
	__atomic_store_n(&ml_cancelled, 0, __ATOMIC_RELAXED);
}
static int touch_open(void) {
	char path[32];
	int fd = touch_find(path);
	if (fd < 0) return -1;
	struct input_absinfo ax = {0}, ay = {0};
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
	if (ax.maximum > 0) xmax = ax.maximum;
	if (ay.maximum > 0) ymax = ay.maximum;
	L("touch: %s range %dx%d (%s)", path, xmax, ymax, touch_pass ? "multi-touch, not a pointer" : "direct");
	return fd;
}
static void *touch_main(void *arg) {
	(void)arg; rt(10);
	// Limitation: protocol A only (the goodix driver has no ABS_MT_SLOT); add slot tracking if the kernel ever changes
	int fd = -1, nf = 0, down = 0, skip = 0, cx = 0, cy = 0, cid = -1, has = 0, consuming = 0;
	struct ml_contact fr[ML_MAX_CONTACTS];
	static struct gest gs;
	for (;;) {
		if (fd < 0 && (fd = touch_open()) < 0) { sleep(2); continue; }
		struct pollfd p = {fd, POLLIN, 0};
		int n = poll(&p, 1, 250);  // wakes up to check the ghost guard below even without touch events
		// Ghost guard: nothing on glass for 2 s (stream lost, decoder wedged) cancels every contact at Sunshine, so no
		// touch is held through an outage. Here, not in the main loop: this thread never waits on the decoder.
		if (!__atomic_load_n(&ml_cancelled, __ATOMIC_RELAXED) && now_ms() - __atomic_load_n(&last_flip_ms, __ATOMIC_RELAXED) > 2000) {
			ml_touch(NULL, -1);
			__atomic_store_n(&ml_cancelled, 1, __ATOMIC_RELAXED);
			if (down) L("touch: nothing on glass for 2 s, cancelled all contacts");
		}
		if (n <= 0) continue;
		struct input_event ev[64];
		ssize_t len = read(fd, ev, sizeof ev);
		if (len < 0 && errno == EAGAIN) continue;
		if (len <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { L("touch: device lost"); close(fd); fd = -1; continue; }
		for (ssize_t k = 0; k < len / (ssize_t)sizeof *ev; k++) {
			const struct input_event *e = &ev[k];
			if (e->type == EV_ABS) {
				if (e->code == ABS_MT_POSITION_X) cx = e->value, has |= 1;
				else if (e->code == ABS_MT_POSITION_Y) cy = e->value, has |= 2;
				else if (e->code == ABS_MT_TRACKING_ID) cid = e->value;
			} else if (e->type == EV_SYN) {
				if (e->code == SYN_DROPPED) { skip = 1; continue; }
				if ((e->code == SYN_MT_REPORT || e->code == SYN_REPORT) && has == 3 && nf < (int)ML_MAX_CONTACTS)
					if (!still60(cid >= 0 ? cid : nf, cx, cy)) fr[nf] = map(cx, cy, cid >= 0 ? cid : nf), nf++;
				if (e->code == SYN_MT_REPORT || e->code == SYN_REPORT) has = 0, cid = -1;
				if (e->code != SYN_REPORT) continue;
				if (skip) skip = 0;  // the frame cut by SYN_DROPPED is incomplete: drop it
				else if (nf) {
					if (!down && __atomic_load_n(&g_state, __ATOMIC_RELAXED) == G_ASLEEP) {  // wake touch: consumed
						consuming = 1;
						__atomic_store_n(&wake_ms, now_ms(), __ATOMIC_RELAXED);
						L("glass: touch wakes the panel (that touch isn't sent)");
					}
					down = 1;
					int b = __atomic_load_n(&bright, __ATOMIC_RELAXED), gr = GEST_NO;
					if (consuming != 1) {  // not a wake touch; the picture's mm set the corner zone and stillness
						gs.wmm = orient % 180 ? panel_hmm : panel_wmm; gs.hmm = orient % 180 ? panel_wmm : panel_hmm;
						gr = gest_feed(&gs, fr, nf, now_ms(), &b);
					}
					if (gr == GEST_START) {
						consuming = 2;
						ml_touch(NULL, -1), __atomic_store_n(&ml_cancelled, 1, __ATOMIC_RELAXED);  // CANCEL: no click
						L("touch: brightness gesture: every contact cancelled at the PC, touch consumed until all fingers lift");
					} else if (gr == GEST_ON && b != __atomic_load_n(&bright, __ATOMIC_RELAXED)) brightness_set(b, 1);
					else if (!consuming) touch_send(fr, nf);
				} else if (down) {
					int b = 0;
					down = 0;
					if (gest_feed(&gs, fr, 0, now_ms(), &b) == GEST_END) {
						L("touch: brightness gesture ended at %d%%", __atomic_load_n(&bright, __ATOMIC_RELAXED));
						__atomic_store_n(&bright_save, 1, __ATOMIC_RELAXED);
					}
					if (consuming) consuming = 0;
					else touch_send(fr, 0);
				}
				nf = 0;
			}
		}
	}
	return NULL;
}

// ---------------------------------------------------------------- libml (Sunshine) source
static int ml_video(void *ctx, const uint8_t *data, int len, uint32_t frame, int key, int codec, uint32_t host_pts_ms, int64_t rx_us) {
	(void)ctx; (void)host_pts_ms;  // Sunshine's own clock: rx_us times the frame on the tablet's clock instead
	if (len <= 0 || (codec != ML_HEVC && codec != ML_H264)) return 0;
	struct vin v = {.data = data, .len = (uint32_t)len, .frame = frame, .codec = codec, .key = key, .rx_us = rx_us};
	return video_in(&v);  // -1: dropped waiting for a keyframe, so ml asks Sunshine for an IDR
}
static void ml_state(void *ctx, int state, const char *detail) {  // libml logs the states itself, except the PIN
	(void)ctx;
	if (state == ML_PAIRING) {  // the PIN is on the glass only, never in the log
		L("ml: PAIRING: the glass shows a PIN; type it into Sunshine's web UI (PIN tab) for client \"tablet\"");
		snprintf(pin_text, sizeof pin_text, "%s", detail ? detail : "");
		__atomic_store_n(&pin_active, 1, __ATOMIC_RELAXED);
	}
	if (state == ML_LIVE) __atomic_store_n(&pin_active, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ml_live, state == ML_LIVE, __ATOMIC_RELAXED);
	if (state == ML_LIVE) { pthread_mutex_lock(&mu); reset_ms = now_ms() | 1; pthread_mutex_unlock(&mu); }  // time to glass
}

// ---------------------------------------------------------------- housekeeping: own log, wakelock, self-watchdog
static void log_open(void) {
	int fd = open(opt_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd >= 0) { dup2(fd, 2); close(fd); }
}
static void log_check(void) {  // rotate at 1 MB, keeping one old file
	struct stat st;
	if (!opt_log || fstat(2, &st) || st.st_size < (1 << 20)) return;
	char old[256];
	snprintf(old, sizeof old, "%s.1", opt_log);
	rename(opt_log, old);
	log_open();
	L("log: rotated, previous in %s", old);
}
// A kernel wakelock, so nothing suspends the tablet while tabd is the display (Android's own wakelocks die with `stop`).
static int wake(const char *file) {
	int fd = open(file, O_WRONLY | O_CLOEXEC), ok = fd >= 0 && write(fd, "tabd", 4) == 4;
	if (fd >= 0) close(fd);
	return ok;
}
static void bl_on_raw(void) {  // async-signal-safe: never leave the panel dark behind us
	int fd = *bl_power ? open(bl_power, O_WRONLY | O_CLOEXEC) : -1;
	if (fd >= 0) { if (write(fd, "0", 1) != 1) {} close(fd); }
}
static void on_term(int sig) { (void)sig; wake("/sys/power/wake_unlock"); bl_on_raw(); _exit(0); }  // async-signal-safe only

// SIGUSR1: the network path changed (wifi.sh's watcher: the Ethernet cable went out or forwards again), so reconnect
// the stream now instead of after the stream's own ~10 s loss timeout
static volatile sig_atomic_t net_kick;
static void on_usr1(int sig) { (void)sig; net_kick = 1; }
static void bl_find(void) {  // backlight = NAME, else the first entry of /sys/class/backlight by name
	char name[64] = "";
	DIR *d = opendir("/sys/class/backlight");
	for (struct dirent *e; d && (e = readdir(d)); )
		if (e->d_name[0] != '.' && (!*name || strcmp(e->d_name, name) < 0)) snprintf(name, sizeof name, "%.63s", e->d_name);
	if (d) closedir(d);
	if (*opt_backlight) snprintf(name, sizeof name, "%s", opt_backlight);
	if (!*name) { L("backlight: none in /sys/class/backlight (brightness and sleep off)"); return; }  // paths stay empty
	snprintf(bl_power, sizeof bl_power, "/sys/class/backlight/%s/bl_power", name);
	snprintf(bl_bright, sizeof bl_bright, "/sys/class/backlight/%s/brightness", name);
	snprintf(bl_maxp, sizeof bl_maxp, "/sys/class/backlight/%s/max_brightness", name);
	L("backlight: /sys/class/backlight/%s", name);
}
static int bl_read(const char *file) {  // a number from a backlight sysfs file, -1 if none
	int v = -1;
	FILE *f = *file ? fopen(file, "r") : NULL;
	if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
	return v;
}
static int bl_max;  // atomic: the panel's max_brightness, read on first use (again only while it can't be read)
static int bl_max_get(void) {
	int m = __atomic_load_n(&bl_max, __ATOMIC_RELAXED);
	if (m <= 0) __atomic_store_n(&bl_max, m = bl_read(bl_maxp), __ATOMIC_RELAXED);
	return m;
}
// quiet: the gesture, which logs where it ends; its failures are logged at most once a second (it retries every frame)
#define BL_ERR(...) do { if (quiet) LR(__VA_ARGS__); else L(__VA_ARGS__); } while (0)
static void brightness_set(int pct, int quiet) {
	int max = bl_max_get(), fd = -1, n;
	char v[16];
	if (max <= 0) { BL_ERR("config: brightness: no backlight max_brightness"); return; }
	n = snprintf(v, sizeof v, "%d", (max * pct + 50) / 100);
	if ((fd = open(bl_bright, O_WRONLY | O_CLOEXEC)) < 0 || write(fd, v, n) != n)
		BL_ERR("config: brightness: %s", strerror(errno));
	else {
		__atomic_store_n(&bright, pct, __ATOMIC_RELAXED);
		if (!quiet) L("config: brightness %d%% (%s of %d)", pct, v, max);
	}
	if (fd >= 0) close(fd);
}
// state/: what tabd remembers by itself, one number per file. tabd never writes tabd.conf.
static int state_get(const char *name) {  // -1 if none
	char p[64];
	snprintf(p, sizeof p, D "/state/%s", name);
	return bl_read(p);
}
static void state_put(const char *name, int v) {  // a temp file, then rename: a power cut keeps the old value or the new
	char p[64], tmp[72];
	snprintf(p, sizeof p, D "/state/%s", name);
	snprintf(tmp, sizeof tmp, "%s.tmp", p);
	mkdir(D "/state", 0755);
	FILE *f = fopen(tmp, "w");
	int bad = !f || fprintf(f, "%d\n", v) < 0 || fflush(f) || fsync(fileno(f));
	if (f) bad |= fclose(f);
	if (bad || rename(tmp, p)) L("state: saving %s: %s", p, strerror(errno));
}
static int pc_set(const char *v) {  // HOST[:PORT], :PORT or auto; -1: empty, a second ':', or a port not 1..65535
	static char host[64];
	char buf[64], *p;
	long port = 0;
	if (!*v || snprintf(buf, sizeof buf, "%s", v) >= (int)sizeof buf) return -1;
	if ((p = strchr(buf, ':'))) {
		*p++ = 0;
		if (!*p || strspn(p, "0123456789") != strlen(p) || strlen(p) > 5 || (port = strtol(p, NULL, 10)) < 1 || port > 65535) return -1;
	}
	memcpy(host, buf, sizeof host);
	ml_port = (int)port;
	ml_host = *host && strcmp(host, "auto") ? host : NULL;
	return 0;
}
static char *trim(char *s) {
	s += strspn(s, " \t\r\n");
	for (char *e = s + strlen(s); e > s && strchr(" \t\r\n", e[-1]); ) *--e = 0;
	return s;
}
static int script_key(const char *k) {  // read by boot.sh, net.sh and wifi.sh; tabd only accepts them
	static const char *ks[] = {"links", "composer", "fixed_mac", "adb_allow", "wifi_country", "wifi_freq", "wifi_bssid"};
	for (unsigned i = 0; i < sizeof ks / sizeof *ks; i++) if (!strcmp(k, ks[i])) return 1;
	return 0;
}
static long conn_type(const char *v) {  // connector = eDP|DSI|LVDS|DPI -> DRM_MODE_CONNECTOR_*, 0 if none of those
	return !strcasecmp(v, "eDP") ? DRM_MODE_CONNECTOR_eDP : !strcasecmp(v, "DSI") ? DRM_MODE_CONNECTOR_DSI
	     : !strcasecmp(v, "LVDS") ? DRM_MODE_CONNECTOR_LVDS : !strcasecmp(v, "DPI") ? DRM_MODE_CONNECTOR_DPI : 0;
}
static int conf_parse(FILE *f, const char *path) {  // -1: a line tabd doesn't understand (main exits 2, boot.sh retries)
	char line[256], *k, *v, *end;
	int bad = 0;
	for (int n = 1; fgets(line, sizeof line, f); n++) {
		if ((end = strchr(line, '#'))) *end = 0;
		if (!*(k = trim(line))) continue;
		if (!(v = strchr(k, '='))) { L("config: %s line %d: no \"key = value\"", path, n); bad = 1; continue; }
		*v = 0; k = trim(k); v = trim(v + 1);
		long x = strtol(v, &end, 10);
		int num = *v && !*end;
		if (!strcmp(k, "pc") && !pc_set(v)) continue;
		if (!strcmp(k, "orient") && (!strcmp(v, "auto") || (num && (x == 0 || x == 90 || x == 180 || x == 270))))
			snprintf(opt_orient, sizeof opt_orient, "%s", v);
		else if (!strcmp(k, "input") && num && (x == 0 || x == 1)) opt_input = (int)x;
		else if (!strcmp(k, "bitrate") && num && x >= 500 && x <= 200000) opt_bitrate = (int)x;
		else if (!strcmp(k, "connector") && (x = conn_type(v))) opt_connector = (int)x;
		else if (!strcmp(k, "touch_driver") && strlen(v) < sizeof opt_touch_driver) strcpy(opt_touch_driver, v);
		else if (!strcmp(k, "touch_parent") && strlen(v) < sizeof opt_touch_parent) strcpy(opt_touch_parent, v);
		else if (!strcmp(k, "backlight") && *v && !strchr(v, '/') && strlen(v) < sizeof opt_backlight) strcpy(opt_backlight, v);
		else if (script_key(k)) continue;
		else { L("config: %s line %d: %s = %s: an unknown key, or a value it does not take", path, n, k, v); bad = 1; }
	}
	return bad ? -1 : 0;
}
static int conf_load(const char *path) {  // a missing file means the defaults
	FILE *f = fopen(path, "r");
	if (!f) { L("config: %s: %s (defaults)", path, strerror(errno)); return 0; }
	int r = conf_parse(f, path);
	fclose(f);
	return r;
}
static void *watchdog_main(void *arg) {  // a wedged loop (e.g. MPP never returning) exits, so run.sh restarts tabd
	(void)arg;
	for (;;) {
		sleep(1);
		uint32_t t = now_ms(), m = t - __atomic_load_n(&main_tick, __ATOMIC_RELAXED), d = t - __atomic_load_n(&disp_tick, __ATOMIC_RELAXED);
		if (m > 3000 || d > 3000) { L("watchdog: %s loop stuck for %u ms, exiting", m > 3000 ? "main" : "display", m > d ? m : d); _exit(3); }
	}
	return NULL;
}

// ---------------------------------------------------------------- main: options, then the housekeeping loop
int main(int argc, char **argv) {
	const char *conf = D "/tabd.conf";
	for (int i = 1; i < argc; i++) {  // first: what decides where tabd reads and logs, and the tests that exit at once
		if (!strcmp(argv[i], "--selftest")) return selftest();
		if (!strcmp(argv[i], "--detect")) { int o = detect_orient(); printf("orientation %d\n", o); return o < 0; }  // check the pose
		if (!strcmp(argv[i], "--glass-test")) {  // render the glass states into D/glass-*.argb (panel BGRA)
			static const int os[] = {0, 270}, ss[] = {G_CONNECTING, G_PIN, G_SILENT, G_NOSIGNAL};
			hdisp = 1920; vdisp = 1080; ovl_pitch = hdisp * 4;  // the reference's panel
			ovl_map = malloc(ovl_pitch * vdisp);
			snprintf(pin_text, sizeof pin_text, "8266");
			for (unsigned a = 0; a < 2; a++)
				for (unsigned b = 0; b < 4; b++) {
					char f[64];
					orient = os[a];
					glass_draw(ss[b], 3, 1);
					snprintf(f, sizeof f, D "/glass-%d-%d.argb", os[a], ss[b]);
					FILE *o = fopen(f, "wb");
					if (o) fwrite(ovl_map, ovl_pitch, vdisp, o), fclose(o);
				}
			return 0;
		}
		if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = argv[++i];
		else if (!strcmp(argv[i], "--log") && i + 1 < argc) opt_log = argv[++i];
	}
	if (opt_log) log_open(), log_check();  // at every start too, so a crash loop's lines (boot.sh's, the scripts') rotate
	if (conf_load(conf)) return 2;
	for (int i = 1; i < argc; i++) {  // then the rest, over tabd.conf
		if (!strcmp(argv[i], "--conf") || !strcmp(argv[i], "--log")) i++;
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) opt_dump = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--margin") && i + 1 < argc) margin_us = (int64_t)(atof(argv[++i]) * 1000);
		else if (!strcmp(argv[i], "--fault") && i + 1 < argc) opt_fault = argv[++i];
		else {  // an unknown option never starts the daemon (an old binary must not run as a second tabd)
			L("tabd: unknown option %s (see the usage at the top of tabd.c)", argv[i]);
			return 2;
		}
	}
	// Sunshine streams the upright picture: tabd locks which way the tablet is turned, once, and turns the frames
	int saved = state_get("orient"), o = -1;
	if (saved != 90 && saved != 180 && saved != 270) saved = 0;  // nothing saved yet: the panel's native orientation
	if (strcmp(opt_orient, "auto")) o = atoi(opt_orient);
	else if ((o = detect_orient()) < 0) L("orient: not clear, using the last one, %d", o = saved);
	orient = o == 90 || o == 180 || o == 270 ? o : 0;
	ml_relaunch = saved % 180 != orient % 180;  // portrait <-> landscape since last run: fresh launch, not resume
	state_put("orient", orient);
	L("orient: locked at %d (%s) until tabd restarts", orient, strcmp(opt_orient, "auto") ? "set" : "auto");
	bl_find();  // before the handlers: on_term writes bl_power
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, SIG_IGN);  // settings are read once; restart tabd to apply new ones
	signal(SIGUSR1, on_usr1);
	L("tabd: margin %lld us%s%s", (long long)margin_us, *opt_fault ? ", FAULT INJECTION: " : "", opt_fault);
	if (!wake("/sys/power/wake_lock")) L("wakelock: /sys/power/wake_lock not writable: %s", strerror(errno));
	main_tick = disp_tick = start_ms = now_ms();
	bl_on_raw();  // a previous tabd may have died with the panel asleep
	int bcur = bl_read(bl_bright), bmax = bl_max_get();
	if (bcur >= 0 && bmax > 0) bright = (bcur * 100 + bmax / 2) / bmax;  // the panel's level, until the gesture saves one
	int bs = state_get("brightness");  // 0..100 % of max_brightness; the glass states switch only bl_power, so it stays
	if (bs >= 0 && bs <= 100) brightness_set(bs, 0);
	kick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (mpp_load()) return 1;
	pthread_t th;
	pthread_create(&th, NULL, out_main, NULL);
	pthread_create(&th, NULL, disp_main, NULL);
	pthread_create(&th, NULL, touch_main, NULL);
	pthread_create(&th, NULL, watchdog_main, NULL);
	// the stream's size and refresh are the panel's: wait for its mode (the display thread retries every 0.5 s)
	for (uint32_t t0 = now_ms(); !__atomic_load_n(&drm_ready, __ATOMIC_ACQUIRE); usleep(10000)) {
		__atomic_store_n(&main_tick, now_ms(), __ATOMIC_RELAXED);
		if (now_ms() - t0 > 15000) { L("drm: no display after 15 s, exiting (boot.sh restarts tabd)"); return 1; }
	}
	struct ml_config mc = {.dir = D "/ml", .host = ml_host, .port = ml_port, .name = "tablet", .input = opt_input, .bitrate_kbps = opt_bitrate,
		// portrait asks for the panel turned (e.g. 1080x1920); sops = 1 lets Sunshine shape its output (the tablet's own
		// virtual monitor) to it
		.width = orient % 180 ? vdisp : hdisp, .height = orient % 180 ? hdisp : vdisp, .refresh_x100 = refresh_x100, .sops = 1,
		.relaunch = ml_relaunch};
	struct ml_callbacks cb = {.video = ml_video, .state = ml_state};
	char br[24];
	snprintf(br, sizeof br, opt_bitrate ? "%d kbps" : "libml's default bitrate", opt_bitrate);
	L("ml: source is Sunshine at %s:%d, %dx%d at %d.%02d Hz, %s (orientation %d), INPUT %s", ml_host ? ml_host : "saved/mDNS",
	  ml_port ? ml_port : 47989, mc.width, mc.height, refresh_x100 / 100, refresh_x100 % 100, br, orient,
	  opt_input ? "ON: touches reach Windows" : "off (touches logged only)");
	ml_start(&mc, &cb);
	rt(10);

	uint32_t last_key = 0, last_status = now_ms(), last_logchk = 0;
	for (;;) {
		usleep(50000);
		uint32_t t = now_ms();
		__atomic_store_n(&main_tick, t, __ATOMIC_RELAXED);
		if (t - last_logchk >= 1000) { last_logchk = t; log_check(); }
		if (__atomic_exchange_n(&bright_save, 0, __ATOMIC_RELAXED)) {  // not on the touch thread
			int b = __atomic_load_n(&bright, __ATOMIC_RELAXED);
			state_put("brightness", b);
			L("state: brightness %d%% saved", b);
		}
		if (net_kick) { net_kick = 0; L("network: the link changed (SIGUSR1), reconnecting the stream"); ml_reconnect(); }
		if (stall_since && now_us() - stall_since > 2000000) {
			pthread_mutex_lock(&in_mu);  // the decoder input may be on libml's thread
			if (stall_since && now_us() - stall_since > 2000000) {
				L("decoder: no output for 2 s, reopening");
				dec_close();
				__atomic_store_n(&need_key, 1, __ATOMIC_RELAXED);
			}
			pthread_mutex_unlock(&in_mu);
		}
		if (__atomic_load_n(&ml_live, __ATOMIC_RELAXED) && __atomic_load_n(&need_key, __ATOMIC_RELAXED) && t - last_key >= 250) {
			ml_request_idr();  // the decoder waits for a keyframe: ask Sunshine
			ADD(keyreqs, 1); last_key = t;
		}
		if (t - last_status >= 10000) {
			int32_t v[RN]; uint16_t g50, g95, t50, t95;
			pthread_mutex_lock(&mu);
			pct(&r_rxg, t, &g50, &g95); pct(&r_rga, t, &t50, &t95);
			L("status: fps %d shown %u dropped %u late %u keyreqs %u rx-to-glass %u/%u ms rga %u/%u us", rwin(&r_flip, t, v),
			  shown_total, dropped_total, late_flips, keyreqs, g50, g95, t50, t95);
			pthread_mutex_unlock(&mu);
			last_status = t;
		}
	}
}
