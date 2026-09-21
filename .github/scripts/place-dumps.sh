#!/usr/bin/env bash
# Copies the module dumps from <dir> into the patch folders as code.bin.
#   place-dumps.sh <dir>
# Each dump is named <TitleID>.dec.code (GodMode9 "Extract .code"). Fails loudly if one is missing.
set -euo pipefail
dir="${1:?usage: place-dumps.sh <dir-with-dumps>}"

place() { # <patch folder> <title id>
	local f="$dir/$2.dec.code"
	if [ ! -f "$f" ]; then echo "missing dump: $2.dec.code (needed for patches/$1)" >&2; exit 1; fi
	cp "$f" "patches/$1/code.bin"
	echo "patches/$1 <- $2 ($(stat -c %s "$f") bytes)"   # sizes only; contents are never printed
}

place act       0004013000003802
place friends   0004013000003202
place http      0004013000002902
place socket    0004013000002E02
place ssl       0004013000002F02
place nim       0004013000002C02
place miiverse  000400300000BE02   # any region's Miiverse dump works
place mint      000400300000D602   # EUR eShop applet (offsets were derived from the EUR build)
