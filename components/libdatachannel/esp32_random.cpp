// ESP32 random number generation compatibility
#include <cstdlib>
#include <esp_random.h>
#include <esp_log.h>
#include <fcntl.h>
#include <unistd.h>

static const char* TAG = "esp32_random";

// Emulate /dev/urandom using ESP32's hardware RNG
static int urandom_fd = -1;

// Override open() calls to /dev/urandom  
extern "C" int __real_open(const char *pathname, int flags, ...);
extern "C" int __wrap_open(const char *pathname, int flags, ...) {
    if (pathname && strcmp(pathname, "/dev/urandom") == 0) {
        ESP_LOGD(TAG, "Intercepted /dev/urandom open");
        // Return a fake file descriptor
        urandom_fd = 42; // Arbitrary non-zero value
        return urandom_fd;
    }
    
    // Pass through to real open
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        va_end(args);
        return __real_open(pathname, flags, mode);
    }
    
    return __real_open(pathname, flags);
}

// Override read() calls from our fake urandom fd
extern "C" ssize_t __real_read(int fd, void *buf, size_t count);
extern "C" ssize_t __wrap_read(int fd, void *buf, size_t count) {
    if (fd == urandom_fd) {
        ESP_LOGD(TAG, "Generating %zu random bytes", count);
        
        // Fill buffer with ESP32 hardware random numbers
        uint8_t *byte_buf = (uint8_t*)buf;
        size_t remaining = count;
        
        while (remaining >= 4) {
            uint32_t rand_val = esp_random();
            memcpy(byte_buf, &rand_val, 4);
            byte_buf += 4;
            remaining -= 4;
        }
        
        // Handle remaining bytes
        if (remaining > 0) {
            uint32_t rand_val = esp_random();
            memcpy(byte_buf, &rand_val, remaining);
        }
        
        return count;
    }
    
    // Pass through to real read
    return __real_read(fd, buf, count);
}

// Override close() for our fake fd
extern "C" int __real_close(int fd);
extern "C" int __wrap_close(int fd) {
    if (fd == urandom_fd) {
        ESP_LOGD(TAG, "Closed /dev/urandom fd");
        urandom_fd = -1;
        return 0;
    }
    
    return __real_close(fd);
}

// Direct random number functions that might be used
extern "C" void esp32_get_random_bytes(void *buf, size_t len) {
    uint8_t *byte_buf = (uint8_t*)buf;
    size_t remaining = len;
    
    while (remaining >= 4) {
        uint32_t rand_val = esp_random();
        memcpy(byte_buf, &rand_val, 4);
        byte_buf += 4;
        remaining -= 4;
    }
    
    if (remaining > 0) {
        uint32_t rand_val = esp_random();
        memcpy(byte_buf, &rand_val, remaining);
    }
}

// getrandom() system call compatibility (if used by libraries)
extern "C" ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags; // Ignore flags for now
    esp32_get_random_bytes(buf, buflen);
    return buflen;
}