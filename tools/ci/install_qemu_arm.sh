#!/usr/bin/env bash
# Builds the pinned QEMU Arm user-mode emulator (determinism profile §6.2, §6.8: every
# M7 measurement used QEMU 10.2.3; Ubuntu's 8.2.2 is untested) into $1 from the
# upstream release tag, records the tag's commit in $1/COMMIT, and fails unless the
# binary reports the pinned version. A cached build is reused after the same check.
set -euo pipefail

VERSION=10.2.3
REPO=https://gitlab.com/qemu-project/qemu.git
DEST=${1:?usage: install_qemu_arm.sh DEST}

if [ ! -x "$DEST/bin/qemu-arm" ]; then
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends ninja-build pkg-config libglib2.0-dev \
    python3-venv flex bison
  tmp=$(mktemp -d)
  git clone --quiet --depth 1 --branch "v$VERSION" "$REPO" "$tmp/qemu"
  mkdir -p "$tmp/build" "$DEST"
  (cd "$tmp/build" &&
    ../qemu/configure --prefix="$DEST" --target-list=arm-linux-user --disable-system \
      --disable-tools --disable-docs --disable-werror &&
    make -j"$(nproc)" &&
    make install)
  git -C "$tmp/qemu" rev-parse HEAD > "$DEST/COMMIT"
  rm -rf "$tmp"
fi
reported=$("$DEST/bin/qemu-arm" --version | head -1)
echo "$reported (v$VERSION at $(cat "$DEST/COMMIT"))"
case "$reported" in
  "qemu-arm version $VERSION" | "qemu-arm version $VERSION "*) ;;
  *) echo "expected qemu-arm version $VERSION" >&2; exit 1 ;;
esac
if [ -n "${GITHUB_PATH:-}" ]; then echo "$DEST/bin" >> "$GITHUB_PATH"; fi
