// gs.h: GameStream/Sunshine control path (HTTP 47989 + HTTPS 47984 over mbedTLS). Used by ml.c and mlprobe.
// Every call blocks; errors return -1 with a reason in g->err (never secrets).
#pragma once

struct gs_info {
	char appversion[32], gfeversion[32];
	int codecs;        // ServerCodecModeSupport (SCM_* bits)
	int currentgame;   // running app id, 0 = none
	int paired;        // our client cert is paired (only trustworthy over HTTPS)
};

struct gs;  // opaque: client identity, pinned server cert, PC address

// Loads the client cert/key from dir (creating dir 0700), or generates an RSA-2048 self-signed pair once (files 0600).
struct gs *gs_open(const char *dir);
void gs_close(struct gs *g);
const char *gs_err(struct gs *g);
const char *gs_host(struct gs *g);

// Picks the PC: host if given, else the saved address, else mDNS _nvstream._tcp. port = HTTP port, 0 = 47989.
// The HTTPS port comes from serverinfo (HttpsPort); RTSP and stream ports come from the launch reply and RTSP.
int gs_find(struct gs *g, const char *host, int port);
void gs_save_host(struct gs *g);   // remember the current address (after a good session)

int gs_serverinfo(struct gs *g, struct gs_info *i);   // HTTPS when paired, else HTTP
// Pairs with a 4-digit PIN the user types into Sunshine's web UI. Blocks until they do (up to 5 min).
int gs_pair(struct gs *g, const char *pin, const char *name);  // name: shown in Sunshine's paired devices
int gs_app_id(struct gs *g, const char *title);       // id of the app with this exact title, or -1
// /launch (resume = 0) or /resume. rikey/rikeyid = remote input AES key and IV id. url gets sessionUrl0 ("" if none).
// sops = 1 asks Sunshine to set its capture display to w x h (see ml_config.sops).
int gs_launch(struct gs *g, int appid, int resume, int w, int h, int fps, int surround_info, int sops,
              const unsigned char rikey[16], int rikeyid, char *url, int urlsz);
int gs_cancel(struct gs *g);  // quit the running app
void gs_abort(struct gs *g);  // from another thread: fail the request in flight now (e.g. a pairing wait)

// For tests: text between <tag> and </tag> after p, or -1.
int gs_xml(const char *xml, const char *tag, char *out, int n);
