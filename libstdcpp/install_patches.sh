#!/bin/bash
# Install libstdc++ patches for ESP32-P4
# Fixes:
#  1. Atomic lock policy for shared_ptr (saves ~88 bytes Internal RAM per shared_ptr)
#  2. shared_mutex destructor memory leak (saves ~176 bytes per shared_mutex)

set -e

TOOLCHAIN_BASE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf
PATCH_DIR="$(dirname "$0")"

echo "=== Installing libstdc++ patches for ESP32-P4 ==="
echo "Toolchain: $TOOLCHAIN_BASE"
echo ""

# Check that patched shared_mutex exists
if [ ! -f "$PATCH_DIR/gcc/libstdc++-v3/include/std/shared_mutex" ]; then
    echo "ERROR: Patched shared_mutex not found at $PATCH_DIR/gcc/libstdc++-v3/include/std/shared_mutex"
    echo "Have you cloned and patched the GCC sources?"
    exit 1
fi

# ============================================================================
# PATCH 1: Enable atomic lock policy in all c++config.h variants
# ============================================================================
echo "PATCH 1: Enabling atomic lock policy for shared_ptr..."
echo ""

found_count=0
patched_count=0
already_patched_count=0

find "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf" -name "c++config.h" | while read config_file; do
    found_count=$((found_count + 1))

    # Check if already patched
    if grep -q "^#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1" "$config_file"; then
        echo "  ✓ Already patched: $(basename $(dirname $config_file))/c++config.h"
        already_patched_count=$((already_patched_count + 1))
        continue
    fi

    # Backup if not already backed up
    if [ ! -f "$config_file.orig" ]; then
        cp "$config_file" "$config_file.orig"
    fi

    # Patch: change /* #undef _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY */ to #define
    echo "  ✓ Patching: $(basename $(dirname $config_file))/c++config.h"
    sed -i 's|/\* #undef _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY \*/|#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1|' "$config_file"
    patched_count=$((patched_count + 1))
done

# Verify ESP32-P4 variant is patched
echo ""
if grep -q "^#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1" \
    "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf/rv32imafc_zicsr_zifencei/ilp32f/bits/c++config.h"; then
    echo "✓ ESP32-P4 variant (rv32imafc_zicsr_zifencei/ilp32f) confirmed patched"
else
    echo "✗ ERROR: ESP32-P4 variant not patched!"
    exit 1
fi

# ============================================================================
# PATCH 2: Install fixed shared_mutex header
# ============================================================================
echo ""
echo "PATCH 2: Installing fixed shared_mutex header..."
echo ""

SHARED_MUTEX="$TOOLCHAIN_BASE/include/c++/14.2.0/shared_mutex"

# Backup original
if [ ! -f "$SHARED_MUTEX.orig" ]; then
    echo "  Backing up original shared_mutex"
    cp "$SHARED_MUTEX" "$SHARED_MUTEX.orig"
fi

# Install patched version
echo "  ✓ Installing patched shared_mutex"
cp "$PATCH_DIR/gcc/libstdc++-v3/include/std/shared_mutex" "$SHARED_MUTEX"

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "=== Installation Complete ==="
echo ""
echo "Changes made:"
echo "  - Atomic lock policy enabled in all c++config.h variants"
echo "  - shared_mutex destructor now properly calls pthread_rwlock_destroy()"
echo ""
echo "Expected memory savings:"
echo "  - ~88 bytes Internal RAM per shared_ptr instance (no mutex)"
echo "  - ~176 bytes Internal RAM per shared_mutex instance (proper cleanup)"
echo ""
echo "Next steps:"
echo "  1. cd /home/rich/pixy3/esp32_libdatachannel"
echo "  2. idf.py fullclean"
echo "  3. idf.py build"
echo ""
echo "To restore original toolchain, run: $PATCH_DIR/restore_patches.sh"
