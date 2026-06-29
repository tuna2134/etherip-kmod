#!/bin/sh
set -eu

KVER="$(uname -r)"
KDIR="/lib/modules/$KVER/build"

if [ ! -d "$KDIR" ]; then
	echo "error: kernel headers for running kernel $KVER were not found at $KDIR" >&2
	echo "install matching Arch headers or reboot into the kernel that matches installed headers" >&2
	exit 1
fi

make KDIR="$KDIR"
