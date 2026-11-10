#!/usr/bin/env bash
# Build helper for the standalone panel test image.
# The shells on this box always inject MSYSTEM, which IDF 6.x rejects, so the
# actual idf.py invocation goes through _idfwrap.py.
set -e
export IDF_PATH="D:/esp-idf-v6.1"
export ESP_ROM_ELF_DIR="C:/Users/pc1/.espressif/tools/esp-rom-elfs/20241011/"
export IDF_PYTHON_ENV_PATH="C:/Users/pc1/.espressif/python_env/idf6.1_py3.12_env"
export ESP_IDF_VERSION="6.1.0"
export ESP_IDF_VERSION_MAJOR=6
export ESP_IDF_VERSION_MINOR=1
export ESP_IDF_VERSION_PATCH=0
export PATH="/c/Users/pc1/.espressif/python_env/idf6.1_py3.12_env/Scripts:/c/Users/pc1/.espressif/tools/ninja/1.12.1:/c/Users/pc1/.espressif/tools/cmake/4.0.3/bin:/c/Users/pc1/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin:/usr/bin:/bin:/c/WINDOWS/system32:/c/WINDOWS"

cd "E:/esp32-c5 android/esp-sdr"
PY="C:/Users/pc1/.espressif/python_env/idf6.1_py3.12_env/Scripts/python.exe"
"$PY" _idfwrap.py "$@"
