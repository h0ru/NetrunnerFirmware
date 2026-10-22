#!/usr/bin/env python3
"""
Enable exFAT in ESP-IDF's bundled FatFs.

ESP-IDF ships FatFs with exFAT hard-disabled (`#define FF_FS_EXFAT 0` in
components/fatfs/src/ffconf.h) and exposes no Kconfig option to turn it on, so
the firmware cannot mount exFAT cards -- the factory default for cards >= 64 GB.
A detected-but-exFAT card then fails to mount and the whole /ext tree (asset
packs, NFC/Sub-GHz/IR saves, everything) is unreadable.

This flips FF_FS_EXFAT to 1 in-place. Prerequisites are already met by the
project's sdkconfig: FF_USE_LFN >= 1 (CONFIG_FATFS_LFN_HEAP) and
FF_FS_READONLY == 0. FF_LBA64 stays 0 (only needed for > 2 TB volumes).

Idempotent: a second run detects the patch and does nothing. The pristine file
is saved next to it as ffconf.h.orig the first time, so the change is
reversible. Mirrors tools/patch_wifi_lib.py; run from CMake at configure time.

Usage: patch_fatfs_exfat.py <path/to/ffconf.h>
"""
import re
import shutil
import sys

PATTERN = re.compile(r"^(#define\s+FF_FS_EXFAT\s+)(\d+)(.*)$", re.MULTILINE)


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <ffconf.h>")
        sys.exit(1)

    ffconf = sys.argv[1]
    try:
        text = open(ffconf, encoding="utf-8").read()
    except OSError as e:
        print(f"exFAT patch: cannot read {ffconf}: {e}")
        sys.exit(1)

    m = PATTERN.search(text)
    if not m:
        print(f"exFAT patch: FF_FS_EXFAT not found in {ffconf} (FatFs layout changed?)")
        sys.exit(1)

    if m.group(2) == "1":
        print("exFAT patch: already enabled (FF_FS_EXFAT == 1), skipping")
        return

    # Preserve the pristine header once so the edit can be undone.
    backup = ffconf + ".orig"
    try:
        open(backup, "x", encoding="utf-8").write(text)
    except FileExistsError:
        pass

    patched = text[: m.start()] + m.group(1) + "1" + m.group(3) + text[m.end():]
    open(ffconf, "w", encoding="utf-8").write(patched)
    print(f"exFAT patch: enabled FF_FS_EXFAT in {ffconf} (backup: {backup})")


if __name__ == "__main__":
    main()
