"""Run ESP-IDF's idf.py from a plain POSIX shell.

This machine's shells always export MSYSTEM=MINGW64, which IDF 6.x treats as an
unsupported MSys environment. The toolchain is a native Windows install, so the
variable is pure noise; strip it before handing control to idf.py.
"""
import os
import runpy
import sys

for var in ("MSYSTEM", "MINGW_PREFIX", "MINGW_CHOST"):
    os.environ.pop(var, None)

IDF_PY = r"D:\esp-idf-v6.1\tools\idf.py"
sys.path.insert(0, os.path.dirname(IDF_PY))
sys.argv = [IDF_PY] + sys.argv[1:]
runpy.run_path(IDF_PY, run_name="__main__")
