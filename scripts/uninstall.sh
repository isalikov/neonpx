#!/bin/sh

# Removes the neonpx binary installed by scripts/install.sh. neonpx keeps no settings.

set -eu

prefix=${NEONPX_PREFIX:-$HOME/.local}
binary=$prefix/bin/neonpx

if [ -f "$binary" ]; then
	rm -f "$binary"
	printf '%s\n' "Removed $binary"
else
	printf '%s\n' "No binary at $binary"
fi
