#!/system/bin/sh
# Glass-state check: make the PC go silent by dropping its UDP on the tablet for SECS seconds (default 36; the rule is
# removed by this same script, so it can't stick), with one simulated wake tap at WAKE seconds (default 33, while the
# panel is asleep). Run on the tablet as root, detached:  setsid sh silence.sh PC_IP [SECS] [WAKE] &
# Expect in tabd.log: glass silent at 1 s, no signal at 5 s, asleep (backlight off) at 30 s, the wake tap consumed,
# then live again after the unblock (time-to-picture = "first frame on glass" minus the unblock time).
PC=${1:?PC address}; SECS=${2:-36}; WAKE=${3:-33}
T=$(for e in /sys/class/input/event*; do case $(readlink $e/device/device/driver) in */Goodix-TS) echo /dev/input/${e##*/};; esac; done)  # never read input names/phys
L=/data/local/tmp/tabd/log/tabd.log
B=/sys/class/backlight/$(ls /sys/class/backlight | head -1)  # the first, as tabd takes it without backlight = NAME
echo "silence: block UDP from $PC at uptime $(cut -d' ' -f1 /proc/uptime)" >> $L
iptables -I INPUT -s $PC -p udp -j DROP
sleep $WAKE
echo "silence: wake tap at uptime $(cut -d' ' -f1 /proc/uptime), bl_power $(cat $B/bl_power)" >> $L
sendevent $T 1 330 1; sendevent $T 3 57 0; sendevent $T 3 53 640; sendevent $T 3 54 400; sendevent $T 0 2 0; sendevent $T 0 0 0
sleep 0.2; sendevent $T 1 330 0; sendevent $T 0 2 0; sendevent $T 0 0 0
sleep $((SECS - WAKE))
iptables -D INPUT -s $PC -p udp -j DROP
echo "silence: unblock at uptime $(cut -d' ' -f1 /proc/uptime), bl_power $(cat $B/bl_power)" >> $L
