# Patched libstdc++ for ESP32-P4

Custom build of libstdc++ with critical memory optimizations for ESP32-P4.

## Fixes Included

### 1. Atomic Lock Policy for shared_ptr
- **Before**: Each `std::shared_ptr` allocated ~88 bytes of Internal RAM for a FreeRTOS mutex
- **After**: Uses atomic operations (no mutex needed)
- **Savings**: ~88 bytes Internal RAM per shared_ptr instance

GCC excludes RISC-V from atomic lock policy for ABI compatibility with desktop systems. ESP32 is embedded and doesn't need that compatibility.

### 2. std::shared_mutex Memory Leak Fix
- **Problem**: `~__shared_mutex_pthread()` was defaulted when `PTHREAD_RWLOCK_INITIALIZER` is defined, so `pthread_rwlock_destroy()` was never called
- **Fix**: Destructor now properly calls `pthread_rwlock_destroy()`
- **Impact**: Each std::shared_mutex leaked ~176 bytes of Internal RAM

## Installation

Use the provided scripts to install patches to your ESP-IDF toolchain:

```bash
cd /home/rich/pixy3/libstdcpp
./install_patches.sh
```

This will:
1. Patch all c++config.h variants to enable atomic lock policy
2. Install the fixed shared_mutex header
3. Back up original files with `.orig` extension

Then rebuild your project:
```bash
cd /home/rich/pixy3/esp32_libdatachannel
idf.py fullclean
idf.py build
```

To restore the original toolchain:
```bash
cd /home/rich/pixy3/libstdcpp
./restore_patches.sh
```

## Patches Applied

### 1. `gcc/libstdc++-v3/include/std/shared_mutex`
- Line 88: Added `_GLIBCXX_GTHRW(rwlock_destroy)` outside the `#ifndef PTHREAD_RWLOCK_INITIALIZER` block
- Lines 166-171: Changed defaulted destructor to explicitly call `pthread_rwlock_destroy()`

### 2. `gcc/libstdc++-v3/configure`
- Lines 16397-16400: Commented out RISC-V exclusion from atomic lock policy

### 3. `gcc/libstdc++-v3/acinclude.m4`
- Lines 4031-4035: Commented out RISC-V exclusion (for documentation, not used by pre-generated configure)

## Rebuilding

To rebuild after changes:
```bash
cd /home/rich/pixy3/libstdcpp
rm -rf build/* install/*
./build.sh
```

## Verification

After installation, verify atomic lock policy is enabled for ESP32-P4:
```bash
grep "HAVE_ATOMIC_LOCK_POLICY" ~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/riscv32-esp-elf/include/c++/14.2.0/riscv32-esp-elf/rv32imafc_zicsr_zifencei/ilp32f/bits/c++config.h
# Should show: #define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1
```

After rebuilding your project, check the backtraces for `_Lock_policy)2` (atomic) instead of `_Lock_policy)1` (mutex):
```
std::_Sp_counted_ptr_inplace<..., (__gnu_cxx::_Lock_policy)2>  ✓ Atomic
std::_Sp_counted_ptr_inplace<..., (__gnu_cxx::_Lock_policy)1>  ✗ Mutex (before patch)
```

## Source Information

- **Toolchain version**: esp-14.2.0_20241119
- **GCC source**: https://github.com/espressif/gcc.git (branch: esp-14.2.0_20241119)
- **crosstool-NG**: https://github.com/espressif/crosstool-NG.git (tag: esp-14.2.0_20241119)

## Results

After patching and rebuilding (tested on esp32_libdatachannel project):
- ✅ Atomic lock policy enabled: `_Lock_policy)2` in backtraces
- ✅ shared_ptr mutex leaks eliminated (~88 bytes per instance)
- ✅ Internal RAM usage improved from 2 mutex leaks to 1 std::future leak
- ✅ Memory returns to baseline after connection cleanup
