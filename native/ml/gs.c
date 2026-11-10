// gs.c: GameStream/Sunshine control path over mbedTLS. See gs.h. Protocol as in moonlight-embedded's libgamestream
// and moonlight-qt's NvPairingManager (SHA-256 / AES-128-ECB pairing, i.e. appversion 7+, which Sunshine reports).
#define _GNU_SOURCE
#define MBEDTLS_ALLOW_PRIVATE_ACCESS  // the certificate signature bytes (crt.sig) are part of the pairing hashes
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include "mbedtls/aes.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "Limelight.h"
#include "gs.h"

#define HTTP_PORT 47989  // Sunshine default; its HTTPS port is HTTP - 5 unless serverinfo says otherwise
#define UNIQUEID "0123456789ABCDEF"  // what Moonlight sends; Sunshine tells clients apart by certificate

struct gs {
	char dir[200], host[80], err[160];
	int http_port, https_port, unauth;   // unauth: the last HTTPS call was refused as unpaired (vs a network failure)
	int fd;                   // the request in flight, for gs_abort
	mbedtls_entropy_context ent;
	mbedtls_ctr_drbg_context rng;
	mbedtls_x509_crt crt, srv;  // our client cert; the server cert pinned at pairing
	mbedtls_pk_context key;
	int have_srv;
	char *cert_pem;
};

static int fail(struct gs *g, const char *fmt, ...) {
	va_list a; va_start(a, fmt); vsnprintf(g->err, sizeof g->err, fmt, a); va_end(a);
	return -1;
}
const char *gs_err(struct gs *g) { return g->err; }
const char *gs_host(struct gs *g) { return g->host; }

static void hex(const unsigned char *b, size_t n, char *out) {
	for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
	out[2 * n] = 0;
}
static int unhex(const char *s, unsigned char *out, int max) {
	int n = (int)strlen(s) / 2;
	if (n > max) return -1;
	for (int i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = v; }
	return n;
}
static void rnd(struct gs *g, unsigned char *b, size_t n) { mbedtls_ctr_drbg_random(&g->rng, b, n); }
static void sha256(const unsigned char *b, size_t n, unsigned char out[32]) { mbedtls_sha256(b, n, out, 0); }
static void file(struct gs *g, char *out, const char *name) { snprintf(out, 256, "%s/%s", g->dir, name); }

static int write_file(const char *p, const void *d, size_t n) {  // 0600, replaced atomically
	char tmp[264];
	snprintf(tmp, sizeof tmp, "%s.tmp", p);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return -1;
	fchmod(fd, 0600);
	int ok = write(fd, d, n) == (ssize_t)n && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, p)) { unlink(tmp); return -1; }
	return 0;
}
static char *read_file(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	char *b = calloc(1, 65536);
	size_t n = fread(b, 1, 65535, f);
	fclose(f);
	b[n] = 0;
	return b;
}
static void mkdirs(const char *d) {
	char p[200];
	snprintf(p, sizeof p, "%s", d);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
	mkdir(p, 0700);
	chmod(p, 0700);
}

// ---------------------------------------------------------------- client identity
static int gen_identity(struct gs *g, const char *cp, const char *kp) {
	mbedtls_pk_context k;
	mbedtls_x509write_cert w;
	mbedtls_pk_init(&k);
	mbedtls_x509write_crt_init(&w);
	unsigned char *buf = calloc(1, 16384);
	int r = mbedtls_pk_setup(&k, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
	if (!r) r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(k), mbedtls_ctr_drbg_random, &g->rng, 2048, 65537);
	if (!r) r = mbedtls_pk_write_key_pem(&k, buf, 16384);
	if (!r) r = write_file(kp, buf, strlen((char *)buf));
	mbedtls_platform_zeroize(buf, 16384);
	if (!r) {
		mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
		mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
		mbedtls_x509write_crt_set_subject_key(&w, &k);
		mbedtls_x509write_crt_set_issuer_key(&w, &k);
		r = mbedtls_x509write_crt_set_subject_name(&w, "CN=NVIDIA GameStream Client") ||
		    mbedtls_x509write_crt_set_issuer_name(&w, "CN=NVIDIA GameStream Client") ||
		    mbedtls_x509write_crt_set_serial_raw(&w, (unsigned char *)"\x01", 1) ||
		    mbedtls_x509write_crt_set_validity(&w, "20000101000000", "20991231235959") ||
		    mbedtls_x509write_crt_pem(&w, buf, 16384, mbedtls_ctr_drbg_random, &g->rng) ||
		    write_file(cp, buf, strlen((char *)buf));
	}
	free(buf);
	mbedtls_x509write_crt_free(&w);
	mbedtls_pk_free(&k);
	return r ? -1 : 0;
}

struct gs *gs_open(const char *dir) {
	struct gs *g = calloc(1, sizeof *g);
	snprintf(g->dir, sizeof g->dir, "%s", dir);
	g->http_port = HTTP_PORT;
	g->https_port = HTTP_PORT - 5;
	g->fd = -1;
	psa_crypto_init();
	mbedtls_entropy_init(&g->ent);
	mbedtls_ctr_drbg_init(&g->rng);
	mbedtls_x509_crt_init(&g->crt);
	mbedtls_x509_crt_init(&g->srv);
	mbedtls_pk_init(&g->key);
	char cp[256], kp[256], sp[256];
	file(g, cp, "client.pem"); file(g, kp, "client.key"); file(g, sp, "server.pem");
	if (mbedtls_ctr_drbg_seed(&g->rng, mbedtls_entropy_func, &g->ent, (const unsigned char *)"tabd-ml", 7)) {
		fprintf(stderr, "ml: no entropy\n");
		goto bad;
	}
	mkdirs(dir);
	if (access(cp, R_OK) || access(kp, R_OK)) {
		fprintf(stderr, "ml: generating the client certificate in %s (once)\n", dir);
		if (gen_identity(g, cp, kp)) { fprintf(stderr, "ml: can't write the client certificate to %s\n", dir); goto bad; }
	}
	if (mbedtls_x509_crt_parse_file(&g->crt, cp) || mbedtls_pk_parse_keyfile(&g->key, kp, NULL, mbedtls_ctr_drbg_random, &g->rng) ||
	    !(g->cert_pem = read_file(cp))) {
		fprintf(stderr, "ml: unreadable client certificate in %s\n", dir);
		goto bad;
	}
	g->have_srv = mbedtls_x509_crt_parse_file(&g->srv, sp) == 0;
	return g;
bad:
	gs_close(g);
	return NULL;
}

void gs_close(struct gs *g) {
	if (!g) return;
	mbedtls_pk_free(&g->key);
	mbedtls_x509_crt_free(&g->crt);
	mbedtls_x509_crt_free(&g->srv);
	mbedtls_ctr_drbg_free(&g->rng);
	mbedtls_entropy_free(&g->ent);
	free(g->cert_pem);
	free(g);
}

// ---------------------------------------------------------------- HTTP(S)
static int tcp(struct gs *g, int port, int timeout_ms) {
	// Limitation: 500 ms connect timeout, LAN only (a SYN-ACK takes < 1 ms; a dead PC must not stall the retry loop)
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *ai, *a;
	char ps[8];
	snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(g->host, ps, &hints, &ai)) return fail(g, "can't resolve %s", g->host);
	int fd = -1;
	for (a = ai; a; a = a->ai_next) {
		fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (fd < 0) continue;
		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) {
			struct pollfd p = {fd, POLLOUT, 0};
			int e = 1;
			socklen_t l = sizeof e;
			if (poll(&p, 1, 500) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) && !e) break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd < 0) return fail(g, "%s:%d unreachable", g->host, port);
	fcntl(fd, F_SETFL, 0);
	struct timeval tv = {timeout_ms / 1000, timeout_ms % 1000 * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	return fd;
}

// GET pathq; returns the malloc'd response body, or NULL with g->err set.
static char *get(struct gs *g, int https, const char *pathq, int timeout_ms) {
	char req[8192];
	int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", pathq, g->host);
	if (rl >= (int)sizeof req) { fail(g, "request too long"); return NULL; }
	int fd = tcp(g, https ? g->https_port : g->http_port, timeout_ms);
	if (fd < 0) return NULL;
	__atomic_store_n(&g->fd, fd, __ATOMIC_SEQ_CST);
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_net_context net = {.fd = fd};
	char *buf = NULL, *body = NULL;
	size_t len = 0, cap = 0;
	int r;
	if (https) {
		if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) goto out;
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);  // the PC's cert is self-signed: pinned below
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g->rng);
		mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);  // TLS 1.2 on purpose: it skips TLS 1.3 ticket handling
		if (mbedtls_ssl_conf_own_cert(&conf, &g->crt, &g->key) || mbedtls_ssl_setup(&ssl, &conf)) goto out;
		mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
		while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
		if (r) { fail(g, "TLS handshake with %s failed (-0x%04x)", g->host, -r); goto out; }
		const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
		if (!g->have_srv || !peer || peer->raw.len != g->srv.raw.len || memcmp(peer->raw.p, g->srv.raw.p, peer->raw.len)) {
			g->unauth = 1;
			fail(g, "the PC's certificate is not the one we paired with");
			goto out;
		}
	}
	for (int off = 0; off < rl; off += r) {
		r = https ? mbedtls_ssl_write(&ssl, (unsigned char *)req + off, rl - off) : (int)send(fd, req + off, rl - off, MSG_NOSIGNAL);
		if (r <= 0) { fail(g, "send to %s failed", g->host); goto out; }
	}
	char *eoh = NULL;
	for (;;) {
		if (len + 4097 > cap) {
			cap = cap ? cap * 2 : 16384;
			if (cap > (1 << 20)) { fail(g, "response too large"); goto out; }
			buf = realloc(buf, cap);
		}
		r = https ? mbedtls_ssl_read(&ssl, (unsigned char *)buf + len, cap - len - 1) : (int)recv(fd, buf + len, cap - len - 1, 0);
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
		if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
		if (r < 0) { fail(g, "no reply from %s", g->host); goto out; }
		len += r;
		buf[len] = 0;
		if ((eoh = strstr(buf, "\r\n\r\n"))) {
			const char *cl = strcasestr(buf, "\r\nContent-Length:");
			if (cl && cl < eoh && len >= (size_t)(eoh + 4 - buf) + strtoul(cl + 17, NULL, 10)) break;
		}
	}
	if (!buf || !(eoh = strstr(buf, "\r\n\r\n")) || strncmp(buf, "HTTP/1.", 7)) { fail(g, "bad reply from %s", g->host); goto out; }
	if (atoi(buf + 9) != 200) { fail(g, "HTTP %d from %s", atoi(buf + 9), g->host); goto out; }
	body = strdup(eoh + 4);
out:
	if (https) mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	__atomic_store_n(&g->fd, -1, __ATOMIC_SEQ_CST);
	close(fd);
	free(buf);
	return body;
}

// Limitation: racy by one close(); only ml_stop calls it, on the way out.
void gs_abort(struct gs *g) {
	int fd = __atomic_load_n(&g->fd, __ATOMIC_SEQ_CST);
	if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

int gs_xml(const char *x, const char *tag, char *out, int n) {
	char o[64], c[64];
	snprintf(o, sizeof o, "<%s>", tag);
	snprintf(c, sizeof c, "</%s>", tag);
	const char *a = strstr(x, o), *b;
	if (!a || !(b = strstr(a += strlen(o), c)) || b - a >= n) return -1;
	memcpy(out, a, b - a);
	out[b - a] = 0;
	return (int)(b - a);
}

// GET /verb?uniqueid=..&uuid=..&<args>, checking the XML root's status_code.
static char *call(struct gs *g, int https, int timeout_ms, const char *verb, const char *fmt, ...) {
	char args[6144], pq[8192], uh[33];
	unsigned char u[16];
	va_list a;
	va_start(a, fmt);
	int n = vsnprintf(args, sizeof args, fmt, a);
	va_end(a);
	if (n >= (int)sizeof args) { fail(g, "request too long"); return NULL; }
	rnd(g, u, sizeof u);
	hex(u, sizeof u, uh);
	snprintf(pq, sizeof pq, "/%s?uniqueid=%s&uuid=%s%s%s", verb, UNIQUEID, uh, *args ? "&" : "", args);
	g->unauth = 0;
	char *x = get(g, https, pq, timeout_ms);
	if (!x) return NULL;
	const char *sc = strstr(x, "status_code=\"");
	if (sc && atoi(sc + 13) != 200) {
		const char *m = strstr(x, "status_message=\"");
		char msg[100] = "";
		if (m) sscanf(m + 16, "%99[^\"]", msg);
		g->unauth = atoi(sc + 13) == 401;
		fail(g, "%s: %d %s", verb, atoi(sc + 13), msg);
		free(x);
		return NULL;
	}
	return x;
}
static int xml_is(const char *x, const char *tag, const char *want) {
	char v[32];
	return gs_xml(x, tag, v, sizeof v) >= 0 && !strcmp(v, want);
}

// ---------------------------------------------------------------- finding the PC
// Limitation: the responder's source address is the PC (first answer wins); IPv4 only. Upgrade: parse SRV/A records.
static int mdns(struct gs *g) {
	static const unsigned char q[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
		9, '_', 'n', 'v', 's', 't', 'r', 'e', 'a', 'm', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0x80, 1};  // PTR, class IN with the QU bit: Windows only answered unicast-requested queries
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(5353)}, from;
	inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
	int found = -1;
	for (int tries = 0; tries < 2 && found; tries++) {
		sendto(fd, q, sizeof q, 0, (struct sockaddr *)&to, sizeof to);
		struct pollfd p = {fd, POLLIN, 0};
		while (found && poll(&p, 1, 1000) == 1) {
			unsigned char b[1500];
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
			if (n > 12 && (b[2] & 0x80) && memmem(b, n, q + 12, 10)) {
				inet_ntop(AF_INET, &from.sin_addr, g->host, sizeof g->host);
				found = 0;
			}
		}
	}
	close(fd);
	return found;
}

int gs_find(struct gs *g, const char *host, int port) {
	if (!port) port = HTTP_PORT;
	if (port != g->http_port) g->http_port = port, g->https_port = port - 5;
	char p[256], *s;
	if (host && *host) { snprintf(g->host, sizeof g->host, "%s", host); return 0; }
	file(g, p, "host");
	if ((s = read_file(p))) {
		s[strcspn(s, "\r\n")] = 0;
		snprintf(g->host, sizeof g->host, "%s", s);
		free(s);
		int fd = *g->host ? tcp(g, g->http_port, 1000) : -1;
		if (fd >= 0) { close(fd); return 0; }
	}
	char saved[80];
	snprintf(saved, sizeof saved, "%s", g->host);
	if (mdns(g) == 0) return 0;
	snprintf(g->host, sizeof g->host, "%s", saved);
	return fail(g, *saved ? "PC at %s unreachable, and no _nvstream._tcp answer" : "no saved PC and no _nvstream._tcp answer", saved);
}

void gs_save_host(struct gs *g) {
	char p[256], line[84];
	int n = snprintf(line, sizeof line, "%s\n", g->host);
	file(g, p, "host");
	char *old = read_file(p);
	if (!old || strcmp(old, line)) write_file(p, line, n);
	free(old);
}

// ---------------------------------------------------------------- serverinfo, pairing, apps
int gs_serverinfo(struct gs *g, struct gs_info *i) {
	memset(i, 0, sizeof *i);
	char *x = NULL, v[32];
	int https = 0;
	if (g->have_srv) {
		x = call(g, 1, 5000, "serverinfo", "");
		if (!x && !g->unauth) return -1;  // network trouble: don't mistake it for "unpaired"
		https = x != NULL;
	}
	if (!x && !(x = call(g, 0, 5000, "serverinfo", ""))) return -1;
	gs_xml(x, "appversion", i->appversion, sizeof i->appversion);
	gs_xml(x, "GfeVersion", i->gfeversion, sizeof i->gfeversion);
	if (gs_xml(x, "ServerCodecModeSupport", v, sizeof v) > 0) i->codecs = atoi(v);
	if (gs_xml(x, "currentgame", v, sizeof v) > 0) i->currentgame = atoi(v);
	if (gs_xml(x, "HttpsPort", v, sizeof v) > 0 && atoi(v) > 0) g->https_port = atoi(v);
	i->paired = https && xml_is(x, "PairStatus", "1");
	free(x);
	return 0;
}

static void ecb(const unsigned char key[16], int enc, const unsigned char *in, int n, unsigned char *out) {
	mbedtls_aes_context a;
	mbedtls_aes_init(&a);
	enc ? mbedtls_aes_setkey_enc(&a, key, 128) : mbedtls_aes_setkey_dec(&a, key, 128);
	for (int i = 0; i < n; i += 16) mbedtls_aes_crypt_ecb(&a, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in + i, out + i);
	mbedtls_aes_free(&a);
}

int gs_pair(struct gs *g, const char *pin, const char *name) {
	unsigned char salt[16], key[32], chal[16], enc[64], dec[64], csec[16], h[32], ps[600], sig[512], buf[2048];
	char hx[8192], sh[33], dn[100], *x = NULL, *chex = NULL, *pem = NULL;
	size_t siglen = 0;
	int r = -1, n = 0;
	mbedtls_x509_crt srv;
	mbedtls_x509_crt_init(&srv);
	if (strlen(pin) != 4 || strspn(pin, "0123456789") != 4) return fail(g, "PIN must be 4 digits");
	for (const char *s = name && *name ? name : "tablet"; *s && n < (int)sizeof dn - 4; s++)  // URL-encode the name
		n += isalnum((unsigned char)*s) || strchr("-_.~", *s) ? (dn[n] = *s, 1) : sprintf(dn + n, "%%%02X", (unsigned char)*s);
	dn[n] = 0;
	rnd(g, salt, 16);
	memcpy(buf, salt, 16);
	memcpy(buf + 16, pin, 4);
	sha256(buf, 20, key);  // AES-128 key = SHA-256(salt || PIN)[0..15]

	// 1. Exchange certificates. Sunshine holds this request until the PIN is typed into its web UI.
	chex = malloc(2 * strlen(g->cert_pem) + 1);
	hex((unsigned char *)g->cert_pem, strlen(g->cert_pem), chex);
	hex(salt, 16, sh);
	x = call(g, 0, 300000, "pair", "devicename=%s&updateState=1&phrase=getservercert&salt=%s&clientcert=%s", dn, sh, chex);
	if (!x) goto out;
	if (!xml_is(x, "paired", "1") || gs_xml(x, "plaincert", hx, sizeof hx) <= 0) { fail(g, "PC refused pairing (PIN not entered in time?)"); goto out; }
	pem = calloc(1, sizeof hx / 2 + 1);
	if ((n = unhex(hx, (unsigned char *)pem, sizeof hx / 2)) <= 0 || mbedtls_x509_crt_parse(&srv, (unsigned char *)pem, n + 1)) {
		fail(g, "PC sent a bad certificate");
		goto out;
	}
	free(x);

	// 2. Our challenge.
	rnd(g, chal, 16);
	ecb(key, 1, chal, 16, enc);
	hex(enc, 16, hx);
	if (!(x = call(g, 0, 10000, "pair", "devicename=%s&updateState=1&clientchallenge=%s", dn, hx))) goto out;
	if (!xml_is(x, "paired", "1") || gs_xml(x, "challengeresponse", hx, sizeof hx) <= 0 || unhex(hx, enc, 64) != 48) {
		fail(g, "pairing failed at the client challenge");
		goto out;
	}
	ecb(key, 0, enc, 48, dec);  // dec = SHA-256(our challenge || server cert sig || server secret) || server challenge
	free(x);

	// 3. Answer the server's challenge: SHA-256(server challenge || our cert sig || our secret).
	rnd(g, csec, 16);
	memcpy(buf, dec + 32, 16);
	memcpy(buf + 16, g->crt.sig.p, g->crt.sig.len);
	memcpy(buf + 16 + g->crt.sig.len, csec, 16);
	sha256(buf, 32 + g->crt.sig.len, h);
	ecb(key, 1, h, 32, enc);
	hex(enc, 32, hx);
	if (!(x = call(g, 0, 10000, "pair", "devicename=%s&updateState=1&serverchallengeresp=%s", dn, hx))) goto out;
	if (!xml_is(x, "paired", "1") || gs_xml(x, "pairingsecret", hx, sizeof hx) <= 0 || (n = unhex(hx, ps, sizeof ps)) <= 16) {
		fail(g, "pairing failed at the server challenge");
		goto out;
	}
	free(x);
	x = NULL;

	// 4. Check the PC: it signed its secret with its cert, and it knew the PIN.
	sha256(ps, 16, h);
	if (mbedtls_pk_verify(&srv.pk, MBEDTLS_MD_SHA256, h, 32, ps + 16, n - 16)) { fail(g, "PC's pairing signature is bad"); goto out; }
	memcpy(buf, chal, 16);
	memcpy(buf + 16, srv.sig.p, srv.sig.len);
	memcpy(buf + 16 + srv.sig.len, ps, 16);
	sha256(buf, 32 + srv.sig.len, h);
	if (memcmp(h, dec, 32)) { fail(g, "wrong PIN"); goto out; }

	// 5. Our signed secret.
	sha256(csec, 16, h);
	if (mbedtls_pk_sign(&g->key, MBEDTLS_MD_SHA256, h, 32, sig, sizeof sig, &siglen, mbedtls_ctr_drbg_random, &g->rng)) {
		fail(g, "signing failed");
		goto out;
	}
	memcpy(buf, csec, 16);
	memcpy(buf + 16, sig, siglen);
	hex(buf, 16 + siglen, hx);
	if (!(x = call(g, 0, 10000, "pair", "devicename=%s&updateState=1&clientpairingsecret=%s", dn, hx))) goto out;
	if (!xml_is(x, "paired", "1")) { fail(g, "PC rejected our pairing secret"); goto out; }
	free(x);

	// 6. Prove it over HTTPS with the pinned cert, then keep the pin.
	mbedtls_x509_crt_free(&g->srv);
	g->srv = srv;
	g->have_srv = 1;
	mbedtls_x509_crt_init(&srv);
	if (!(x = call(g, 1, 10000, "pair", "devicename=%s&updateState=1&phrase=pairchallenge", dn))) goto out;
	if (!xml_is(x, "paired", "1")) { fail(g, "PC rejected the HTTPS pair challenge"); goto out; }
	char sp[256];
	file(g, sp, "server.pem");
	if (write_file(sp, pem, strlen(pem))) { fail(g, "can't save the PC's certificate in %s", g->dir); goto out; }
	r = 0;
out:
	free(x);
	free(chex);
	free(pem);
	mbedtls_x509_crt_free(&srv);
	mbedtls_platform_zeroize(key, sizeof key);
	mbedtls_platform_zeroize(csec, sizeof csec);
	return r;
}

int gs_app_id(struct gs *g, const char *title) {
	char *x = call(g, 1, 5000, "applist", ""), t[128], v[32];
	int id = -1;
	if (!x) return -1;
	for (char *p = x, *e; id < 0 && (p = strstr(p, "<App>")) && (e = strstr(p, "</App>")); p = e + 1) {
		*e = 0;
		if (gs_xml(p, "AppTitle", t, sizeof t) >= 0 && !strcmp(t, title) && gs_xml(p, "ID", v, sizeof v) > 0) id = atoi(v);
	}
	free(x);
	if (id < 0) fail(g, "no app titled \"%s\"", title);
	return id;
}

int gs_launch(struct gs *g, int appid, int resume, int w, int h, int fps, int surround_info, int sops,
              const unsigned char rikey[16], int rikeyid, char *url, int urlsz) {
	char rk[33];
	hex(rikey, 16, rk);
	// sops=0 (default): Sunshine never applies our resolution to a PC display; sops=1 only when its output is our own
	// virtual monitor. localAudioPlayMode=1: PC audio stays on the PC.
	char *x = call(g, 1, 30000, resume ? "resume" : "launch",
	               "appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d&localAudioPlayMode=1"
	               "&surroundAudioInfo=%d&remoteControllersBitmap=0&gcmap=0&gcpersist=0&hdrMode=0%s",
	               appid, w, h, fps, !!sops, rk, rikeyid, surround_info, LiGetLaunchUrlQueryParameters());
	mbedtls_platform_zeroize(rk, sizeof rk);
	if (!x) return -1;
	int ok = !xml_is(x, resume ? "resume" : "gamesession", "0") && strstr(x, resume ? "<resume>" : "<gamesession>");
	url[0] = 0;
	gs_xml(x, "sessionUrl0", url, urlsz);
	free(x);
	return ok ? 0 : fail(g, "PC refused to %s the app", resume ? "resume" : "launch");
}

int gs_cancel(struct gs *g) {
	char *x = call(g, 1, 10000, "cancel", "");
	if (!x) return -1;
	int ok = xml_is(x, "cancel", "1");
	free(x);
	return ok ? 0 : fail(g, "PC refused to quit the app");
}
