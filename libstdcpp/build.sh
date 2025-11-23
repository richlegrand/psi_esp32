#!/bin/bash
# Build script for patched libstdc++ for ESP32-P4
# Includes fixes for:
# 1. std::shared_mutex memory leak (pthread_rwlock_destroy)
# 2. Atomic lock policy for shared_ptr (saves ~88 bytes Internal RAM per shared_ptr)

set -e

# Set up ESP-IDF environment
source ~/pixy3/esp-idf-v5.5.1/export.sh

BUILD_DIR="$(dirname "$0")/build"
SRC_DIR="$(dirname "$0")/gcc/libstdc++-v3"
INSTALL_DIR="$(dirname "$0")/install"

echo "=== Building patched libstdc++ for ESP32-P4 ==="
echo "Build directory: $BUILD_DIR"
echo "Source directory: $SRC_DIR"
echo "Install directory: $INSTALL_DIR"

cd "$BUILD_DIR"

# Configure for cross-compilation to riscv32-esp-elf
echo ""
echo "=== Configuring libstdc++ ==="
"$SRC_DIR/configure" \
  --host=riscv32-esp-elf \
  --target=riscv32-esp-elf \
  --disable-multilib \
  --disable-hosted-libstdcxx \
  --with-newlib \
  --disable-nls \
  --enable-threads=posix \
  --prefix="$INSTALL_DIR" \
  CC=riscv32-esp-elf-gcc \
  CXX=riscv32-esp-elf-g++

echo ""
echo "=== Building libstdc++ ==="
make -j$(nproc)

echo ""
echo "=== Installing libstdc++ ==="
make install

echo ""
echo "=== Build complete! ==="
echo "Patched libstdc++ installed to: $INSTALL_DIR"
echo ""
echo "To use in your ESP-IDF project, add to CMakeLists.txt:"
echo "  target_include_directories(\${COMPONENT_LIB} PUBLIC $INSTALL_DIR/include)"
echo "  target_link_directories(\${COMPONENT_LIB} PUBLIC $INSTALL_DIR/lib)"
