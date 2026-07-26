"""
Pre-build: strip the builder's home directory from strings embedded in
the firmware (assert/log __FILE__ paths leak the local username into
every published binary). Remaps $HOME -> /b at compile time; the home
path is derived here at build time so it never appears in the repo.
"""
Import("env")
import os
home = os.path.expanduser("~")
env.Append(CCFLAGS=[
    "-ffile-prefix-map=%s=/b" % home,
    "-fmacro-prefix-map=%s=/b" % home,
])
