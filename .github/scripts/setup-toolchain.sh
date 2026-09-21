#!/usr/bin/env bash
# Installs the tools needed to build Nimbus inside the devkitpro/devkitarm image.
# (makerom, bannertool, armips and flips are built from source and cached; libctrpf and 3gxtool are pacman packages.)
#   setup-toolchain.sh app       -> what `make app` needs (CIA: makerom + bannertool, zlib)
#   setup-toolchain.sh patches   -> what `make patches` needs (armips, flips, 3gxtool, libctrpf, zip)
# Used by the GitHub workflows and by local Docker test builds, so both run exactly the same steps.
set -euo pipefail

what="${1:-all}"
export DEBIAN_FRONTEND=noninteractive
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
src=/tmp/nimbus-tool-src
mkdir -p "$src"

apt-get update -qq
apt-get install -y -qq --no-install-recommends git cmake build-essential zip unzip curl jq ca-certificates pkg-config libpng-dev libssl-dev >/dev/null

dkp-pacman -Sy --noconfirm >/dev/null
dkp-pacman -S --noconfirm --needed 3ds-dev 3ds-zlib 3ds-curl 3ds-mbedtls >/dev/null

# Everything built from source below lands in /usr/local/bin; those files are what CI caches
# (see .github/workflows/toolchain-cache.yml). A tool that is already there is not rebuilt.
have() { [ -x "/usr/local/bin/$1" ]; }

build_makerom() {
	if have makerom; then echo "makerom: cached"; return; fi
	git clone --depth 1 https://github.com/3DSGuy/Project_CTR "$src/ctr"
	make -C "$src/ctr/makerom" deps -j"$(nproc)" >/dev/null
	make -C "$src/ctr/makerom" -j"$(nproc)" >/dev/null
	install -m755 "$src/ctr/makerom/bin/makerom" /usr/local/bin/makerom
}

build_bannertool() {
	if have bannertool; then echo "bannertool: cached"; return; fi
	git clone --depth 1 --recursive https://github.com/carstene1ns/3ds-bannertool "$src/bannertool"
	cmake -S "$src/bannertool" -B "$src/bannertool/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$src/bannertool/build" -j"$(nproc)" >/dev/null
	install -m755 "$(find "$src/bannertool/build" -type f -name bannertool -perm -u+x | head -n1)" /usr/local/bin/bannertool
}

build_armips() {
	if have armips; then echo "armips: cached"; return; fi
	git clone --depth 1 --recursive https://github.com/Kingcom/armips "$src/armips"
	cmake -S "$src/armips" -B "$src/armips/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$src/armips/build" -j"$(nproc)" >/dev/null
	install -m755 "$src/armips/build/armips" /usr/local/bin/armips
}

build_flips() {
	if have flips; then echo "flips: cached"; return; fi
	git clone --depth 1 https://github.com/Alcaro/Flips "$src/flips"
	make -C "$src/flips" -f Makefile TARGET=cli -j"$(nproc)" >/dev/null
	install -m755 "$src/flips/flips" /usr/local/bin/flips
}

# libctrpf and 3gxtool come from the framework's own package repositories (fast; nothing to cache).
install_ctrpf_and_3gxtool() {
	local conf="$DEVKITPRO/pacman/etc/pacman.conf"
	grep -Fxq "[thepixellizeross-lib]" "$conf" || printf '\n[thepixellizeross-lib]\nServer = https://thepixellizeross.gitlab.io/packages/any\nSigLevel = Optional\n' >> "$conf"
	grep -Fxq "[thepixellizeross-linux]" "$conf" || printf '\n[thepixellizeross-linux]\nServer = https://thepixellizeross.gitlab.io/packages/x86_64/linux\nSigLevel = Optional\n' >> "$conf"
	dkp-pacman -Sy --noconfirm >/dev/null
	dkp-pacman -S --noconfirm --needed libctrpf 3gxtool >/dev/null
}

case "$what" in
	app)      build_makerom; build_bannertool ;;
	patches)  build_armips; build_flips; install_ctrpf_and_3gxtool ;;
	all)      build_makerom; build_bannertool; build_armips; build_flips; install_ctrpf_and_3gxtool ;;
	*) echo "usage: $0 app|patches|all" >&2; exit 2 ;;
esac

echo "toolchain ready: $what"
