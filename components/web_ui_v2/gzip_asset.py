#!/usr/bin/env python3
"""Gzip a file for embedding (portable — used by both web UI components'
CMakeLists so the build needs no external gzip binary).

  python gzip_asset.py <src> <dst.gz>
"""
import gzip
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: gzip_asset.py <src> <dst.gz>\n")
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f_in, gzip.open(dst, "wb", compresslevel=9) as f_out:
        shutil.copyfileobj(f_in, f_out)


if __name__ == "__main__":
    main()
