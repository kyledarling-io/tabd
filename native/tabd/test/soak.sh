#!/bin/sh
# soak.sh: measure tabd over one link for a while, from the PC (Git Bash):   sh native/tabd/test/soak.sh LINK SECONDS
#   LINK = eth0 or wlan0: the interface the ping is pinned to and, for wlan0, the one sampled with `iw` every 10 s.
#   tabd's own traffic follows the routes. To force the stream over Wi-Fi, add a host route to the PC via wlan0 first
#   (e.g. ip route add <PC>/32 dev wlan0 src <wlan0 IP> metric 50) and restart tabd (pkill -x tabd).
# Prints one summary line set:
#   tabd: fps, rx->glass p50/p95, and shown/dropped/late/keyreqs deltas (from its 10 s status lines)
#   ping: RTT to the PC p50/p95/max and loss
#   wifi: signal and tx bitrate (MCS)
# Read-only on the tablet, except two scratch files in /data/local/tmp/tabd/log (soak*), removed at the end.
set -e
export MSYS_NO_PATHCONV=1
LINK=${1:?eth0 or wlan0}; D=${2:-600}
S=${TABLET:-$(adb devices | awk -F'\t' '$1 ~ /:5555$/ && $2 == "device" {print $1; exit}')}
A="adb -s $S"
T=/data/local/tmp/tabd/log
$A root >/dev/null 2>&1 || true; adb connect "$S" >/dev/null 2>&1 || true; $A wait-for-device  # tabd's folder is root's
PC=$($A shell "cut -d' ' -f1 /data/local/tmp/tabd/state/pc" | tr -d '\r')
$A shell "rm -f $T/soak.txt $T/soak-ping.txt; setsid sh -c '
	echo start \$(cut -d\" \" -f1 /proc/uptime) > $T/soak.txt
	ping -i 0.2 -c $((D * 5)) -I $LINK $PC > $T/soak-ping.txt 2>&1 &
	k=0; while [ \$k -lt $((D / 10)) ]; do
		[ $LINK = wlan0 ] && iw dev wlan0 link | grep -E \"signal:|tx bitrate:\" >> $T/soak.txt
		sleep 10; k=\$((k + 1))
	done
	wait; echo end \$(cut -d\" \" -f1 /proc/uptime) >> $T/soak.txt' > /dev/null 2>&1 < /dev/null &"
sleep $((D + 3))
while ! $A shell "grep -q ^end $T/soak.txt" 2>/dev/null; do sleep 2; done
t0=$($A shell "sed -n 's/^start //p' $T/soak.txt" | tr -d '\r'); t1=$($A shell "sed -n 's/^end //p' $T/soak.txt" | tr -d '\r')
echo "== $LINK, $D s (tablet uptime $t0-$t1)"
$A shell "cat $T/tabd.log.1 $T/tabd.log 2>/dev/null | grep 'status:'" | tr -d '\r' | awk -v a="$t0" -v b="$t1" '
	# The log spans boots, and its stamps are uptime: count only the last boot (stamps restart). A counter that goes
	# down means tabd restarted, so it adds its new value.
	function up(v, p) { return p == "" ? 0 : v >= p ? v - p : v }
	{ t = substr($1, 2) / 1000; if (t < last) { n = 0; fs = 0; ds = dd = dl = dk = 0; ps = pd = pl = pk = "" } last = t
	  if (t < a || t > b) next
	  for (i = 1; i <= NF; i++) { if ($i == "fps") f = $(i + 1); if ($i == "shown") sh = $(i + 1); if ($i == "dropped") dr = $(i + 1)
	    if ($i == "late") la = $(i + 1); if ($i == "keyreqs") kr = $(i + 1); if ($i == "rx-to-glass") rg = $(i + 1) }
	  n++; fs += f; if (n == 1 || f < fmin) fmin = f; split(rg, r, "/"); p50[n] = r[1]; p95[n] = r[2]
	  ds += up(sh, ps); dd += up(dr, pd); dl += up(la, pl); dk += up(kr, pk); ps = sh; pd = dr; pl = la; pk = kr }
	function med(x, m,   i, j, t) { for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (x[j] + 0 < x[i] + 0) { t = x[i]; x[i] = x[j]; x[j] = t } return x[int((m + 1) / 2)] }
	function mx(x, m,   i, v) { v = 0; for (i = 1; i <= m; i++) if (x[i] + 0 > v) v = x[i] + 0; return v }
	END { if (!n) { print "tabd: no status lines in the window"; exit }
	  printf "tabd: fps %.1f avg (min %d) over %d windows; rx->glass p50 %s ms, p95 %s ms median (worst %s); shown +%d dropped +%d late +%d keyreqs +%d\n",
	    fs / n, fmin, n, med(p50, n), med(p95, n), mx(p95, n), ds, dd, dl, dk }'
$A shell "cat $T/soak-ping.txt" | tr -d '\r' | awk '
	/time=/ { sub(/.*time=/, ""); v[++n] = $1 + 0 } /packet loss/ { loss = $0 }
	END { for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
	  if (n) printf "ping: RTT p50 %.2f ms, p95 %.2f ms, max %.2f ms (%d replies); %s\n", v[int(n * 0.5) + 1], v[int(n * 0.95) + 1], v[n], n, loss
	  else print "ping: no replies; " loss }'
[ $LINK = wlan0 ] && $A shell "cat $T/soak.txt" | tr -d '\r' | awk '
	/signal:/ { s[++n] = $2 + 0 } /tx bitrate:/ { sub(/^[ \t]*tx bitrate: /, ""); b[$0]++ }
	END { lo = 0; hi = -200; for (i = 1; i <= n; i++) { t += s[i]; if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i] }
	  if (n) printf "wifi: signal %.0f dBm avg (%d..%d) over %d samples; tx bitrate:", t / n, lo, hi, n
	  for (k in b) printf " [%s] x%d", k, b[k]; print "" }'
$A shell "rm -f $T/soak.txt $T/soak-ping.txt"
