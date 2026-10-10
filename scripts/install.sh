#!/bin/sh

set -eu

die() {
	printf '%s\n' "$*" >&2
	exit 1
}

have() {
	command -v "$1" >/dev/null 2>&1
}

repo=${NEONPX_REPO:-isalikov/neonpx}
version=${NEONPX_VERSION:-latest}
prefix=${NEONPX_PREFIX:-$HOME/.local}
bin_dir=$prefix/bin

if [ "$version" = latest ]; then
	release_base=${NEONPX_RELEASE_BASE_URL:-https://github.com/$repo/releases/latest/download}
else
	release_base=${NEONPX_RELEASE_BASE_URL:-https://github.com/$repo/releases/download/$version}
fi

os=$(uname -s)
arch=$(uname -m)

case "$os" in
	Darwin)
		asset=neonpx-macos-universal.tar.gz
		;;
	Linux)
		case "$arch" in
			x86_64|amd64) asset=neonpx-linux-x86_64.tar.gz ;;
			arm64|aarch64) asset=neonpx-linux-aarch64.tar.gz ;;
			*) die "Unsupported architecture: $arch" ;;
		esac
		;;
	*)
		die "Unsupported operating system: $os"
		;;
esac

tmp_dir=$(mktemp -d 2>/dev/null || mktemp -d -t neonpx-install)
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

printf '%s\n' "Installing neonpx ($asset)"

if curl -fsSL "$release_base/$asset" -o "$tmp_dir/$asset"; then
	if curl -fsSL "$release_base/sha256sums.txt" -o "$tmp_dir/sha256sums.txt" 2>/dev/null; then
		expected=$(grep " $asset\$" "$tmp_dir/sha256sums.txt" | cut -d' ' -f1)
		if have sha256sum; then
			actual=$(sha256sum "$tmp_dir/$asset" | cut -d' ' -f1)
		else
			actual=$(shasum -a 256 "$tmp_dir/$asset" | cut -d' ' -f1)
		fi
		[ -z "$expected" ] || [ "$expected" = "$actual" ] || die "Checksum mismatch for $asset"
	fi
	tar -xzf "$tmp_dir/$asset" -C "$tmp_dir"
	install -d "$bin_dir"
	install -m 755 "$tmp_dir/neonpx" "$bin_dir/neonpx"
	printf '%s\n' "Installed to $bin_dir/neonpx"
else
	have make || die "No release asset at $release_base/$asset and make is not installed"
	have cc || die "No release asset at $release_base/$asset and no C compiler (cc) is installed"
	printf '%s\n' "Release asset not found, building from source"
	curl -fsSL "https://github.com/$repo/archive/refs/heads/master.tar.gz" -o "$tmp_dir/src.tar.gz" ||
		die "Could not download the source of $repo"
	tar -xzf "$tmp_dir/src.tar.gz" -C "$tmp_dir"
	src=$(find "$tmp_dir" -maxdepth 1 -type d -name 'neonpx-*' | head -n 1)
	make -C "$src" install PREFIX="$prefix" VERSION=source
fi

case ":$PATH:" in
	*":$bin_dir:"*)
		;;
	*)
		printf '%s\n' "Add $bin_dir to PATH if neonpx is not found after installation"
		;;
esac
