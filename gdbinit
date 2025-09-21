file build/esp32_libdatachannel.elf
target remote :3333
monitor reset halt
#thb do_global_ctors 
thb app_main
continue

