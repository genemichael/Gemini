"""
PlatformIO pre-build script: patches the libdeps copy of microStore's
FileStore.h before each build. Rewritten 2026-07-22 for the pinned
microStore c5fb69d (0.1.6) — the original pyxis version of this script
carried byte-exact patterns from a pre-0.1.6 FileStore.h and had been
silently no-oping against this pin (its patch() printed nothing when a
pattern missed; both defects it targets were observed live on-device).

Two patches:

1. Silences microStore's per-operation prints ("get: fetching key",
   "get: returning key", "get: key ... not found in index",
   "put: storing key", "put: wrote key"). RNS hits the path store on
   every routed packet; these flood USB CDC at hundreds of lines/sec
   (pyxis issue #75 — starved serial responses during calls). Rare
   lifecycle prints (compaction, "Closing active file") and error
   prints ("header read failed") are kept.

2. Closes `active_file` before finalize_compaction() unlinks/renames
   segment files. ESP32 LittleFS cannot unlink or rename over an open
   FD; observed on-device 2026-07-22: "Failed to unlink path
   \"/path_store_0.dat\". Has open FD." followed by a stale segment
   surviving compaction and subsequent "header read failed" on gets.

Idempotent: each patch checks for its own marker before applying.
Any anchor that fails to match prints a loud WARNING — never silent.
"""
Import("env")
import os, sys
sys.path.insert(0, env.get("PROJECT_DIR", "."))
from _build_helpers import env_libdeps_dir  # per-env libdeps path; never hardcode the env

FILESTORE_H = env_libdeps_dir(env, "microStore", "include", "microStore", "FileStore.h")

SPAM_MARKERS = (
    'printf("[ustore] get: fetching key',
    'printf("[ustore] get: returning key',
    'not found in index',
    'printf("[ustore] put: storing key',
    'printf("[ustore] put: wrote key',
)
SILENCE_TAG = "/* pyxis-silenced per-op print */"

FDLEAK_ANCHOR = "// Remove all existing segments, then rename tmp"
FDLEAK_TAG = "pyxis-local: close active_file before unlinking"
FDLEAK_INSERT = (
    "\t\t// %s -- ESP32 LittleFS cannot\n"
    "\t\t// unlink/rename over an open FD; a stale segment surviving\n"
    "\t\t// compaction resurfaces dead records and corrupts reads.\n"
    "\t\tif (active_file) active_file.close();\n" % FDLEAK_TAG
)

def patch(content):
    lines = content.split("\n")
    out = []
    silenced = 0
    for line in lines:
        if (SILENCE_TAG not in line
                and 'printf("[ustore]' in line
                and any(m in line for m in SPAM_MARKERS)):
            # Comment the whole single-line statement out in place.
            indent_len = len(line) - len(line.lstrip())
            line = (line[:indent_len] + SILENCE_TAG + " // "
                    + line[indent_len:])
            silenced += 1
        out.append(line)
    content = "\n".join(out)
    if silenced:
        print("PATCH: FileStore.h: silenced %d per-op prints" % silenced)
    elif SILENCE_TAG in content:
        print("PATCH: FileStore.h: per-op prints already silenced")
    else:
        print("PATCH: WARNING -- FileStore.h: no per-op prints matched "
              "(upstream changed? re-verify patch_filestore.py)")

    if FDLEAK_TAG in content:
        print("PATCH: FileStore.h: finalize_compaction FD fix already applied")
    elif content.count(FDLEAK_ANCHOR) == 1:
        idx = content.index(FDLEAK_ANCHOR)
        line_start = content.rfind("\n", 0, idx) + 1
        content = content[:line_start] + FDLEAK_INSERT + content[line_start:]
        print("PATCH: FileStore.h: close active_file before compaction unlink")
    else:
        print("PATCH: WARNING -- FileStore.h: finalize_compaction anchor "
              "matched %d times (expected 1); FD-leak fix NOT applied"
              % content.count(FDLEAK_ANCHOR))
    return content

if os.path.exists(FILESTORE_H):
    with open(FILESTORE_H) as f:
        original = f.read()
    patched = patch(original)
    if patched != original:
        with open(FILESTORE_H, "w") as f:
            f.write(patched)
else:
    print("PATCH: WARNING -- FileStore.h not found at %s" % FILESTORE_H)
