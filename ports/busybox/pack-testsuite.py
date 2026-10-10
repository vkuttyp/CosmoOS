#!/usr/bin/env python3
"""pack-testsuite.py SRC_TESTSUITE_DIR OUTPUT.tgz

BusyBox's testsuite directory as a reproducible gzipped tar for the boot
image (tests/busybox/suite.sh unpacks it with BusyBox tar): entries
sorted, under testsuite/, owner 0, mtime 0, the file's own permission
bits, the gzip header's mtime 0.
"""
import gzip
import io
import os
import sys
import tarfile


def main(argv):
    src, out = argv[1], argv[2]
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        paths = []
        for root, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(dirs + files):
                paths.append(os.path.join(root, name))
        for path in [src] + sorted(paths):
            arc = "testsuite" + path[len(src):]
            info = tar.gettarinfo(path, arc)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            if info.isreg():
                with open(path, "rb") as f:
                    tar.addfile(info, f)
            else:
                tar.addfile(info)
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", mtime=0, filename="") as gz:
            gz.write(buf.getvalue())
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
