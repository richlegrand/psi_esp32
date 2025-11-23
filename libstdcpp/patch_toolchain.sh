#!/bin/bash
# Patch existing toolchain c++config.h files to enable atomic lock policy
# This patches in place rather than replacing with incompatible headers

set -e

TOOLCHAIN_BASE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf

echo "=== Patching toolchain c++config.h for atomic lock policy ==="
echo "Toolchain: $TOOLCHAIN_BASE"
echo ""

# Find and patch all c++config.h files
echo "Finding all c++config.h files..."
find "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf" -name "c++config.h" | while read config_file; do
    # Check if already patched
    if grep -q "^#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1" "$config_file"; then
        echo "  Already patched: $(basename $(dirname $config_file))/c++config.h"
        continue
    fi

    # Backup if not already backed up
    if [ ! -f "$config_file.orig" ]; then
        echo "  Backing up: $(basename $(dirname $config_file))/c++config.h"
        cp "$config_file" "$config_file.orig"
    fi

    # Patch: change /* #undef _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY */ to #define
    echo "  Patching: $(basename $(dirname $config_file))/c++config.h"
    sed -i 's|/\* #undef _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY \*/|#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1|' "$config_file"
done

echo ""
echo "=== Verification ==="
# Verify the patch worked
if grep -q "^#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1" "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf/rv32imafc_zicsr_zifencei/ilp32f/bits/c++config.h"; then
    echo "✓ ESP32-P4 variant (rv32imafc_zicsr_zifencei/ilp32f) patched successfully"
else
    echo "✗ ESP32-P4 variant patch FAILED"
    exit 1
fi

echo ""
echo "=== Complete ==="
echo "Rebuild your project with: idf.py fullclean && idf.py build"
