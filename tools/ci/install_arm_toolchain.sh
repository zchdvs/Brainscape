#!/usr/bin/env bash
# Installs the pinned GNU Arm Embedded Toolchain 10.3-2021.10 (determinism profile
# §6.8: every M7 measurement used it; release firmware, the oracle and golden minting
# pin it) into $1 and puts its bin/ on GITHUB_PATH. The archive is checked against
# Arm's published MD5 before it is unpacked; a cached install is reused.
set -euo pipefail

VERSION=10.3-2021.10
ARCHIVE=gcc-arm-none-eabi-${VERSION}-x86_64-linux.tar.bz2
URL=https://developer.arm.com/-/media/Files/downloads/gnu-rm/${VERSION}/${ARCHIVE}
MD5=2383e4eb4ea23f248d33adc70dc3227e
DEST=${1:?usage: install_arm_toolchain.sh DEST}

if [ ! -x "$DEST/bin/arm-none-eabi-g++" ]; then
  tmp=$(mktemp -d)
  curl -fsSL --retry 3 -o "$tmp/$ARCHIVE" "$URL"
  echo "$MD5  $tmp/$ARCHIVE" | md5sum -c -
  mkdir -p "$DEST"
  tar -xjf "$tmp/$ARCHIVE" -C "$DEST" --strip-components=1
  rm -rf "$tmp"
fi
"$DEST/bin/arm-none-eabi-g++" --version | head -1
if [ -n "${GITHUB_PATH:-}" ]; then echo "$DEST/bin" >> "$GITHUB_PATH"; fi
