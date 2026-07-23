"""
PlatformIO pre-build script: route esp32_codec2's heap into PSRAM.

Why (measured on device 2026-07-22 via [call][mem] instrumentation):
each Codec2 instance allocates ~10KB of state through codec2_malloc/
codec2_calloc (memtools.c). Two instances (separate encode/decode, per
pyxis's contention design) took call-time internal RAM from 22.7KB free
to 2.6KB — a knife's edge that pyxis never saw because it had ~150KB
free at call time and no launcher/Lua/LVGL underneath. The codec state
is CPU-only data (no DMA, no ISR access), so PSRAM through the S3's
cache is safe; at 8kHz frame cadence the cache-miss cost is noise.

Rewrites memtools.c's allocator bodies to heap_caps_*(MALLOC_CAP_SPIRAM).
Idempotent; warns loudly if the upstream pattern ever drifts (lesson
from patch_filestore.py silently no-oping for months).
"""
Import("env")
import os, sys
sys.path.insert(0, env.get("PROJECT_DIR", "."))
from _build_helpers import env_libdeps_dir

MEMTOOLS = env_libdeps_dir(env, "esp32_codec2", "src", "memtools.c")

MARKER = "pyxis-hybrid: codec2 state in PSRAM"

OLD = """#include <stdlib.h>"""
NEW_INCLUDE = """#include <stdlib.h>
/* %s (see patch_codec2_psram.py) */
#include <esp_heap_caps.h>""" % MARKER

REPLACEMENTS = [
    ("    return malloc(size);",
     "    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);"),
    ("    return calloc(nmemb, size);",
     "    return heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM);"),
]

if os.path.exists(MEMTOOLS):
    with open(MEMTOOLS) as f:
        content = f.read()
    if MARKER in content:
        print("PATCH: memtools.c: codec2 PSRAM allocators already applied")
    elif OLD in content and all(o in content for o, _ in REPLACEMENTS):
        content = content.replace(OLD, NEW_INCLUDE, 1)
        for o, n in REPLACEMENTS:
            content = content.replace(o, n, 1)
        with open(MEMTOOLS, "w") as f:
            f.write(content)
        print("PATCH: memtools.c: codec2 allocators -> PSRAM")
    else:
        print("PATCH: WARNING -- memtools.c patterns not found; codec2 "
              "will allocate ~20KB of INTERNAL RAM per call "
              "(re-verify patch_codec2_psram.py against the pinned lib)")
else:
    print("PATCH: WARNING -- memtools.c not found at %s" % MEMTOOLS)
