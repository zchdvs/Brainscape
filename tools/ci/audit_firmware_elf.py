#!/usr/bin/env python3
"""firmware-elf-audit (docs/design/companion-app.md §3.4, §7.2; determinism-profile.md §8.4
step 13): what each firmware image links that a GPLv3 release cannot carry, and whether the
engine archive inside it is position-dependent.

For every image (its ELF and the linker map beside it) it lists:
  * ST middleware under SLA0044: objects from libDaisy's src/usbd/, src/usbh/,
    src/util/*diskio*, ST's USB device and host libraries, and the symbols USBD_*, USBH_*,
    SD_Driver;
and for the engine archive (--archive) it requires:
  * no GOT relocation (R_ARM_GOT*): the archive is built without -fPIC (companion §3.2).

  audit_firmware_elf.py [--prototype] --readelf arm-none-eabi-readelf --nm arm-none-eabi-nm
                        --archive libbrainscape_dsp.a IMAGE.elf...

Exit 1 on a GOT relocation, and on any SLA0044 code unless --prototype: the bring-up images
(firmware/README.md) use libDaisy's USB CDC on purpose and are not distributed, so they are
audited and labelled, not failed. The product firmware moves to TinyUSB and its own SD glue
(companion §7.2) and runs this without --prototype.
"""
import argparse
import os
import re
import subprocess
import sys

SLA_OBJECTS = re.compile(
    r"(libdaisy\.a\((usbd_[a-z_]+|usbh_[a-z_]+|[a-z_]*diskio)\.c\.obj\)"
    r"|libSTM32_USB_(DEVICE|HOST)_LIBRARY\.a\([^)]*\))")
SLA_SYMBOLS = re.compile(r"^(USBD_\w+|USBH_\w+|SD_Driver)$")
GOT = re.compile(r"R_ARM_GOT\w*")


def members(map_path):
    """Archive members the link pulled in, from the map's first section."""
    out = set()
    with open(map_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("Allocating common symbols") or line.startswith("Discarded input sections"):
                break
            m = SLA_OBJECTS.search(line.split(" ")[0] if line[:1] not in " \t" else "")
            if m:
                out.add(m.group(1))
    return sorted(out)


def symbols(nm, elf):
    text = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    found = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "TtDdBbRr" and SLA_SYMBOLS.match(parts[2]):
            found.add(parts[2])
    return sorted(found)


def got_relocations(readelf, archive):
    text = subprocess.run([readelf, "-r", archive], check=True, capture_output=True, text=True).stdout
    return sorted({m.group(0) for m in GOT.finditer(text)})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prototype", action="store_true",
                    help="report SLA0044 code instead of failing (undistributed bring-up images)")
    ap.add_argument("--readelf", default="arm-none-eabi-readelf")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--archive", action="append", default=[], help="engine archive(s) to check for GOT relocations")
    ap.add_argument("images", nargs="+", help="firmware ELF files (the .map must sit beside each)")
    args = ap.parse_args()

    failed = False
    for archive in args.archive:
        got = got_relocations(args.readelf, archive)
        print("%s  %s: %s" % ("FAIL" if got else "ok  ", archive, ", ".join(got) if got else "no GOT relocation"))
        failed = failed or bool(got)
    for elf in args.images:
        map_path = os.path.splitext(elf)[0] + ".map"
        objs = members(map_path) if os.path.exists(map_path) else ["(no map file: " + map_path + ")"]
        syms = symbols(args.nm, elf)
        if not objs and not syms:
            print("ok    %s: no SLA0044 code" % elf)
            continue
        verdict = "NOTE" if args.prototype else "FAIL"
        print("%s  %s: links ST SLA0044 code (%d objects, %d symbols): NOT DISTRIBUTABLE" % (
            verdict, elf, len(objs), len(syms)))
        for o in objs:
            print("        object  %s" % o)
        shown = syms[:12]
        print("        symbols %s%s" % (", ".join(shown), ", ..." if len(syms) > len(shown) else ""))
        failed = failed or not args.prototype
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
