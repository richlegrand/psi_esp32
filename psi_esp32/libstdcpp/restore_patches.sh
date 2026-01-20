#!/bin/bash
# Restore original ESP-IDF toolchain libstdc++ files

set -e

TOOLCHAIN_BASE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf

echo "=== Restoring original libstdc++ ==="
echo "Toolchain: $TOOLCHAIN_BASE"
echo ""

# Restore all c++config.h variants
echo "Restoring c++config.h files..."
find "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf" -name "c++config.h.orig" | while read orig_file; do
    config_file="${orig_file%.orig}"
    echo "  ✓ Restoring $(basename $(dirname $config_file))/c++config.h"
    cp "$orig_file" "$config_file"
done

# Restore shared_mutex
echo ""
echo "Restoring shared_mutex..."
SHARED_MUTEX="$TOOLCHAIN_BASE/include/c++/14.2.0/shared_mutex"
if [ -f "$SHARED_MUTEX.orig" ]; then
    echo "  ✓ Restoring shared_mutex"
    cp "$SHARED_MUTEX.orig" "$SHARED_MUTEX"
else
    echo "  ⚠ No backup found for shared_mutex"
fi

echo ""
echo "=== Restoration Complete ==="
echo ""
echo "Rebuild your project with: idf.py fullclean && idf.py build"
