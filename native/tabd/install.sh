#!/bin/sh
# install.sh: make the tablet the appliance (tabd as its boot display), from stock or factory-reset Android, or any state
# in between. Safe to re-run: every step looks first and skips what's done, so on an installed tablet it changes
# nothing. devices/rk3399-tve1030g/README.md has the whole procedure, including the steps that need hands.
# From Git Bash on the PC:
#   sh native/tabd/install.sh [--dry-run]
#     --dry-run       only print what each step would do (changes nothing; the final check is read-only)
#     TABD_BIN=FILE   a prebuilt tabd (e.g. from a GitHub release); default native/build/tabd, built if missing
#     TABD_CONF=FILE  the tablet's tabd.conf if it has none yet (default tabd.conf.example: pc = auto, orient = 0)
#     TABLET=ID       the adb device (ip:5555 or a USB serial) if it isn't the one `adb devices` lists first
#     TABD_SERIAL     the tablet's serial, for finding it again after reboots (default: read from the tablet once adb reaches it)
# Steps:
#  1. adb on TCP 5555 at every boot (persist.adb.tcp.port): the appliance has no other way in
#  2. back up boot, vbmeta, dtbo, recovery and misc to the PC (read-only dd, sha256-checked on both sides), unless
#     /vendor is already remounted (then nothing below writes outside /data)
#  3. adb disable-verity + reboot + adb remount: /vendor becomes writable through overlayfs (a "scratch" partition in
#     super's free space). On this tablet the stock vbmeta already has verification disabled, so no partition image
#     changes (checked: all five backed-up partitions identical after the first install).
#  4. tabd and its files into /data/local/tmp/tabd (run.sh push), if any differ; a tablet set up before that was a
#     folder moves into it first (run.sh migrate: keeps the pairing, reboots with a trial boot)
#  5. tabd.rc into /vendor/etc/init, then a trial boot into appliance mode, confirmed once adb reaches the tablet
#     (unconfirmed for 5 minutes, the tablet boots Android once by itself, so adb can reach it to fix things). Already
#     installed and running: only restart tabd if step 4 changed files.
#  7. run.sh check
set -e
export MSYS_NO_PATHCONV=1
D=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))  # adb.exe needs Windows paths (C:/...), not /c/...
[ -n "$TABD_BIN" ] && TABD_BIN=$(cd "$(dirname "$TABD_BIN")" && (pwd -W 2>/dev/null || pwd))/$(basename "$TABD_BIN")
DRY=; [ "$1" = --dry-run ] && DRY=1
T=/data/local/tmp
TD=$T/tabd  # everything on the tablet
TS=$TD/state
SERIAL=${TABD_SERIAL:-}
LAST=  # the tablet's adb address before a reboot
do_() { if [ "$DRY" ]; then echo "   would: $*"; else "$@"; fi; }  # every change goes through here
get() { adb -s "$S" shell "$1" 2>/dev/null | tr -d '\r'; }
is() { adb -s "$S" shell "$1" >/dev/null 2>&1; }
md5() { md5sum < "$1" | cut -c1-32; }
rmd5() { get "md5sum < $1" | cut -c1-32; }
root() { adb -s "$S" root >/dev/null 2>&1 || true; sleep 2; adb connect "$S" >/dev/null 2>&1 || true; adb -s "$S" wait-for-device; }
up() {  # find the tablet after a reboot: USB, mDNS, else the address it had (its DHCP lease usually survives)
	S=; for i in $(seq 120); do
		adb devices | awk -v s="$SERIAL" '$1 == s && $2 == "device" {f = 1} END {exit !f}' && S=$SERIAL && break
		S=$(adb mdns services 2>/dev/null | awk -v s="$SERIAL" '/_adb._tcp/ && index($0, s) {print $3; exit}')
		[ -z "$S" ] && S=$LAST
		[ -n "$S" ] && adb connect "$S" 2>/dev/null | grep -q "connected to" && break
		S=; sleep 2
	done
	[ -n "$S" ] || { echo "the tablet didn't come back within 4 minutes (try: adb mdns services)"; exit 1; }
	root; echo "   tablet at $S"
}
reboot_wait() {
	[ "$DRY" ] && { echo "   would: reboot and wait for the tablet"; return; }
	LAST=$S; adb -s "$S" reboot; adb disconnect "$S" >/dev/null 2>&1 || true; echo "   rebooting..."; sleep 20; up
}
remount() {  # /vendor writable through overlayfs; the first remount after disable-verity may ask for a reboot
	[ "$DRY" ] && { echo "   would: adb remount"; return; }
	out=$(adb -s "$S" remount 2>&1) || true; echo "$out" | sed 's/^/   /'
	if echo "$out" | grep -qi reboot; then reboot_wait; out=$(adb -s "$S" remount 2>&1) || true; echo "$out" | sed 's/^/   /'; fi
	echo "$out" | grep -qi "remount succeeded" || { echo "adb remount failed: stopping"; exit 1; }
}
backup() {  # read-only dd on the tablet, pulled to the PC, sha256-checked on both sides; stops the install if it fails
	BK=${TABD_BACKUP:-${XDG_CONFIG_HOME:-$HOME/.config}/tabd/backup}  # outside the clone, like tabd.local.conf
	BK=$(cygpath -m "$BK" 2>/dev/null || echo "$BK")/partitions-$(date +%F)  # adb.exe needs C:/..., not /c/...
	[ -e "$BK" ] && BK=$BK-$(date +%H%M%S)
	[ "$DRY" ] && { echo "   would: dd boot vbmeta dtbo recovery misc into $TD/backup, pull to $BK, check sha256"; return; }
	adb -s "$S" shell "umask 022; mkdir -p $TD/backup && cd $TD/backup && for p in boot vbmeta dtbo recovery misc; do
		dd if=/dev/block/by-name/\$p of=\$p.img bs=1M 2>/dev/null || exit 1; done && sha256sum *.img > SHA256SUMS && sha256sum -c SHA256SUMS >/dev/null"
	mkdir -p "$(dirname "$BK")"; adb -s "$S" pull $TD/backup "$BK" >/dev/null
	(cd "$BK" && sha256sum -c --quiet SHA256SUMS) || { echo "the backup in $BK doesn't check out: stopping before any write"; exit 1; }
	echo "   $BK (sha256 OK on the tablet and here)"
}

S=${TABLET:-$(adb devices | awk -F'\t' '$2 == "device" {print $1; exit}')}
if [ -n "$S" ]; then root; else [ -n "$SERIAL" ] || { echo "no tablet in adb devices: connect it (devices/rk3399-tve1030g/README.md, Install, step 1), or set TABD_SERIAL so install.sh can look for it"; exit 1; }; up; fi
[ -n "$SERIAL" ] || SERIAL=$(get 'getprop ro.serialno')
[ "$(get 'getprop ro.build.type')" = userdebug ] || { echo "not a userdebug build: adb root and remount need one (devices/rk3399-tve1030g/README.md)"; exit 1; }
[ "$(get 'id -u')" = 0 ] || { echo "adb root didn't work"; exit 1; }
[ "$(get 'getprop ro.boot.verifiedbootstate')" = orange ] || { echo "the bootloader is locked (verified boot isn't orange): remount needs it unlocked (devices/rk3399-tve1030g/README.md)"; exit 1; }
echo "tablet $S ($(get 'getprop ro.product.model'), serial $(get 'getprop ro.serialno'))${DRY:+, dry run: nothing changes}"

echo "1/6 adb on TCP 5555 at every boot"
if [ "$(get 'getprop persist.adb.tcp.port')" = 5555 ]; then echo "   already"
else do_ adb -s "$S" shell setprop persist.adb.tcp.port 5555; echo "   set (live from the next adbd start; adb tcpip 5555 makes it live now)"; fi

if is "grep -q ' /vendor overlay ' /proc/mounts"; then
	echo "2/6 backup: skipped, /vendor is already remounted (nothing below writes outside /data)"
	echo "3/6 disable-verity, remount: already done"
else
	echo "2/6 back up the boot partitions to the PC"
	backup
	echo "3/6 disable-verity, reboot, remount"
	do_ adb -s "$S" disable-verity
	reboot_wait
	remount
fi

echo "4/6 tabd and its files in $TD"
BIN=${TABD_BIN:-$D/../build/tabd}
if [ ! -s "$BIN" ]; then
	[ -n "$TABD_BIN" ] && { echo "no $TABD_BIN"; exit 1; }
	echo "   no $BIN: building it (needs the NDK, see CLAUDE.md step 3)"; do_ sh "$D/../build.sh"
fi
if is "[ -f $TD ]"; then  # /data/local/tmp/tabd is the old binary: move into the folder first (it reboots)
	echo "   the layout from before $TD was a folder: moving it in (run.sh migrate)"
	do_ env TABD_BIN="$BIN" TABD_CONF="$TABD_CONF" TABLET="$S" sh "$D/run.sh" migrate
fi
changed=
for f in "$BIN tabd" "$D/boot.sh boot.sh" "$D/net.sh net.sh" "$D/udhcpc.sh udhcpc.sh" "$D/wifi.sh wifi.sh"; do
	set -- $f
	{ [ -s "$1" ] && [ "$(md5 "$1")" = "$(rmd5 $TD/$2)" ] && is "[ -x $TD/$2 ]"; } || changed="$changed $2"
done
need=tabd.conf; case $S in *:*) need="$need state/pc" ;; esac; [ -z "$TABD_PC" ] || need="$need state/pc"  # USB adb has no peer
for f in $need; do is "[ -s $TD/$f ]" || changed="$changed $f"; done
is "grep -q '^exec sh $TD/boot.sh' $T/tabd-decide.sh" || changed="$changed tabd-decide.sh"
if [ -n "$changed" ]; then echo "   new or different:$changed"; do_ env TABD_BIN="$BIN" TABD_CONF="$TABD_CONF" TABLET="$S" sh "$D/run.sh" push
else echo "   all current"; fi

echo "5/6 tabd.rc and appliance mode"
rc_ok=; [ "$(md5 "$D/tabd.rc")" = "$(rmd5 /vendor/etc/init/tabd.rc)" ] && rc_ok=1
if [ "$rc_ok" ] && [ "$(get 'getprop vendor.tabd.mode')" = appliance ]; then
	echo "   tabd.rc current, appliance mode running"
	if [ -n "$changed" ]; then
		echo "   restarting tabd for the new files (boot.sh changes apply at the next boot)"
		do_ adb -s "$S" shell "sh $TD/net.sh; pkill -x tabd"
	fi
else
	if [ -z "$rc_ok" ]; then
		remount
		do_ adb -s "$S" push "$D/tabd.rc" /vendor/etc/init/tabd.rc
		do_ adb -s "$S" shell "chmod 644 /vendor/etc/init/tabd.rc; restorecon /vendor/etc/init/tabd.rc 2>/dev/null; true"
	fi
	echo "   trial boot into appliance mode"
	do_ adb -s "$S" shell "umask 022; rm -f $T/tabd.off $TS/off $TS/ok $TS/android-once $TS/rebooted; touch $TS/trial"
	reboot_wait
	do_ adb -s "$S" shell "umask 022; touch $TS/ok"
	[ "$DRY" ] || echo "   trial confirmed: adb reaches the tablet in appliance mode"
	if [ -z "$DRY" ]; then  # first contact with Sunshine: pairing shows a PIN on the glass
		for i in $(seq 30); do
			l=$(get "n=\$(grep -n '\] appliance mode\$' $TD/log/tabd.log | tail -1 | cut -d: -f1); tail -n +\$n $TD/log/tabd.log | grep -E 'ml: live|not paired' | tail -1")
			[ -n "$l" ] && break; sleep 1
		done
		case "$l" in
		*"not paired"*) echo "   PAIRING: the glass shows a 4-digit PIN; enter it in Sunshine's web UI (PIN page) on the PC" ;;
		*live*) echo "   picture: Sunshine is streaming to the tablet" ;;
		*) echo "   no picture after 30 s: is Sunshine running on the PC? (run.sh log)" ;;
		esac
	fi
fi

echo "6/6 check"
env TABLET="$S" TABD_BIN="$BIN" sh "$D/run.sh" check || true
