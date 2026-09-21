#!/usr/bin/env bash
# Installs the tools needed to build Nimbus inside the devkitpro/devkitarm image.
#   setup-toolchain.sh app       -> what `make app` needs (CIA: makerom + bannertool, zlib)
#   setup-toolchain.sh patches   -> what `make patches` needs (armips, flips, 3gxtool, libctrpf, zip)
# Used by the GitHub workflows and by local Docker test builds, so both run exactly the same steps.
set -euo pipefail

what="${1:-all}"
export DEBIAN_FRONTEND=noninteractive
src=/tmp/nimbus-tool-src
mkdir -p "$src"

apt-get update -qq
apt-get install -y -qq --no-install-recommends git cmake build-essential zip unzip curl jq ca-certificates pkg-config libpng-dev libssl-dev >/dev/null

dkp-pacman -Sy --noconfirm >/dev/null
dkp-pacman -S --noconfirm --needed 3ds-dev 3ds-zlib >/dev/null

build_makerom() {
	git clone --depth 1 https://github.com/3DSGuy/Project_CTR "$src/ctr"
	make -C "$src/ctr/makerom" deps -j"$(nproc)" >/dev/null
	make -C "$src/ctr/makerom" -j"$(nproc)" >/dev/null
	install -m755 "$src/ctr/makerom/bin/makerom" /usr/local/bin/makerom
}

build_bannertool() {
	git clone --depth 1 --recursive https://github.com/carstene1ns/3ds-bannertool "$src/bannertool"
	make -C "$src/bannertool" -j"$(nproc)" >/dev/null
	install -m755 "$(find "$src/bannertool" -type f -name bannertool -perm -u+x | head -n1)" /usr/local/bin/bannertool
}

build_armips() {
	git clone --depth 1 --recursive https://github.com/Kingcom/armips "$src/armips"
	cmake -S "$src/armips" -B "$src/armips/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$src/armips/build" -j"$(nproc)" >/dev/null
	install -m755 "$src/armips/build/armips" /usr/local/bin/armips
}

build_flips() {
	git clone --depth 1 https://github.com/Alcaro/Flips "$src/flips"
	make -C "$src/flips" -f Makefile TARGET=cli -j"$(nproc)" >/dev/null
	install -m755 "$src/flips/flips" /usr/local/bin/flips
}

build_ctrpf_and_3gxtool() {
	git clone --depth 1 https://gitlab.com/thepixellizeross/3gxtool "$src/3gxtool"
	make -C "$src/3gxtool" -j"$(nproc)" >/dev/null
	install -m755 "$(find "$src/3gxtool" -type f -name 3gxtool -perm -u+x | head -n1)" /usr/local/bin/3gxtool
	git clone --depth 1 https://gitlab.com/thepixellizeross/ctrpluginframework "$src/ctrpf"
	make -C "$src/ctrpf" -j"$(nproc)" >/dev/null
	make -C "$src/ctrpf" install >/dev/null
}

case "$what" in
	app)      build_makerom; build_bannertool ;;
	patches)  build_armips; build_flips; build_ctrpf_and_3gxtool ;;
	all)      build_makerom; build_bannertool; build_armips; build_flips; build_ctrpf_and_3gxtool ;;
	*) echo "usage: $0 app|patches|all" >&2; exit 2 ;;
esac

echo "toolchain ready: $what"
