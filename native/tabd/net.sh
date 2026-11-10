#!/system/bin/sh
# net.sh: the tablet's network while tabd runs and Android's network stack is stopped (run.sh start) or never started
# (appliance boot). Run as root; safe to run again. links (tabd.conf, default "eth0 wlan0"): the first carries the
# stream, the next takes over while it's down.
#  - DHCP: busybox udhcpc on each wired link (wifi.sh does a wlan link), keeping the current address.
#  - adb (TCP 5555), when adb is unauthenticated (adb_allow = auto, the default: when ro.adb.secure isn't 1; 1 or 0
#    forces it): only from the PC (its IP or its MAC, so a new PC address doesn't lock adb out) and localhost; adb over
#    IPv6 closed. Runtime rules, re-applied every boot.
#  - The watcher (net.sh watch, started here), with two links or a wlan one: failover and failback, Wi-Fi kept up.
# The PC comes from state/pc ("IP MAC"), which run.sh push writes from `net.sh peer`.
#   net.sh peer   print the PC as the tablet sees it: the one host with an adb connection (TCP 5555, IPv4, not
#                 loopback) and its MAC from the neighbour table. Nothing if there is none (USB adb: run.sh then takes
#                 TABD_PC/TABD_PCMAC) or more than one, so another adb client never becomes "the PC". Changes nothing.
D=/data/local/tmp/tabd
S=$D/state
L=$D/log/tabd.log
log() { echo "[net $(cut -d' ' -f1 /proc/uptime)] $*" >> $L; }
cf() { sed -n "s/^[ \t]*$1[ \t]*=[ \t]*\([^#]*\).*/\1/p" $D/tabd.conf 2> /dev/null | tail -1 | sed 's/[ \t\r]*$//'; }  # a tabd.conf value
if [ "$1" = peer ]; then
	IP=$(awk '$2 ~ /:15B3$/ && $4 == "01" { a = substr($3, 1, index($3, ":") - 1)
		if (length(a) == 32 && substr(a, 1, 24) != "0000000000000000FFFF0000") next  # IPv6, not IPv4-mapped
		a = substr(a, length(a) - 7); h = "0123456789ABCDEF"; ip = ""  # 4 bytes, little-endian hex
		for (i = 7; i >= 1; i -= 2) ip = ip (i < 7 ? "." : "") ((index(h, substr(a, i, 1)) - 1) * 16 + index(h, substr(a, i + 1, 1)) - 1)
		if (ip !~ /^127\./) seen[ip] = 1 }
		END { for (ip in seen) { n++; one = ip } if (n == 1) print one }' /proc/net/tcp6 /proc/net/tcp 2> /dev/null)
	[ -n "$IP" ] || exit 0
	mac() { ip neigh show $IP | awk '{for (i = 1; i < NF; i++) if ($i == "lladdr") { print $(i + 1); exit } }'; }
	M=$(mac); [ -n "$M" ] || { ping -c 1 -W 1 $IP > /dev/null 2>&1; M=$(mac); }  # an empty neighbour table: ask once
	echo $IP $M
	exit 0
fi
read PC PCMAC 2> /dev/null < $S/pc
LINKS=$(cf links); LINKS=${LINKS:-eth0 wlan0}
P=${LINKS%% *}; F=$(echo $LINKS | awk '{print $2}')  # the primary link and its fallback
W=; for l in $LINKS; do case $l in wlan*) W=$l; break ;; esac; done  # the Wi-Fi link, if any
dhcp() {  # $1: a wired link
	[ -f $S/udhcpc-$1.pid ] && kill $(cat $S/udhcpc-$1.pid) 2> /dev/null
	a=$(ip -4 addr show $1 | awk '/inet /{split($2, a, "/"); print a[1]; exit}')
	busybox udhcpc -i $1 -s $D/udhcpc.sh -b -p $S/udhcpc-$1.pid ${a:+-r $a} -t 30 -T 1 -A 10 > /dev/null 2>&1
}
if [ "$1" = watch ]; then  # 1 s: P's carrier (failover to F and failback); every 20 s: keep Wi-Fi connected
	up() { cut -d' ' -f1 /proc/uptime; }
	car() { cat /sys/class/net/$P/carrier 2> /dev/null || echo 0; }
	CLI="/vendor/bin/wpa_cli -p $S/wpa -i $W"
	i=0; until [ -z "$F" ] || ip -4 addr show $F 2> /dev/null | grep -q inet || [ $i -ge 60 ]; do sleep 1; i=$((i + 1)); done  # the pin needs a source
	was=0  # a carrier already up counts as "back": P may not forward yet (boot)
	pin=; k=0; redo=
	while :; do
		sleep 1; k=$((k + 1)); now=$(car)
		if [ -n "$F" ] && [ "$now" != "$was" ]; then
			was=$now; redo=
			if [ "$now" = 0 ]; then
				if [ -n "$pin" ]; then pin=; ip route del $PC/32 dev $F metric 50 2> /dev/null
				else log "$P carrier lost: the stream moves to $F"; kill -s USR1 $(pidof tabd) 2> /dev/null; fi
			elif [ -n "$PC" ]; then
				ip route replace $PC/32 dev $F metric 50 && pin=$(up) && log "$P carrier up: $PC stays on $F until $P forwards"
			fi
		fi
		# failback once P forwards (its own route, pin or not) and has its subnet route: an interface reset (ip link set
		# down) deletes that, and with noprefixroute (udhcpc.sh) only a new DHCP bind puts it back
		if [ -n "$pin" ] && ping -c 1 -W 1 -I $P $PC > /dev/null 2>&1; then
			if ip -4 route show dev $P | grep -q .; then
				ip route del $PC/32 dev $F metric 50
				log "$P forwards $(awk -v a=$pin -v b=$(up) 'BEGIN {printf "%.1f", b - a}') s after its carrier came up: the stream moves back to $P"
				pin=; kill -s USR1 $(pidof tabd) 2> /dev/null
			elif [ -z "$redo" ]; then
				redo=1; log "$P forwards but has no subnet route (an interface reset?): DHCP again"
				dhcp $P
			fi
		fi
		[ -n "$W" ] && [ $((k % 20)) = 0 ] && case "$($CLI status 2> /dev/null | sed -n 's/^wpa_state=//p')" in  # Android's supplicant leaves rescans to its framework
		DISCONNECTED | INACTIVE) log "$W not connected: reconnect + scan"; $CLI reconnect > /dev/null 2>&1; $CLI scan > /dev/null 2>&1 ;;
		esac
	done
fi
n=
for l in $LINKS; do
	case $l in wlan*) continue ;; esac
	[ -e /sys/class/net/$l ] || { log "no $l"; continue; }
	dhcp $l; n="$n $l"
done
if [ -n "$F" ]; then
	# failover: skip routes through a link without carrier (the unplugged cable); with two links on one subnet, answer
	# ARP only for an interface's own address, from its own address. "default" covers a link that appears later.
	for f in all default $LINKS; do
		[ -e /proc/sys/net/ipv4/conf/$f ] && echo 1 > /proc/sys/net/ipv4/conf/$f/ignore_routes_with_linkdown
		[ -e /proc/sys/net/ipv6/conf/$f ] && echo 1 > /proc/sys/net/ipv6/conf/$f/ignore_routes_with_linkdown
	done
	echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore
	echo 2 > /proc/sys/net/ipv4/conf/all/arp_announce
fi
if [ -n "$F" ] || [ -n "$W" ]; then
	[ -f $S/watch.pid ] && kill $(cat $S/watch.pid) 2> /dev/null
	setsid sh $0 watch > /dev/null 2>&1 < /dev/null &
	echo $! > $S/watch.pid
fi
A=$(cf adb_allow); why="adb_allow = $A"
[ "${A:-auto}" = auto ] && { why="ro.adb.secure = $(getprop ro.adb.secure)"; [ "$(getprop ro.adb.secure)" = 1 ] && A=0 || A=1; }
if [ "$A" != 1 ]; then
	iptables -D INPUT -p tcp --dport 5555 -j tabd_adb 2> /dev/null
	log "adb allow-list off ($why: adb asks for the PC's key); DHCP on${n:- nothing}"
elif [ -n "$PC" ]; then
	iptables -N tabd_adb 2> /dev/null; iptables -F tabd_adb
	iptables -A tabd_adb -i lo -j ACCEPT
	iptables -A tabd_adb -s $PC -j ACCEPT
	[ -n "$PCMAC" ] && iptables -A tabd_adb -m mac --mac-source $PCMAC -j ACCEPT
	iptables -A tabd_adb -j DROP
	iptables -C INPUT -p tcp --dport 5555 -j tabd_adb 2> /dev/null || iptables -I INPUT -p tcp --dport 5555 -j tabd_adb
	ip6tables -C INPUT -p tcp --dport 5555 ! -i lo -j DROP 2> /dev/null || ip6tables -I INPUT -p tcp --dport 5555 ! -i lo -j DROP
	log "adb only from $PC${PCMAC:+ or MAC $PCMAC} ($why); DHCP on${n:- nothing}"
else
	log "no PC in $S/pc: adb left open ($why); DHCP on${n:- nothing}"
fi
