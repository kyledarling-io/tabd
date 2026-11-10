#!/system/bin/sh
# Injects tabd's brightness gesture as goodix-ts touches (multi-touch protocol A, as touch.sh): one finger held 8 frames (2-3 s:
# sendevent is slow) in the bottom-right corner of the picture, then two fingers land and swipe vertically while it stays.
#   sh gesture.sh [P]   P = points to dim (default 40; negative brightens): the swipe is P % of the picture height
# Run on the tablet as root while tabd runs. Prints the backlight before and after, tabd's new log lines and state/brightness.
# The corner and the swipe follow tabd's locked orientation (state/orient), so they land the same way up in any pose.
# Never reads input-device names or phys (the goodix phys pointer dangles): the device is found by its driver link.
T=/data/local/tmp/tabd
D=$(for e in /sys/class/input/event*; do case $(readlink $e/device/device/driver) in */Goodix-TS) echo /dev/input/${e##*/};; esac; done)
O=$(cat $T/state/orient 2>/dev/null || echo 0)
P=${1:-40}
s() { sendevent $D "$@"; }
c() {  # one contact: id, then x y in the upright picture, 0..1000 (as map() in tabd.c, inverted; rx/ry, not the caller's y)
	case $O in
	90) rx=$(($3 * 1280 / 1000)) ry=$(((1000 - $2) * 800 / 1000));;
	180) rx=$(((1000 - $2) * 1280 / 1000)) ry=$(((1000 - $3) * 800 / 1000));;
	270) rx=$(((1000 - $3) * 1280 / 1000)) ry=$(($2 * 800 / 1000));;
	*) rx=$(($2 * 1280 / 1000)) ry=$(($3 * 800 / 1000));;
	esac
	s 3 57 $1; s 3 53 $rx; s 3 54 $ry; s 3 48 30; s 3 50 30; s 0 2 0
}
syn() { s 0 0 0; }
B=/sys/class/backlight/$(ls /sys/class/backlight | head -1)  # the first, as tabd takes it without backlight = NAME
bl() { echo "backlight $(cat $B/brightness) of $(cat $B/max_brightness) ($1)"; }
L0=$(wc -l < $T/log/tabd.log)
bl before
y0=$((P > 0 ? 200 : 800))
echo "corner finger held, then two fingers swipe $((P > 0 ? P : -P)) % of the picture $([ $P -gt 0 ] && echo down || echo up)"
s 1 330 1
for i in 1 2 3 4 5 6 7 8; do c 0 985 975; syn; sleep 0.1; done
for i in 0 1 2 3 4 5 6 7 8 9 10; do y=$((y0 + P * i)); c 0 985 975; c 1 400 $y; c 2 600 $y; syn; done
bl "fingers still down"
s 3 48 0; s 3 50 0; s 0 2 0; s 1 330 0; syn  # all up
sleep 0.3
bl after
tail -n +$((L0 + 1)) $T/log/tabd.log | grep -E "touch|brightness|state"
echo "state/brightness: $(cat $T/state/brightness)"
