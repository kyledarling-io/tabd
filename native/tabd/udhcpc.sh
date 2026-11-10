#!/system/bin/sh
# Event script for `busybox udhcpc` (busybox 1.22.1 is on /system), one instance per link: wired ones from net.sh, a
# wlan one from wifi.sh. Android's DHCP client never runs in appliance mode, so without this the lease is never renewed.
# The address carries a route metric by the link's place in links (tabd.conf): 100, 600, 1100, ..., so the first wins
# while it's up. The current address is kept until a lease is bound and replaced only if the server hands out a
# different one. There's no default route: the PC is on the same LAN. Runs as root; logs into tabd's log.
D=/data/local/tmp/tabd
L=$D/log/tabd.log
log() { echo "[udhcpc $interface $(cut -d' ' -f1 /proc/uptime)] $*" >> $L; }
LINKS=$(sed -n 's/^[ \t]*links[ \t]*=[ \t]*\([^#]*\).*/\1/p' $D/tabd.conf 2> /dev/null | tail -1)
M=; k=0; for l in ${LINKS:-eth0 wlan0}; do [ "$l" = "$interface" ] && M=$((100 + k * 500)) && break; k=$((k + 1)); done
M=${M:-$((100 + k * 500))}  # not in links: after the last one
bits() {  # 255.255.255.0 -> 24
	n=0
	for o in $(echo $1 | tr . ' '); do
		case $o in 255) n=$((n + 8)) ;; 254) n=$((n + 7)) ;; 252) n=$((n + 6)) ;; 248) n=$((n + 5)) ;; 240) n=$((n + 4)) ;;
		224) n=$((n + 3)) ;; 192) n=$((n + 2)) ;; 128) n=$((n + 1)) ;; esac
	done
	echo $n
}
ipnet() {  # 192.0.2.50 24 -> 192.0.2.0
	b=$2; r=
	for x in $(echo $1 | tr . ' '); do
		if [ $b -ge 8 ]; then m=255; b=$((b - 8)); else m=$(((255 << (8 - b)) & 255)); b=0; fi
		r=$r${r:+.}$((x & m))
	done
	echo $r
}
cur=$(ip -4 addr show $interface | awk '/inet /{print $2; exit}')
case "$1" in
deconfig) log "started, keeping ${cur:-no address}" ;;
bound|renew)
	P=${mask:-$(bits ${subnet:-255.255.255.0})}
	if [ -n "$cur" ] && [ "${cur%/*}" != "$ip" ]; then ip addr del $cur dev $interface; log "$1: address ${cur%/*} -> $ip"; fi
	# This iproute2 (2017) can't give an address a metric: add it without its automatic route, add the subnet route
	# with this link's metric, and drop an automatic metric-0 one left from before. No outage on a renew.
	N=$(ipnet $ip $P)/$P
	ip addr replace $ip/$P brd ${broadcast:-+} dev $interface noprefixroute
	ip route replace $N dev $interface proto kernel scope link src $ip metric $M
	# "metric 0" in a delete means any metric: only when an automatic (metric 0) route is there, and it sorts first
	ip -4 route show $N dev $interface | grep -qv " metric " && ip route del $N dev $interface metric 0
	log "$1: $ip/$P metric $M, lease ${lease}s from ${serverid}"
	;;
leasefail|nak) log "$1: keeping the current address" ;;
esac
