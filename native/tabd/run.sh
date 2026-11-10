#!/bin/sh
# Run and maintain tabd on the tablet from the PC (Git Bash). First install, or after a factory reset: install.sh
# (devices/rk3399-tve1030g/README.md has the whole procedure). On the tablet everything is in /data/local/tmp/tabd:
# tabd, its scripts, tabd.conf (settings), secrets.conf (Wi-Fi), ml/ (pairing), state/, log/, backup/.
#   run.sh start        push everything (as run.sh push) and restart tabd. In appliance mode (tabd installed as the boot
#                       display) the boot's own loop restarts it; else stop Android's display stack and run boot.sh's
#                       loop by hand (boot.sh --loop).
#   run.sh push         push tabd (TABD_BIN=a prebuilt one, else native/build/tabd), its scripts, the Wi-Fi secrets and
#                       this PC's adb allow-list (state/pc: net.sh peer, or TABD_PC/TABD_PCMAC); tabd.conf only if the
#                       tablet has none (tabd/tabd.conf.example), or TABD_CONF=<file> always (keeping the tablet's
#                       pc, wifi_country, wifi_freq and wifi_bssid unless the file sets them). Restarts nothing.
#   run.sh migrate      once, on a tablet set up before /data/local/tmp/tabd was a folder (tabd-decide.sh, tabd-ml/, ...):
#                       move it into the folder (boot.sh --migrate: tabd.args becomes tabd.conf, the pairing is moved and
#                       kept, the old files go to backup/old-layout), reboot with a trial boot and confirm it
#   run.sh wifi [stop]  push the Wi-Fi credentials (~/.config/tabd/tabd.local.conf, outside the clone; TABD_LOCAL
#                       overrides; secrets.conf, mode 0600, on the tablet) and (re)start Wi-Fi, the fallback link (wifi.sh)
#   run.sh check        read-only: is the tablet the installed appliance? (overlayfs and tabd.rc, files, mode, picture,
#                       network, adb allow-list, state, backups). Exit status: the number of failures. Reads no
#                       input-device names (CLAUDE.md, rule 4).
#   run.sh stop         kill tabd and REBOOT the tablet (appliance mode boots tabd again; never `start`: see below)
#   run.sh find         find the tablet (adb devices, else mDNS), connect, adb root, print its address
#   run.sh log [N]      last N lines of tabd's log (log/tabd.log, rotated at 1 MB to tabd.log.1)
# Settings: tabd reads tabd.conf when it starts. Edit the tablet's copy (or push one: TABD_CONF=<file> run.sh push),
# then restart tabd: adb shell pkill -x tabd (the loop starts it again within a second).
# Restore is a reboot: Android's input stack reads each input device's "phys" string when it opens it, and the goodix-ts
# driver's phys pointer dangles (EVIOCGPHYS once panicked the kernel), so `start` after a tabd session can panic.
# TABLET=<ip>:5555 picks the device; TABD_SERIAL (default: any) is the adb serial mDNS looks for.
set -e
export MSYS_NO_PATHCONV=1
abs() { [ -f "$1" ] || { echo "$2=$1: no such file" >&2; exit 1; }; echo "$(cd "$(dirname "$1")" && (pwd -W 2>/dev/null || pwd))/$(basename "$1")"; }
# before the cd below; adb.exe needs Windows paths (C:/...), not Git Bash ones (/c/...); a file that was named must exist
[ -z "$TABD_BIN" ] || TABD_BIN=$(abs "$TABD_BIN" TABD_BIN)
[ -z "$TABD_CONF" ] || TABD_CONF=$(abs "$TABD_CONF" TABD_CONF)
[ -z "$TABD_LOCAL" ] || TABD_LOCAL=$(abs "$TABD_LOCAL" TABD_LOCAL)
cd "$(dirname "$0")/.."
T=/data/local/tmp
D=$T/tabd
S=$D/state
L=$D/log/tabd.log
find_tablet() {
	ID=${TABLET:-$(adb devices | awk -F'\t' '$1 ~ /:5555$/ && $2 == "device" {print $1; exit}')}
	if [ -z "$ID" ]; then  # not connected (new DHCP address after a reboot?): the adb mDNS service of the tablet's serial
		ID=$(adb mdns services | awk -v s="${TABD_SERIAL:-adb-}" '/_adb._tcp/ && index($0, s) {print $3; exit}')
		[ -n "$ID" ] && adb connect "$ID" >/dev/null
	fi
	[ -n "$ID" ] || { echo "no tablet: not in 'adb devices' and not on mDNS (adb mdns services); is it booted?"; exit 1; }
	A="adb -s $ID"
}
root() { $A root >/dev/null 2>&1 || true; adb connect "$ID" >/dev/null 2>&1 || true; $A wait-for-device; }
put() { $A push "$1" "$2.new" >/dev/null && $A shell "chmod ${3:-755} $2.new && mv $2.new $2"; }  # mv: what runs keeps its file
old_layout() { $A shell "[ -f $D ]" 2>/dev/null; }  # before the folder, /data/local/tmp/tabd was the binary
push_local() {  # the secrets (Wi-Fi), never in git: through a 0700 folder, so no moment at a looser mode on the tablet
	LC=${TABD_LOCAL:-${XDG_CONFIG_HOME:-$HOME/.config}/tabd/tabd.local.conf}
	[ -f "$LC" ] || return 0  # only the default can be missing: Wi-Fi isn't set up from this PC
	LC=$(cygpath -m "$LC" 2>/dev/null || echo "$LC")
	$A shell "umask 077; mkdir -p $D/secrets.in; chmod 700 $D/secrets.in"
	$A push "$LC" $D/secrets.in/f >/dev/null
	$A shell "chmod 600 $D/secrets.in/f && mv $D/secrets.in/f $D/secrets.conf && rmdir $D/secrets.in"
}
push_files() {
	BIN=${TABD_BIN:-build/tabd}
	[ -s "$BIN" ] || { echo "no $BIN: build it (sh native/build.sh) or set TABD_BIN to a prebuilt tabd"; exit 1; }
	old_layout && { echo "this tablet has the layout from before /data/local/tmp/tabd was a folder: run.sh migrate first"; exit 1; }
	$A shell "mkdir -p $S $D/log && chmod 755 $D $S $D/log"
	put "$BIN" $D/tabd
	for f in boot.sh net.sh udhcpc.sh wifi.sh; do put tabd/$f $D/$f; done
	# tabd.rc (on /vendor) starts this old path: a stub that runs boot.sh
	$A shell "printf '#!/system/bin/sh\nexec sh $D/boot.sh \"\$@\"\n' > $T/tabd-decide.sh && chmod 755 $T/tabd-decide.sh"
	if [ -n "$TABD_CONF" ]; then  # the site's settings on the tablet (pc, Wi-Fi) stay, unless the new file sets them
		NC=$(mktemp); cp "$TABD_CONF" "$NC"; [ -z "$(tail -c1 "$NC")" ] || echo >> "$NC"
		$A shell "grep -E '^ *(pc|wifi_country|wifi_freq|wifi_bssid) *=' $D/tabd.conf 2>/dev/null" | tr -d '\r' | while read -r k rest; do
			grep -q -E "^ *$k *=" "$NC" || { echo "$k $rest" >> "$NC"; echo "kept the tablet's $k"; }
		done
		put "$(cygpath -m "$NC" 2>/dev/null || echo "$NC")" $D/tabd.conf 644; rm -f "$NC"
	elif ! $A shell "[ -f $D/tabd.conf ]"; then put tabd/tabd.conf.example $D/tabd.conf 644; fi
	push_local
	PC=$TABD_PC; PCMAC=$TABD_PCMAC
	[ -n "$PC" ] || read PC PCMAC <<-EOF
	$($A shell "sh $D/net.sh peer" | tr -d '\r')
	EOF
	# no MAC this time: keep the one the allow-list has, never narrow it to the IP alone
	[ -z "$PC" ] || [ -n "$PCMAC" ] || PCMAC=$($A shell "awk '{print \$2}' $S/pc 2>/dev/null" | tr -d '\r')
	if [ -n "$PC" ]; then $A shell "umask 022; echo $PC $PCMAC > $S/pc"
	else echo "warning: couldn't tell this PC's address (adb over USB, or another adb client connected? set TABD_PC and TABD_PCMAC); state/pc unchanged"; fi
	[ -z "$PC" ] || [ -n "$PCMAC" ] || echo "warning: no MAC for $PC (set TABD_PCMAC): the allow-list accepts its IP only"
}
find_tablet
if [ "$1" != log ]; then root; fi  # tabd runs as root: signalling, replacing, reading the firewall or rebooting needs it
case "$1" in
start)
	push_files
	# appliance mode: boot.sh's loop owns tabd and restarts it; a second loop here would fight it
	$A shell "sh $D/net.sh;
		if [ \"\$(getprop vendor.tabd.mode)\" = appliance ]; then pkill -x tabd; exit 0; fi;
		[ -f $S/loop.pid ] && kill \$(cat $S/loop.pid) 2>/dev/null; pkill -x tabd; for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -x tabd >/dev/null || break; sleep 0.2; done;
		stop; setsid sh $D/boot.sh --loop > /dev/null 2>&1 < /dev/null &"
	echo "tabd (re)started on $ID (log: $0 log)"
	;;
push)
	push_files
	echo "pushed to $ID; tabd.conf: $($A shell "grep -E '^[ \t]*[a-z]' $D/tabd.conf" | tr -d '\r' | tr '\n' ';')"
	;;
migrate)
	old_layout || { echo "nothing to migrate: $D is already a folder"; exit 0; }
	BIN=${TABD_BIN:-build/tabd}
	[ -s "$BIN" ] || { echo "no $BIN: build it (sh native/build.sh) or set TABD_BIN to a prebuilt tabd"; exit 1; }
	$A shell "rm -rf $D.new && mkdir $D.new"
	$A push "$BIN" $D.new/tabd >/dev/null
	for f in boot.sh net.sh udhcpc.sh wifi.sh; do $A push tabd/$f $D.new/$f >/dev/null; done
	$A shell "chmod 755 $D.new/tabd $D.new/*.sh"
	[ -z "$TABD_CONF" ] || { $A push "$TABD_CONF" $D.new/tabd.conf >/dev/null; $A shell "chmod 644 $D.new/tabd.conf"; }
	out=$($A shell "sh $D.new/boot.sh --migrate" | tr -d '\r') || true
	echo "$out" | grep -v '^tabd-migrate: rebooting$'
	echo "$out" | grep -q '^tabd-migrate: rebooting$' || { echo "boot.sh --migrate didn't move the tablet (its output above; if it rebooted, the old layout boots): run.sh stops here"; exit 1; }
	adb disconnect "$ID" >/dev/null 2>&1 || true
	echo "waiting for the tablet to come back"; sleep 20
	for i in $(seq 110); do adb connect "$ID" 2>/dev/null | grep -q "connected to" && $A shell true 2>/dev/null && break; sleep 2; done
	root
	$A shell "umask 022; touch $S/ok" && echo "trial boot confirmed: adb reaches the tablet in the new layout"
	for i in $(seq 30); do  # in the first boot after the move only (the moved log holds the old boots)
		l=$($A shell "m=\$(grep -n 'moved the old layout into' $L | tail -1 | cut -d: -f1); n=\$(grep -n '\] appliance mode\$' $L | tail -1 | cut -d: -f1)
			[ \${n:-0} -gt \${m:-0} ] && tail -n +\$n $L | grep -E 'ml: live|not paired' | tail -1" | tr -d '\r')
		[ -n "$l" ] && break; sleep 1
	done
	case "$l" in *live*) echo "picture: Sunshine is streaming to the tablet" ;; *"not paired"*) echo "NOT PAIRED: the glass shows a PIN" ;; *) echo "no picture after 30 s ($0 log)" ;; esac
	;;
wifi)  # push the Wi-Fi credentials (push_local) and wifi.sh, (re)start Wi-Fi; wifi stop
	push_local
	put tabd/wifi.sh $D/wifi.sh; put tabd/udhcpc.sh $D/udhcpc.sh
	$A shell "sh $D/wifi.sh $2"
	$A shell "grep -E '^\[(wifi|udhcpc wlan) ' $L | tail -4"
	;;
check)  # read-only: no pushes, no restarts, no input devices
	set +e
	F=0
	ok() { echo "  ok    $*"; }
	no() { echo "  FAIL  $*"; F=$((F + 1)); }
	note() { echo "  note  $*"; }
	is() { $A shell "$1" >/dev/null 2>&1; }  # a test on the tablet
	get() { $A shell "$1" 2>/dev/null | tr -d '\r'; }
	same() { [ "$(md5sum < "$1" | cut -c1-32)" = "$(get "md5sum < $2" | cut -c1-32)" ]; }  # local file, tablet file
	cf() { get "sed -n 's/^[ \t]*$1[ \t]*=[ \t]*\([^#]*\).*/\1/p' $D/tabd.conf | tail -1" | sed 's/[ \t]*$//'; }  # a tabd.conf value
	addr() { get "ip -4 addr show $1" | awk '/inet /{split($2, a, "/"); print a[1]; exit}'; }
	hex() { echo $1 | awk -F. '{printf "%02X%02X%02X%02X", $4, $3, $2, $1}'; }  # /proc/net/udp's form of an address
	echo "tablet $ID: $(get 'getprop ro.product.model'), $(get 'getprop ro.build.type'), verified boot $(get 'getprop ro.boot.verifiedbootstate')"
	echo "install"
	is "grep -q ' /vendor overlay ' /proc/mounts" && ok "/vendor has its overlayfs" || no "/vendor has no overlayfs (install.sh)"
	if same tabd/tabd.rc /vendor/etc/init/tabd.rc; then ok "/vendor/etc/init/tabd.rc is the repo's"
	elif is "grep -q '^exec sh $D/boot.sh' $T/tabd-decide.sh"; then note "/vendor/etc/init/tabd.rc is an older one, working through the stub $T/tabd-decide.sh: install.sh replaces it (a trial boot)"
	else no "/vendor/etc/init/tabd.rc missing, or an older one without its stub (install.sh)"; fi
	old_layout && no "the layout from before $D was a folder: run.sh migrate"
	bad=; for f in boot.sh net.sh udhcpc.sh wifi.sh; do same tabd/$f $D/$f && is "[ -x $D/$f ]" || bad="$bad $f"; done
	[ -z "$bad" ] && ok "scripts current" || no "not the repo's, or not executable:$bad (run.sh push)"
	BIN=${TABD_BIN:-build/tabd}
	if ! is "[ -x $D/tabd ]"; then no "no $D/tabd (run.sh push)"
	elif [ -s "$BIN" ] && ! same "$BIN" $D/tabd; then note "tabd differs from $BIN (deployed from elsewhere?)"
	else ok "tabd$([ -s "$BIN" ] && echo " matches $BIN")"; fi
	is "[ -s $D/tabd.conf ]" && ok "tabd.conf: $(get "grep -E '^[ \t]*[a-z]' $D/tabd.conf" | tr '\n' ';')" || no "no tabd.conf (tabd runs on defaults)"
	is "[ -d $D/ml ]" && ok "pairing identity in ml/" || no "no $D/ml: tabd pairs again (a PIN on the glass)"
	is "[ ! -e $S/off ] && [ ! -e $T/tabd.off ]" && ok "maintenance switch off" || no "the maintenance switch is on (state/off or $T/tabd.off): Android boots"
	echo "running"
	[ "$(get 'getprop vendor.tabd.mode')" = appliance ] && is "! pidof system_server" && ok "appliance mode, Android's stack down" || no "not in appliance mode, or Android's stack runs"
	fps=$(get "grep status: $L | tail -1" | sed -n 's/.*status: fps \([0-9]*\).*/\1/p')
	is "pidof tabd" && [ "${fps:-0}" -gt 0 ] && ok "tabd running, $fps fps in its last status line" || no "tabd not running, or no picture (fps ${fps:-?}): Sunshine down, or not paired? ($0 log)"
	echo "network"
	LINKS=$(cf links); LINKS=${LINKS:-eth0 wlan0}; P=${LINKS%% *}; FB=$(echo $LINKS | awk '{print $2}')
	WL=; for l in $LINKS; do case $l in wlan*) WL=$l; break ;; esac; done
	a=$(addr $P); car=$(get "cat /sys/class/net/$P/carrier")
	[ -n "$a" ] && [ "$car" = 1 ] && ok "$P up, $a" || no "$P: no link or no address (its cable, or its AP)"
	on=; for l in $LINKS; do x=$(addr $l); [ -n "$x" ] && [ "$(get "grep -c ' $(hex $x):' /proc/net/udp")" -gt 0 ] 2> /dev/null && on=$l && break; done
	if [ "$on" = "$P" ]; then ok "the stream runs over $P"  # tabd's UDP sockets are bound to that link's address
	elif [ -n "$on" ] && [ "$car" = 1 ]; then no "the stream runs over $on although $P is up (it moves back once $P forwards)"
	elif [ -n "$on" ]; then note "the stream runs over $on: $P is down"
	else no "no stream sockets on $LINKS"; fi
	FM=$(cf fixed_mac); [ "$FM" = 1 ] && FM=$P
	if [ -n "$FM" ]; then
		want=02:$(printf '%s\n' "$(get 'getprop ro.serialno')" | sha256sum | sed 's/^\(..\)\(..\)\(..\)\(..\)\(..\).*/\1:\2:\3:\4:\5/')
		[ "$(get "cat /sys/class/net/$FM/address")" = "$want" ] && ok "fixed MAC $want on $FM" || no "$FM hasn't the fixed MAC $want (fixed_mac = $FM)"
	fi
	is "ip rule | grep -q '^100:.*lookup main'" && ok "routing rule for the main table" || no "no 'ip rule pref 100 lookup main' (netd's rules leave the tablet unreachable)"
	if [ -n "$FB" ]; then  # failover's preconditions: a link without carrier loses its routes, the first link wins
		lo=$(get 'ip -4 route' | awk -v l=" $LINKS " '/ src / {d = ""; m = 0; for (i = 1; i < NF; i++) { if ($i == "dev") d = $(i + 1); if ($i == "metric") m = $(i + 1) }
			if (index(l, " " d " ") && (o == "" || m + 0 < b + 0)) { b = m; o = d } } END {if (o != "") print o, b}')
		[ "$(get 'cat /proc/sys/net/ipv4/conf/all/ignore_routes_with_linkdown')" = 1 ] && [ "${lo% *}" = "$P" ] &&
			ok "failover ready: ignore_routes_with_linkdown = 1, $P has the lowest route metric (${lo#* })" ||
			no "failover: ignore_routes_with_linkdown isn't 1, or $P hasn't the lowest route metric (lowest: ${lo:-none})"
	fi
	[ -z "$FB$WL" ] || { is "kill -0 \$(cat $S/watch.pid)" && ok "link watcher running (failover, failback, Wi-Fi reconnects)" || no "link watcher not running (reboot)"; }
	if [ -n "$WL" ]; then
		is "cat $D/secrets.conf $D/tabd.conf 2>/dev/null | grep -q '^[ \t]*wifi_country[ \t]*=[ \t]*[A-Za-z]'" && ok "wifi_country set" ||
			no "$WL is in links but there's no wifi_country (secrets.conf or tabd.conf): Wi-Fi stays off, and failover with it"
		is "[ \"\$(stat -c %a $D/secrets.conf)\" = 600 ]" && ok "secrets.conf, mode 600 (contents never shown)" || no "no secrets.conf at mode 600 (run.sh wifi)"
		ps=$(get "iw dev $WL get power_save" | awk '{print $3}')
		get "iw dev $WL link" | grep -q '^Connected' && [ "$ps" = off ] && ok "$WL associated, $(addr $WL), power save off" ||
			no "$WL not associated, or power save ${ps:-?} (wifi.sh turns it off)"
	fi
	echo "adb allow-list"
	read pc pcmac <<-EOF
	$(get "cat $S/pc")
	EOF
	AA=$(cf adb_allow); sec=$(get 'getprop ro.adb.secure'); [ "${AA:-auto}" = auto ] && { [ "$sec" = 1 ] && AA=0 || AA=1; }
	if [ "$AA" != 1 ]; then note "off (adb_allow = $(cf adb_allow), ro.adb.secure = ${sec:-unset}): adb asks for the PC's key"
	else
		rules=$(get 'iptables -S tabd_adb')
		is "iptables -C INPUT -p tcp --dport 5555 -j tabd_adb" && echo "$rules" | grep -q -- "-A tabd_adb -j DROP" && ok "TCP 5555 only from the allow-list (and localhost)" || no "adb is open to the LAN (tabd_adb not hooked in, or no DROP)"
		echo "$rules" | grep -q -- "-s ${pc:-none}/32 -j ACCEPT" && echo "$rules" | grep -q -- "--mac-source ${pcmac:-none} -j ACCEPT" && ok "accepts the PC: $pc, MAC $pcmac" ||
			no "the PC's IP or MAC isn't accepted (state/pc: ${pc:-none} ${pcmac:-none}; run.sh push)"
		is "ip6tables -C INPUT -p tcp --dport 5555 ! -i lo -j DROP" && ok "adb over IPv6 closed" || no "adb over IPv6 is open"
		peer=$(get "sh $D/net.sh peer" | awk '{print $1}')
		[ -z "$peer" ] || [ "$peer" = "$pc" ] || note "this PC is $peer now, the allow-list has $pc: its MAC still gets in; run.sh push updates it"
	fi
	echo "state"
	pct=$(get "cat $S/brightness"); bl=$(cf backlight); [ -n "$bl" ] || bl=$(get 'ls /sys/class/backlight | head -1')
	max=$(get "cat /sys/class/backlight/$bl/max_brightness"); cur=$(get "cat /sys/class/backlight/$bl/brightness")
	if [ -z "$pct" ]; then note "no brightness saved yet (the panel keeps $cur of $max)"
	elif [ "$cur" = $(( (max * pct + 50) / 100 )) ]; then ok "brightness $pct% ($cur of $max, $bl), orientation $(get "cat $S/orient")"
	else no "brightness $cur of $max, state/brightness says $pct%"; fi
	echo "backups"
	is "cd $D/backup && sha256sum -c SHA256SUMS" && ok "on the tablet: $D/backup (sha256 OK)" || no "no intact backup in $D/backup"
	BK=${TABD_BACKUP:-${XDG_CONFIG_HOME:-$HOME/.config}/tabd/backup}  # install.sh's, outside the clone
	n=0
	for d in "$BK"/partitions-*/; do
		[ -f "$d/SHA256SUMS" ] && (cd "$d" && sha256sum -c --quiet SHA256SUMS >/dev/null 2>&1) && n=$((n + 1)) && ok "on the PC: ${d%/} (sha256 OK)"
	done
	[ $n -gt 0 ] || no "no intact partition backup under $BK (install.sh makes one before writing)"
	[ $F = 0 ] && echo "all good" || echo "$F failed"
	exit $F
	;;
stop)
	$A shell "[ -f $S/loop.pid ] && kill \$(cat $S/loop.pid) 2>/dev/null; rm -f $S/loop.pid; pkill -x tabd; sleep 0.5; reboot" || true
	adb disconnect "$ID" >/dev/null 2>&1 || true
	echo "tabd stopped, tablet $ID rebooting (appliance mode starts tabd again; Android only with state/off; $0 find)"
	;;
find) echo "$ID" ;;
log) $A shell tail -n "${2:-40}" $L ;;
*) sed -n '2,26p' tabd/run.sh; exit 1 ;;
esac
