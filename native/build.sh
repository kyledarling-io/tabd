#!/bin/sh
# Build libml (ml + gs + moonlight-common-c + enet + nanors + mbedTLS), mlprobe and tabd.
#   build.sh [android]  arm64 Android with the NDK (r28c tested) -> native/build/tabd, plus native/build/android/mlprobe
#                       (static). The NDK: $NDK, else $ANDROID_NDK_HOME. Runs on Linux, macOS and Windows Git Bash.
#   build.sh host       this machine with gcc -> native/build/host/tabd and mlprobe, for the selftests and for trying
#                       ml against Sunshine without the tablet
# API (default 31, Android 12) is the oldest Android the binary runs on. CFLAGS adds flags for the first-party code
# (CI uses -Werror). After editing third_party/ itself: rm -r native/build/<target>.
set -e
cd "$(dirname "$0")"
T=${1:-android}
case $T in
android)
	NDK=${NDK:-$ANDROID_NDK_HOME}
	case $(uname -s) in
	Linux) H=linux-x86_64 ;;
	Darwin) H=darwin-x86_64 ;;
	*)	H=windows-x86_64  # Git Bash: with no NDK set, one unpacked in a top-level folder of the system drive
		if [ -z "$NDK" ]; then for d in "$(cygpath -u "${SYSTEMDRIVE:-C:}/")"*/android-ndk-r*; do [ -d "$d" ] && NDK=$d; done; fi ;;
	esac
	B=$NDK/toolchains/llvm/prebuilt/$H/bin
	[ -e "$B/clang" ] || [ -e "$B/clang.exe" ] || { echo "no NDK clang in $B: set NDK to an Android NDK (r28c tested)" >&2; exit 1; }
	CC="$B/clang --target=aarch64-linux-android${API:-31}"
	AR=$B/llvm-ar
	LDFLAGS="-static -s"  # mlprobe only: it runs anywhere on the tablet
	TABD=build/tabd
	;;
host)
	CC=${CC:-gcc}
	AR=ar
	LDFLAGS=
	TABD=build/host/tabd
	;;
*) echo "usage: $0 [android|host]" >&2; exit 2 ;;
esac
O=build/$T
M=third_party/moonlight-common-c
INC="-Ithird_party/mbedtls/include -I$M/src -I$M/enet/include -I$M/nanors -I$M/nanors/deps/obl"
DEFS="-DUSE_MBEDTLS -DNDEBUG -DHAS_SOCKLEN_T -DHAS_FCNTL -DHAS_IOCTL -DHAS_POLL -DHAS_GETADDRINFO -DHAS_GETNAMEINFO \
-DHAS_INET_PTON -DHAS_INET_NTOP -DHAS_MSGHDR_FLAGS"
# The third-party objects are built once, and again whenever their compile line (compiler, NDK, API, flags) changes.
TC="$CC -O2 -fPIC -std=gnu11 -w $DEFS $INC"
export TC O
if [ ! -f "$O/libthird.a" ] || [ "$(cat "$O/third.cc" 2>/dev/null)" != "$TC" ]; then
	rm -rf "$O/obj" "$O/libthird.a" "$O/third.cc"
	mkdir -p "$O/obj"
	ls third_party/mbedtls/library/*.c $M/src/*.c $M/enet/*.c $M/nanors/rs.c $M/nanors/deps/obl/oblas_common.c \
		$M/nanors/deps/obl/oblas_lite.c |
		xargs -P 16 -I{} sh -c 'o=$O/obj/$(echo {} | tr / _).o; $TC -c {} -o $o || exit 255'
	$AR rcs "$O/libthird.a" "$O"/obj/*.o
	printf '%s\n' "$TC" > "$O/third.cc"
fi
W="-O2 -Wall -Wextra -Wno-unused-parameter $CFLAGS"
$CC $W -fPIC -std=gnu11 $DEFS $INC -c ml/ml.c -o "$O/ml.o"  # always: a stale libml.a would link old ml code
$CC $W -fPIC -std=gnu11 $DEFS $INC -c ml/gs.c -o "$O/gs.o"
cp "$O/libthird.a" "$O/libml.a"
$AR rs "$O/libml.a" "$O/ml.o" "$O/gs.o"
$CC $W -fPIC -std=gnu11 $DEFS $INC $LDFLAGS -o "$O/mlprobe" ml/mlprobe.c "$O/libml.a" -pthread -lm
$CC $W -I. -Ithird_party/mpp/inc -o "$TABD" tabd/tabd.c "$O/libml.a" -ldl -pthread -lm
echo "built native/$TABD and native/$O/mlprobe"
