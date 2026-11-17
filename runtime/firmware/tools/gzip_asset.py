"""gzip one page for the firmware image (main/CMakeLists.txt), reproducibly:
level 9, no name or time in the header, so the same page always gives the
same bytes (and the preview's ETag only changes with the preview).

    py tools/gzip_asset.py <in> <out>
"""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    data = f.read()
with open(dst, 'wb') as f:
    f.write(gzip.compress(data, 9, mtime=0))
