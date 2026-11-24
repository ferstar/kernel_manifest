#!/usr/bin/env python3
"""Build the flat KernelSU install archive and a separate source archive."""
import stat
import sys
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
props = dict(line.split("=", 1) for line in (ROOT / "module.prop").read_text().splitlines())
stem = "ksu-gt5pro-memcomp-" + props["version"]
out = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT.parent
out.mkdir(parents=True, exist_ok=True)
module = [
    "README.txt", "action.sh", "compressor.sh", "customize.sh", "module.prop",
    "osense.awk", "post-fs-data.sh", "skip_mount", "snapshot.sh",
    "vendor/install.sh", "vendor/oplus_bsp_schedinfo.ko", "vendor/patch-image",
    *["vendor/src/" + name for name in (
        "LICENSE", "footer.bin", "go.mod", "lz4.c", "lz4.h", "lz4hc.c",
        "lz4hc.h", "main.go", "vbmeta.bin",
    )],
    "webroot/app.js", "webroot/index.html", "webroot/style.css",
]
source = module + ["DEVELOPING.md", "tools/package.py", "tools/verify.cjs"]
executables = {"action.sh", "compressor.sh", "post-fs-data.sh", "snapshot.sh",
               "vendor/install.sh", "vendor/patch-image"}
for name, files in [(stem, module), (stem + "-source", source)]:
    with NamedTemporaryFile(dir=out, suffix=".tmp", delete=False) as tmp:
        path = Path(tmp.name)
    try:
        with ZipFile(path, "w") as archive:
            for item in sorted(files):
                info = ZipInfo(item)  # stable timestamps for repeatable rebuilds
                info.create_system = 3
                mode = 0o755 if item in executables else 0o644
                info.external_attr = (stat.S_IFREG | mode) << 16
                archive.writestr(info, (ROOT / item).read_bytes(), ZIP_DEFLATED, 9)
        path.chmod(0o644)
        final = out / (name + ".zip")
        path.replace(final)
        print(final)
    finally:
        path.unlink(missing_ok=True)
