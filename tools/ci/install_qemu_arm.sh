#!/usr/bin/env bash
# Builds the pinned QEMU Arm user-mode emulator (determinism profile §6.2, §6.8: every
# M7 measurement used QEMU 10.2.3; Ubuntu's 8.2.2 is untested) into $1 from the
# upstream release tag, and fails unless the tag resolves to the pinned commit and the
# binary reports the pinned version. A cached build is reused after the same checks.
set -euo pipefail

VERSION=10.2.3
COMMIT=2e7e8b7eae85ad523378eefefd03bfa7fbbe92f4  # v10.2.3, recorded by the first CI run
REPO=https://gitlab.com/qemu-project/qemu.git
DEST=${1:?usage: install_qemu_arm.sh DEST}

if [ ! -x "$DEST/bin/qemu-arm" ]; then
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends ninja-build pkg-config libglib2.0-dev \
    python3-venv flex bison
  tmp=$(mktemp -d)
  git clone --quiet --depth 1 --branch "v$VERSION" "$REPO" "$tmp/qemu"
  cloned=$(git -C "$tmp/qemu" rev-parse HEAD)
  if [ "$cloned" != "$COMMIT" ]; then
    echo "v$VERSION resolved to $cloned, expected $COMMIT" >&2
    exit 1
  fi
  mkdir -p "$tmp/build" "$DEST"
  (cd "$tmp/build" &&
    ../qemu/configure --prefix="$DEST" --target-list=arm-linux-user --disable-system \
      --disable-tools --disable-docs --disable-werror &&
    make -j"$(nproc)" &&
    make install)
  git -C "$tmp/qemu" rev-parse HEAD > "$DEST/COMMIT"
  rm -rf "$tmp"
fi
built=$(cat "$DEST/COMMIT")
if [ "$built" != "$COMMIT" ]; then
  echo "cached qemu-arm was built from $built, expected $COMMIT" >&2
  exit 1
fi
reported=$("$DEST/bin/qemu-arm" --version | head -1)
echo "$reported (v$VERSION at $built)"
case "$reported" in
  "qemu-arm version $VERSION" | "qemu-arm version $VERSION "*) ;;
  *) echo "expected qemu-arm version $VERSION" >&2; exit 1 ;;
esac
if [ -n "${GITHUB_PATH:-}" ]; then echo "$DEST/bin" >> "$GITHUB_PATH"; fi
