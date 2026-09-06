#!/usr/bin/env python3
"""Minify + gzip the web UI for embedding (portable: the IDF Python only, no
node or external gzip binary).

  python gzip_asset.py <src.html|src.js|src.css> <dst.gz> [--no-minify]

Minification (2026-09-06): the inline <script> blocks go through rjsmin
and the <style> blocks through rcssmin (both vendored under tools/,
Apache-2.0, pure Python); HTML comments and leading indentation are
stripped from the markup. Measured on index.html: 85 KB -> ~70 KB gzipped
with no functional change, which is what pays for the dashboard widgets
under the "no bigger web UI" rule. The preview/probe tooling can build
from the same function (make_preview.py --min) so the tests run against
what ships. Bang comments (/*! ... */) survive — that is where the licence
note of any vendored snippet lives.
"""
import gzip
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))

import rcssmin  # noqa: E402  (vendored, Apache-2.0)
import rjsmin   # noqa: E402  (vendored, Apache-2.0)

_BLOCK = re.compile(r"(<script\b[^>]*>)(.*?)(</script>)|(<style\b[^>]*>)(.*?)(</style>)",
                    re.S | re.I)
_HTML_COMMENT = re.compile(r"<!--(?!\[if).*?-->", re.S)


def _markup(chunk):
    """HTML between the blocks: drop comments, indentation and blank lines
    (attribute values never span lines in this file, so this is safe)."""
    chunk = _HTML_COMMENT.sub("", chunk)
    chunk = re.sub(r"\n[ \t]+", "\n", chunk)
    chunk = re.sub(r"\n{2,}", "\n", chunk)
    return chunk


def minify_html(text):
    """Return the minified page (str in, str out)."""
    out = []
    pos = 0
    for m in _BLOCK.finditer(text):
        out.append(_markup(text[pos:m.start()]))
        if m.group(1) is not None:
            if "src=" in m.group(1):
                out.append(m.group(0))       # external script tag: untouched
            else:
                out.append(m.group(1) + rjsmin.jsmin(m.group(2), keep_bang_comments=True)
                           + m.group(3))
        else:
            out.append(m.group(4) + rcssmin.cssmin(m.group(5), keep_bang_comments=True)
                       + m.group(6))
        pos = m.end()
    out.append(_markup(text[pos:]))
    return "".join(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) != 2:
        sys.stderr.write("usage: gzip_asset.py <src> <dst.gz> [--no-minify]\n")
        sys.exit(2)
    src, dst = args
    with open(src, "rb") as f_in:
        raw = f_in.read()
    low = src.lower()
    if "--no-minify" in flags:
        data = raw
    elif low.endswith((".html", ".htm")):
        data = minify_html(raw.decode("utf-8")).encode("utf-8")
    elif low.endswith(".js"):        # an on-demand page chunk (web/scripts.js)
        data = rjsmin.jsmin(raw.decode("utf-8"), keep_bang_comments=True).encode("utf-8")
    elif low.endswith(".css"):
        data = rcssmin.cssmin(raw.decode("utf-8"), keep_bang_comments=True).encode("utf-8")
    else:
        data = raw
    with gzip.open(dst, "wb", compresslevel=9) as f_out:
        f_out.write(data)
    sys.stdout.write("gzip_asset: %s %d -> %d bytes (%d gzipped)\n"
                     % (os.path.basename(src), len(raw), len(data), os.path.getsize(dst)))


if __name__ == "__main__":
    main()
