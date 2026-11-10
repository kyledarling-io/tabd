#!/system/bin/sh
# wifi.sh: Wi-Fi with Android stopped: the wlan link in links (tabd.conf), primary or fallback. Android's own
# wpa_supplicant runs standalone (nl80211), with busybox udhcpc. Run as root (boot.sh at boot, run.sh wifi); safe to
# run again.   wifi.sh [stop]
# As a fallback: udhcpc.sh gives each link a route metric by its place in links (100, 600, ...), and net.sh's watcher
# moves the stream between them (see net.sh).
# Settings: wifi_ssid and wifi_password in /data/local/tmp/tabd/secrets.conf (pushed by run.sh from the PC at mode
# 0600, never in git); wifi_country (required: the regulatory country, e.g. US), optional wifi_freq (MHz: only that
# channel) and wifi_bssid (only that AP) in secrets.conf or tabd.conf.
# The password is never logged or printed. It only goes into state/wpa.conf, which is mode 0600 and owned by the wifi
# user (wpa_supplicant drops to it at start, so it can't read root's files). That file is deleted as soon as
# wpa_supplicant has read it, and wpa_supplicant runs quiet (-q; it shows keys as [REMOVED] without -K anyway).
# The AP is pinned (no roaming, bgscan off): wifi_bssid, else the BSSID in state/wifi.bssid; without either, the
# strongest BSS of wifi_ssid (on wifi_freq if set), saved there.
D=/data/local/tmp/tabd
S=$D/state
L=$D/log/tabd.log
log() { echo "[wifi $(cut -d' ' -f1 /proc/uptime)] $*" >> $L; }
C=$D/secrets.conf
kv() { sed -n "s/^[ \t]*$1[ \t]*=[ \t]*//p" $2 2> /dev/null | tail -1 | sed 's/[ \t\r]*$//'; }
key() { v=$(kv $1 $C); [ -n "$v" ] || [ "$2" = secret ] || v=$(kv $1 $D/tabd.conf | sed 's/[ \t]*#.*//'); echo "$v"; }
LINKS=$(kv links $D/tabd.conf | sed 's/[ \t]*#.*//'); LINKS=${LINKS:-eth0 wlan0}
W=; for l in $LINKS; do case $l in wlan*) W=$l; break ;; esac; done  # the Wi-Fi link
[ -n "$W" ] || { log "no wlan link in links ($LINKS): Wi-Fi off"; exit 0; }
CLI="/vendor/bin/wpa_cli -p $S/wpa -i $W"
read PC PCMAC 2> /dev/null < $S/pc
for p in $(pidof wpa_supplicant); do grep -q $S/wpa /proc/$p/cmdline 2> /dev/null && kill $p; done  # ours only
[ -f $S/udhcpc-$W.pid ] && kill $(cat $S/udhcpc-$W.pid) 2> /dev/null
rm -f $S/wpa.conf $S/udhcpc-$W.pid
[ -n "$PC" ] && ip route del $PC/32 dev $W metric 50 2> /dev/null  # a watcher's pin
if [ "$1" = stop ]; then ip -4 addr flush dev $W 2> /dev/null; ip link set $W down 2> /dev/null; log "stopped"; exit 0; fi
SSID=$(key wifi_ssid secret); PASS=$(key wifi_password secret); FREQ=$(key wifi_freq); CC=$(key wifi_country); BSSID=$(key wifi_bssid)
if [ -z "$SSID" ] || [ -z "$PASS" ]; then log "no wifi_ssid and wifi_password in $C: Wi-Fi off"; ip link set $W down 2> /dev/null; exit 0; fi
[ -n "$CC" ] || { log "no wifi_country (the regulatory country, e.g. US) in $C or tabd.conf: Wi-Fi off"; exit 0; }
i=0; while [ ! -e /sys/class/net/$W ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
[ -e /sys/class/net/$W ] || { log "no $W after 30 s"; exit 1; }
iw reg set $CC
ip link set $W up
[ -n "$BSSID" ] || BSSID=$(cat $S/wifi.bssid 2> /dev/null)
if [ -z "$BSSID" ]; then  # the strongest BSS of the SSID (on wifi_freq, if set), kept in state/
	for n in 1 2 3; do
		BSSID=$(iw dev $W scan 2> /dev/null | awk -v ssid="$SSID" -v f="$FREQ" '/^BSS /{b = substr($2, 1, 17)} /freq:/{q = $2 + 0}
			/signal:/{s = $2 + 0} /^[ \t]*SSID: /{n = $0; sub(/^[ \t]*SSID: /, "", n); if (n == ssid && (f == "" || q == f + 0) && (best == "" || s > bs)) {best = b; bs = s; bq = q}}
			END {if (best != "") print best, bs, bq}')
		[ -n "$BSSID" ] && break
		sleep 2
	done
	[ -n "$BSSID" ] || { log "no \"$SSID\"${FREQ:+ on $FREQ MHz} in 3 scans"; exit 1; }
	set -- $BSSID; BSSID=$1
	log "pinned the strongest \"$SSID\": $1 at $2 dBm, $3 MHz"
	echo $BSSID > $S/wifi.bssid
fi
WC=$S/wpa.conf
# a control socket for wpa_cli: both drop to the wifi user, so its folder is wifi-owned (wpa_cli's own socket goes in
# /data/vendor/wifi/wpa/sockets)
mkdir -p $S/wpa; chown 1010:1010 $S/wpa; chmod 770 $S/wpa
umask 077
{  # print -r: mksh builtin, so the password never shows up in a process list
	print -r -- "country=$CC"
	print -r -- "ctrl_interface=$S/wpa"
	print -r -- "update_config=0"
	print -r -- "pmf=1"
	print -r -- "network={"
	print -r -- "	ssid=\"$SSID\""
	print -r -- "	bssid=$BSSID"
	print -r -- "	key_mgmt=SAE WPA-PSK"
	print -r -- "	psk=\"$PASS\""
	print -r -- "	ieee80211w=1"
	[ -n "$FREQ" ] && print -r -- "	scan_freq=$FREQ" && print -r -- "	freq_list=$FREQ"
	print -r -- "	bgscan=\"\""
	print -r -- "}"
} > $WC
chown 1010:1010 $WC  # AID_WIFI
# WIFI_DEBUG=1: debug level into logcat for diagnosis (keys still show as [REMOVED]: never add -K)
Q=-q; [ "$WIFI_DEBUG" ] && Q=
/vendor/bin/hw/wpa_supplicant -B $Q -i $W -D nl80211 -c $WC > /dev/null 2>&1; r=$?  # -B returns after reading $WC
rm -f $WC
[ $r = 0 ] || { log "wpa_supplicant didn't start (exit $r)"; exit 1; }
# Android's supplicant never scans on its own at start (its framework does that), and `reconnect` doesn't either:
# one scan starts the usual select-and-associate
sleep 0.5; $CLI scan > /dev/null 2>&1 || log "wpa_cli scan failed"
i=0; until iw dev $W link | grep -q '^Connected' || [ $i -ge 30 ]; do sleep 1; i=$((i + 1)); done
iw dev $W set power_save off 2> /dev/null
if iw dev $W link | grep -q '^Connected'; then
	log "associated with $(iw dev $W link | awk '/^Connected/{b = $3} /freq:/{f = $2} /signal:/{s = $2 " " $3}
		/tx bitrate:/{sub(/^[ \t]*tx bitrate: /, ""); t = $0} END {print b ", " f " MHz, " s ", tx " t}') after $i s, $($CLI status | sed -n 's/^key_mgmt=//p'), power save $(iw dev $W get power_save | awk '{print $3}')"
else log "not associated after 30 s (wpa_supplicant keeps trying; net.sh's watcher kicks it)"; fi
busybox udhcpc -i $W -s $D/udhcpc.sh -b -p $S/udhcpc-$W.pid -t 30 -T 1 -A 10 > /dev/null 2>&1
