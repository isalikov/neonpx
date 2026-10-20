#!/bin/sh

# Removes the NeonPX binary installed by scripts/install.sh. NeonPX keeps no settings.

set -eu

prefix=${NEONPX_PREFIX:-$HOME/.local}
binary=$prefix/bin/neonpx

if [ -f "$binary" ]; then
	rm -f "$binary"
	printf '%s\n' "Removed $binary"
else
	printf '%s\n' "No binary at $binary"
fi
