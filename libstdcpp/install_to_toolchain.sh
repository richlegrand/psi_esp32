#!/bin/bash
# Install patched libstdc++ to ESP-IDF toolchain
# Backs up original files before overwriting

set -e

TOOLCHAIN_BASE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf
PATCHED_DIR="$(dirname "$0")/install"

echo "=== Installing patched libstdc++ to toolchain ==="
echo "Toolchain: $TOOLCHAIN_BASE"
echo "Patched:   $PATCHED_DIR"
echo ""

# Check that patched files exist
if [ ! -f "$PATCHED_DIR/lib/libstdc++.a" ]; then
    echo "ERROR: Patched library not found. Run build.sh first."
    exit 1
fi

# Backup and copy libraries
echo "Backing up and replacing libraries..."
for lib in libstdc++.a libsupc++.a; do
    if [ ! -f "$TOOLCHAIN_BASE/lib/$lib.orig" ]; then
        echo "  Backing up $lib -> $lib.orig"
        cp "$TOOLCHAIN_BASE/lib/$lib" "$TOOLCHAIN_BASE/lib/$lib.orig"
    else
        echo "  Backup already exists: $lib.orig"
    fi
    echo "  Copying $lib"
    cp "$PATCHED_DIR/lib/$lib" "$TOOLCHAIN_BASE/lib/$lib"
done

# Backup and copy headers
echo ""
echo "Backing up and replacing headers..."

# c++config.h (has _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY)
# Need to patch ALL multilib variants, not just the base one
echo "  Finding all c++config.h variants..."
find "$TOOLCHAIN_BASE/include/c++/14.2.0/riscv32-esp-elf" -name "c++config.h" | while read config_file; do
    if [ ! -f "$config_file.orig" ]; then
        echo "    Backing up $(basename $(dirname $config_file))/c++config.h"
        cp "$config_file" "$config_file.orig"
    fi
    echo "    Patching $(basename $(dirname $config_file))/c++config.h"
    cp "$PATCHED_DIR/include/c++/14.2.0/riscv32-esp-elf/bits/c++config.h" "$config_file"
done

# shared_mutex (has destructor fix)
SHARED_MUTEX="include/c++/14.2.0/shared_mutex"
if [ ! -f "$TOOLCHAIN_BASE/$SHARED_MUTEX.orig" ]; then
    echo "  Backing up shared_mutex -> shared_mutex.orig"
    cp "$TOOLCHAIN_BASE/$SHARED_MUTEX" "$TOOLCHAIN_BASE/$SHARED_MUTEX.orig"
else
    echo "  Backup already exists: shared_mutex.orig"
fi
echo "  Copying shared_mutex"
cp "$PATCHED_DIR/include/c++/14.2.0/shared_mutex" "$TOOLCHAIN_BASE/$SHARED_MUTEX"

echo ""
echo "=== Installation complete ==="
echo ""
echo "To use: rebuild your ESP-IDF project with 'idf.py fullclean && idf.py build'"
echo "To restore originals: run restore_toolchain.sh"
