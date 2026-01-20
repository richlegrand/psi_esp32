#!/bin/bash
# Restore original libstdc++ to ESP-IDF toolchain

set -e

TOOLCHAIN_BASE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf

echo "=== Restoring original libstdc++ to toolchain ==="
echo "Toolchain: $TOOLCHAIN_BASE"
echo ""

# Restore libraries
echo "Restoring libraries..."
for lib in libstdc++.a libsupc++.a; do
    if [ -f "$TOOLCHAIN_BASE/lib/$lib.orig" ]; then
        echo "  Restoring $lib from $lib.orig"
        cp "$TOOLCHAIN_BASE/lib/$lib.orig" "$TOOLCHAIN_BASE/lib/$lib"
    else
        echo "  WARNING: No backup found for $lib"
    fi
done

# Restore headers
echo ""
echo "Restoring headers..."

echo "  Finding all c++config.h variants..."
find "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf" -name "c++config.h.orig" | while read orig_file; do
    config_file="${orig_file%.orig}"
    echo "    Restoring $(basename $(dirname $config_file))/c++config.h"
    cp "$orig_file" "$config_file"
done

SHARED_MUTEX="include/c++/14.2.0/shared_mutex"
if [ -f "$TOOLCHAIN_BASE/$SHARED_MUTEX.orig" ]; then
    echo "  Restoring shared_mutex from shared_mutex.orig"
    cp "$TOOLCHAIN_BASE/$SHARED_MUTEX.orig" "$TOOLCHAIN_BASE/$SHARED_MUTEX"
else
    echo "  WARNING: No backup found for shared_mutex"
fi

echo ""
echo "=== Restoration complete ==="
echo ""
echo "Rebuild your ESP-IDF project with 'idf.py fullclean && idf.py build'"
