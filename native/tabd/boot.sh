#!/system/bin/sh
# boot.sh: at boot (started by tabd.rc on post-fs-data), run tabd as the tablet's display ("appliance mode"). The
# tablet is a dedicated Ethernet touchscreen: Android is not a destination, only a maintenance mode set over adb.
# Everything lives in /data/local/tmp/tabd (D below), so updates never touch /vendor. tabd.rc starts the old path
# /data/local/tmp/tabd-decide.sh, where run.sh keeps a two-line stub that runs this script.
# Android boots instead only when:
#   - state/off exists (or /data/local/tmp/tabd.off, the old path): the maintenance switch, set by hand over adb
#     (touch it, then reboot; rm it and reboot to come back). Nothing creates it automatically.
#   - state/android-once exists: one Android boot, then appliance mode again (left by an unconfirmed trial boot below).
#   - there is no tabd binary, or tabd.rc's triggers don't stop Android's display stack (see below).
# In appliance mode tabd.rc stops zygote (before system_server, so Android's input stack never opens a device),
# surfaceflinger, the composer and netd as soon as init starts them. Android is only ever reached by a reboot, never
# `start`: Android's input stack reads goodix-ts's dangling phys pointer, which can panic the kernel after tabd ran.
# tabd crash loop (5 exits within 60 s; a setting tabd.conf can't have counts): reboot once, back into appliance mode;
# looping again within 2 minutes of that boot, keep retrying tabd every 30 s and stay in appliance mode.
#   --dry-run   only log the decision (safe to run from adb at any time)
#   --loop      only the restart loop, for run.sh start in hand mode; a crash loop there retries every 30 s, no reboot
#   --migrate   once, from run.sh migrate: move a tablet set up before the single folder into D (see migrate below)
D=/data/local/tmp/tabd
S=$D/state
L=$D/log/tabd.log
log() { echo "[boot $(cut -d' ' -f1 /proc/uptime)] $*" >> $L; }
android() { log "$1: Android boots"; [ "$DRY" ] || setprop vendor.tabd.mode android; exit 0; }
cf() { sed -n "s/^[ \t]*$1[ \t]*=[ \t]*\([^#]*\).*/\1/p" $D/tabd.conf 2> /dev/null | tail -1 | sed 's/[ \t\r]*$//'; }  # a tabd.conf value
# Android's composer HAL holds the display, and SurfaceFlinger (tabd.rc stops it as init starts it) drives it.
composers() {  # the running composer HALs: composer in tabd.conf, else every init service *hwcomposer* or *graphics.composer*
	C=$(cf composer); [ "$C" = auto ] && C=
	if [ -n "$C" ]; then [ "$(getprop init.svc.$C)" = running ] && echo $C; return 0; fi
	for c in $(getprop | sed -n 's/^\[init\.svc\.\([^]]*composer[^]]*\)\]: \[running\]$/\1/p'); do
		case $c in *hwcomposer* | *graphics.composer*) echo $c ;; esac
	done
}
display_free() {  # stop them and wait until neither they nor SurfaceFlinger run; 1 if one still does after 5 s
	i=0
	while :; do
		cs=$(composers)
		for c in $cs; do setprop ctl.stop $c; log "stopping $c"; done
		[ -z "$cs" ] && [ "$(getprop init.svc.surfaceflinger)" != running ] && return 0
		[ $i -ge 50 ] && return 1
		sleep 0.1; i=$((i + 1))
	done
}
# A composer that init starts later would take the display back: keep stopping any that reaches running, for 60 s.
composer_guard() { ( i=0; while [ $i -lt 120 ]; do sleep 0.5; i=$((i + 1)); for c in $(composers); do setprop ctl.stop $c; log "stopping $c (started late)"; done; done ) & }
# The layout before the single folder: files named tabd-* and tabd.* directly in /data/local/tmp, with /data/local/tmp/tabd
# the binary. run.sh migrate stages the new files in D.new and runs this. Nothing is deleted: the old files go to
# backup/old-layout; the pairing identity (tabd-ml/) is moved, never made again, so the tablet stays paired.
migrate() {
	umask 022  # adb's root shell has 0: nothing new may be world-writable
	T=/data/local/tmp; N=$D.new; B=$N/backup/old-layout
	[ -f $D ] && [ -x $N/tabd ] || { echo "nothing to migrate: $D isn't the old binary, or $N/tabd isn't staged"; exit 1; }
	mkdir -p $B $N/state $N/log; chmod 755 $N $N/state $N/log
	if [ ! -f $N/tabd.conf ]; then  # tabd's arguments become tabd.conf (the old binary's defaults: orient auto, input off)
		set -- $(cat $T/tabd.args 2> /dev/null); pc=auto; o=auto; in=0
		while [ $# -gt 0 ]; do
			case $1 in --ml) pc=$2; shift ;; --orient) o=$2; shift ;; --input) in=1 ;; *) echo "tabd.args: $1 dropped (not a tabd.conf setting)" ;; esac
			shift
		done
		printf 'pc = %s\norient = %s\ninput = %s\n' "$pc" "$o" "$in" > $N/tabd.conf
	fi
	b=$(sed -n 's/^[ \t]*brightness[ \t]*=[ \t]*\([0-9]*\).*/\1/p' $T/tabd.conf 2> /dev/null | tail -1)
	[ -n "$b" ] && echo $b > $N/state/brightness
	cp $T/tabd-orient $N/state/orient 2> /dev/null
	cp $T/tabd-wifi.bssid $N/state/wifi.bssid 2> /dev/null
	[ -f $T/tabd-pc.conf ] && echo $(sed -n 's/^PC=//p; s/^PCMAC=//p' $T/tabd-pc.conf) > $N/state/pc
	# the switch: stop the old boot's loop, its link watcher and tabd, so nothing uses the old paths while they move
	pkill -f tabd-decide.sh; [ -f $T/tabd-wifi-watch.pid ] && kill $(cat $T/tabd-wifi-watch.pid); pkill -x tabd
	i=0; while pgrep -x tabd > /dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
	# the stub first: from here a power cut boots Android (the stub finds no boot.sh until the last mv), never the old
	# decide.sh with its binary but no tabd-ml/, which would make a new pairing identity
	mv $T/tabd-decide.sh $B/ && printf '#!/system/bin/sh\nexec sh %s/boot.sh "$@"\n' $D > $T/tabd-decide.sh && chmod 755 $T/tabd-decide.sh ||
		{ echo "couldn't replace tabd-decide.sh: rebooting into the old layout"; [ -f $T/tabd-decide.sh ] || mv $B/tabd-decide.sh $T/; reboot; exit 1; }
	mv $T/tabd-ml $N/ml || { echo "couldn't move tabd-ml: rebooting into the old layout"; mv $B/tabd-decide.sh $T/; reboot; exit 1; }
	[ -f $T/tabd.local.conf ] && mv $T/tabd.local.conf $N/secrets.conf
	[ -d $T/tabd-backup ] && mv $T/tabd-backup/* $N/backup/ && rmdir $T/tabd-backup
	mv $T/tabd.log* $N/log/ 2> /dev/null
	rm -rf $T/tabd-wpa; rm -f $T/tabd*.pid; rmdir $T/tabd-secret 2> /dev/null  # runtime files; an empty folder
	for f in $T/tabd $T/tabd-* $T/tabd.*; do case $f in $N | $T/tabd.off | $T/tabd-decide.sh) ;; *) [ -e $f ] && mv $f $B/ ;; esac; done
	mv $N $D
	touch $S/trial
	log "moved the old layout into $D (its files in backup/old-layout); rebooting, a trial boot"
	echo "moved into $D; rebooting (trial boot: confirm with touch $S/ok)"
	echo "tabd-migrate: rebooting"  # run.sh waits for the tablet only after this line
	sync; reboot; exit 0
}
DRY=; LOOP=
case "$1" in --dry-run) DRY=1 ;; --loop) LOOP=1 ;; --migrate) migrate ;; esac
umask 022; mkdir -p $S $D/log
if [ "$LOOP" ]; then echo $$ > $S/loop.pid; display_free || log "the composer still runs: tabd waits for the display"  # run.sh stop kills it
else
	{ [ -f $S/off ] || [ -f /data/local/tmp/tabd.off ]; } && android "the maintenance switch is set (state/off or /data/local/tmp/tabd.off)"
	if [ -f $S/android-once ]; then [ "$DRY" ] || rm -f $S/android-once; android "state/android-once (one boot only)"; fi
	[ -x $D/tabd ] || android "no $D/tabd"
	log "appliance mode${DRY:+ (dry run: nothing changed)}"
	[ "$DRY" ] && exit 0
	setprop vendor.tabd.mode appliance
	rm -f $S/loop.pid  # run.sh's hand-mode loop from an earlier boot: `run.sh stop` would kill whatever has that pid now
	# Trial boot, armed by install.sh and run.sh migrate (state/trial): unless state/ok appears within 5 minutes (they
	# touch it once adb reaches the tablet), boot Android once, so a first appliance boot that loses the network can't
	# strand a tablet with no buttons. The next boot is appliance mode again.
	if [ -f $S/trial ]; then
		rm -f $S/trial $S/ok
		log "trial boot: touch $S/ok within 5 minutes, or the tablet boots Android once"
		( sleep 300; [ -f $S/ok ] || { log "trial boot not confirmed: rebooting into Android once"; touch $S/android-once; reboot; } ) &
	fi
	# A crash loop reboots once; the marker expires 2 minutes into the boot, so a later crash loop gets its reboot too.
	( sleep 120; rm -f $S/rebooted ) &
	display_free || android "Android's composer or SurfaceFlinger still runs 5 s after appliance mode was set"
	composer_guard
	# Network, in the background so the glass doesn't wait for it. links (tabd.conf, default "eth0 wlan0"): the first
	# carries the stream, the next takes over while it's down (net.sh's watcher); wifi.sh brings up a wlan link.
	#  - The first link can appear late (the reference's eth0 is a vendor module, ~12 s into boot): wait up to 60 s.
	#  - netd starts at zygote-start (~5 s) and tabd.rc stops it, but its start-up replaces the routing rules with its
	#    own, which never look up the main table (where the links' subnet routes live): add a rule for main once netd is
	#    gone. Its brief start also sets accept_ra_rt_table to -1000 (a table per interface): 0 keeps each link's IPv6
	#    routes, fe80::/64 too, in main.
	#  - fixed_mac = IFACE, a quirk (the reference's r8168 gets a random MAC every boot, hence a new DHCP lease each
	#    time): a locally administered MAC from the serial on that link, wherever it is in links (1: the first link), and
	#    an EUI-64 link-local address from it, so both stay the same across boots (and a DHCP reservation becomes possible).
	(
		LINKS=$(cf links); LINKS=${LINKS:-eth0 wlan0}; P=${LINKS%% *}
		FM=$(cf fixed_mac); [ "$FM" = 1 ] && FM=$P
		i=0  # the first link exists, and netd is gone
		while { [ ! -e /sys/class/net/$P ] || [ "$(getprop init.svc.netd)" = running ]; } && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
		for l in $LINKS; do [ -e /proc/sys/net/ipv6/conf/$l ] && echo 0 > /proc/sys/net/ipv6/conf/$l/accept_ra_rt_table; done
		MAC=
		[ -z "$FM" ] || [ -e /sys/class/net/$FM ] || log "fixed_mac = $FM: no such interface yet, its MAC left as it is"
		if [ -n "$FM" ] && [ -e /sys/class/net/$FM ]; then
			MAC=02:$(getprop ro.serialno | sha256sum | sed 's/^\(..\)\(..\)\(..\)\(..\)\(..\).*/\1:\2:\3:\4:\5/')
			ip link set $FM down
			echo 0 > /proc/sys/net/ipv6/conf/$FM/addr_gen_mode
			ip link set $FM address $MAC
			ip link set $FM up
		fi
		ip rule add pref 100 lookup main
		ip -6 rule add pref 100 lookup main
		log "links $LINKS${MAC:+; $FM $MAC (fixed_mac)} after waiting $((i / 10)).$((i % 10)) s (netd: $(getprop init.svc.netd))"
		sh $D/wifi.sh &  # a wlan link in links, if secrets.conf has the Wi-Fi credentials (it waits for the interface)
		sh $D/net.sh
	) &
fi
# tabd, restarted when it exits; it reads tabd.conf at each start, and rotates the log (these lines too) at 1 MB
n=0; t0=$(date +%s); delay=1
while :; do
	s=$(date +%s)
	$D/tabd --log $L
	r=$?
	now=$(date +%s)
	[ $((now - s)) -gt 60 ] && delay=1  # ran a while: back to quick restarts
	[ $((now - t0)) -gt 60 ] && n=0 && t0=$now
	n=$((n + 1))
	log "tabd exited $r after $((now - s)) s ($n in this minute); restarting in $delay s"
	if [ $n -ge 5 ] && [ $delay = 1 ]; then
		if [ "$LOOP" ]; then log "crash loop (hand mode): retrying every 30 s"; delay=30
		elif [ -f $S/rebooted ]; then
			log "crash loop again within 2 minutes of the last one's reboot: retrying every 30 s, staying in appliance mode"
			delay=30
		else
			log "crash loop: rebooting once, back into appliance mode"
			touch $S/rebooted; reboot; exit 0
		fi
	fi
	sleep $delay
done
