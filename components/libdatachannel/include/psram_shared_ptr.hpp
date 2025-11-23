// Simplified shared_ptr implementation for ESP32 PSRAM allocation
// Minimal implementation - no weak_ptr support needed
#ifndef PSRAM_SHARED_PTR_HPP
#define PSRAM_SHARED_PTR_HPP

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include "esp_log.h"
#include "esp_heap_caps.h"

namespace psram {

static const char* TAG = "psram_shared_ptr";

// Disable verbose logging for performance
#define PSRAM_SHARED_PTR_VERBOSE 0

// Base class for reference counted control blocks
class _Sp_counted_base {
public:
    _Sp_counted_base() noexcept : _M_use_count(1) {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_base() constructor at %p", this);
#endif
    }

    virtual ~_Sp_counted_base() noexcept {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_base() destructor at %p", this);
#endif
    }

    virtual void _M_dispose() noexcept = 0;  // Destroy the managed object
    virtual void _M_destroy() noexcept {      // Destroy the control block
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_base::_M_destroy() at %p", this);
#endif
        delete this;
    }

    void _M_add_ref_copy() noexcept {
        // ESP32-P4 supports atomic operations on PSRAM with natural alignment (4-byte for int32)
        // Use GCC built-in atomic operations for thread-safe reference counting
        __atomic_fetch_add(&_M_use_count, 1, __ATOMIC_ACQ_REL);
    }

    void _M_release() noexcept {
        // Atomic decrement and test
        if (__atomic_fetch_sub(&_M_use_count, 1, __ATOMIC_ACQ_REL) == 1) {
            _M_dispose();
            _M_destroy();
        }
    }

    long _M_get_use_count() const noexcept {
        // Atomic read
        return __atomic_load_n(&_M_use_count, __ATOMIC_ACQUIRE);
    }

private:
    // ESP32-P4 RISC-V requires 4-byte alignment for 32-bit atomic operations
    // long is 4 bytes on RISC-V32, so it's naturally aligned in the structure
    alignas(4) long _M_use_count;
};

// Control block for raw pointers (stores pointer, not object inline)
template<typename _Tp>
class _Sp_counted_ptr final : public _Sp_counted_base {
public:
    explicit _Sp_counted_ptr(_Tp* __p) noexcept : _Sp_counted_base(), _M_ptr(__p) {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_ptr() constructor at %p for ptr %p", this, __p);
#endif
    }

    ~_Sp_counted_ptr() noexcept override {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_ptr() destructor at %p", this);
#endif
    }

    void _M_dispose() noexcept override {
        delete _M_ptr;
    }

private:
    _Tp* _M_ptr;
};

// Control block for make_shared (stores object inline with control block)
template<typename _Tp, typename _Alloc>
class _Sp_counted_ptr_inplace final : public _Sp_counted_base {
    using _Impl = std::remove_cv_t<_Tp>;
    using _Alloc_traits = std::allocator_traits<_Alloc>;

public:
    using __allocator_type = typename _Alloc_traits::template rebind_alloc<_Sp_counted_ptr_inplace>;

    template<typename... _Args>
    _Sp_counted_ptr_inplace(const __allocator_type& __a, _Args&&... __args)
    : _Sp_counted_base(), _M_alloc(__a) {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_ptr_inplace() constructor at %p", this);
#endif
        ::new ((void*)_M_ptr()) _Impl(std::forward<_Args>(__args)...);
    }

    ~_Sp_counted_ptr_inplace() noexcept override {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "  _Sp_counted_ptr_inplace() destructor at %p", this);
#endif
    }

    void _M_dispose() noexcept override {
        _M_ptr()->~_Impl();
    }

    void _M_destroy() noexcept override {
        __allocator_type __alloc(_M_alloc);
        this->~_Sp_counted_ptr_inplace();
        __alloc.deallocate(this, 1);
    }

    void* _M_get_deleter(const std::type_info&) noexcept {
        return nullptr;
    }

    // Allow psram_shared_count to access _M_ptr() to get the object pointer
    friend class psram_shared_count;

    _Impl* _M_ptr() noexcept {
        return reinterpret_cast<_Impl*>(&_M_storage);
    }

private:
    __allocator_type _M_alloc;
    alignas(alignof(_Tp)) std::byte _M_storage[sizeof(_Tp)];
};

// Simple shared count wrapper
class psram_shared_count {
public:
    constexpr psram_shared_count() noexcept : _M_pi(nullptr) {}

    // Constructor from pre-existing control block (for raw pointer wrapping)
    explicit psram_shared_count(_Sp_counted_base* __p) noexcept : _M_pi(__p) {}

    template<typename _Tp, typename _Alloc, typename... _Args>
    psram_shared_count(_Tp*& __p, const _Alloc& __a, _Args&&... __args) {
        using _Sp_cp_type = _Sp_counted_ptr_inplace<_Tp, _Alloc>;
        typename _Sp_cp_type::__allocator_type __alloc(__a);

        _Sp_cp_type* __mem = __alloc.allocate(1);

        try {
            // Construct the control block (which also constructs the object inside it)
            ::new ((void*)__mem) _Sp_cp_type(__alloc, std::forward<_Args>(__args)...);
            _M_pi = __mem;
            // Get the pointer to the object stored inside the control block
            // This is the correct way, not __mem + 1 which would point past allocated memory!
            __p = __mem->_M_ptr();
        } catch(...) {
            __alloc.deallocate(__mem, 1);
            __p = nullptr;
            throw;
        }
    }

    ~psram_shared_count() noexcept {
        if (_M_pi != nullptr)
            _M_pi->_M_release();
    }

    psram_shared_count(const psram_shared_count& __r) noexcept
    : _M_pi(__r._M_pi) {
        if (_M_pi != nullptr)
            _M_pi->_M_add_ref_copy();
    }

    psram_shared_count& operator=(const psram_shared_count& __r) noexcept {
        _Sp_counted_base* __tmp = __r._M_pi;
        if (__tmp != _M_pi) {
            if (__tmp != nullptr)
                __tmp->_M_add_ref_copy();
            if (_M_pi != nullptr)
                _M_pi->_M_release();
            _M_pi = __tmp;
        }
        return *this;
    }

    void _M_swap(psram_shared_count& __r) noexcept {
        _Sp_counted_base* __tmp = __r._M_pi;
        __r._M_pi = _M_pi;
        _M_pi = __tmp;
    }

    long _M_get_use_count() const noexcept {
        return _M_pi != nullptr ? _M_pi->_M_get_use_count() : 0;
    }

    friend bool operator==(const psram_shared_count& __a, const psram_shared_count& __b) noexcept {
        return __a._M_pi == __b._M_pi;
    }

private:
    _Sp_counted_base* _M_pi;
};

// The actual shared pointer class
template<typename _Tp>
class psram_shared_ptr {
public:
    using element_type = _Tp;

    constexpr psram_shared_ptr() noexcept : _M_ptr(nullptr), _M_refcount() {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "CREATED (default) ptr=nullptr");
#endif
    }

    constexpr psram_shared_ptr(std::nullptr_t) noexcept : _M_ptr(nullptr), _M_refcount() {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "CREATED (nullptr) ptr=nullptr");
#endif
    }

    // Constructor from raw pointer - takes ownership
    // Used for wrapping externally allocated objects (e.g., from unique_ptr)
    template<typename _Yp, typename = std::enable_if_t<std::is_convertible_v<_Yp*, _Tp*>>>
    explicit psram_shared_ptr(_Yp* __p) : _M_ptr(__p), _M_refcount() {
        if (__p) {
            try {
                // Allocate control block in PSRAM directly
                void* __mem = heap_caps_malloc(sizeof(_Sp_counted_ptr<_Yp>), MALLOC_CAP_SPIRAM);
                if (!__mem)
                    throw std::bad_alloc();
                ::new (__mem) _Sp_counted_ptr<_Yp>(__p);
                _M_refcount = psram_shared_count(static_cast<_Sp_counted_ptr<_Yp>*>(__mem));
            } catch(...) {
                delete __p;
                throw;
            }
        }
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "CREATED (raw ptr) ptr=%p", (void*)__p);
#endif
    }

    ~psram_shared_ptr() noexcept {
        // Refcount destructor handles cleanup
    }

    psram_shared_ptr(const psram_shared_ptr& __r) noexcept
    : _M_ptr(__r._M_ptr), _M_refcount(__r._M_refcount) {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "COPY ptr=%p (refcount incremented)", (void*)_M_ptr);
#endif
    }

    psram_shared_ptr(psram_shared_ptr&& __r) noexcept
    : _M_ptr(__r._M_ptr), _M_refcount() {
        _M_refcount._M_swap(__r._M_refcount);
        __r._M_ptr = nullptr;
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "MOVE ptr=%p (refcount unchanged)", (void*)_M_ptr);
#endif
    }

    psram_shared_ptr& operator=(const psram_shared_ptr& __r) noexcept {
        _M_ptr = __r._M_ptr;
        _M_refcount = __r._M_refcount;
        return *this;
    }

    psram_shared_ptr& operator=(psram_shared_ptr&& __r) noexcept {
        psram_shared_ptr(std::move(__r)).swap(*this);
        return *this;
    }

    element_type& operator*() const noexcept {
        return *_M_ptr;
    }

    element_type* operator->() const noexcept {
        return _M_ptr;
    }

    element_type* get() const noexcept {
        return _M_ptr;
    }

    long use_count() const noexcept {
        return _M_refcount._M_get_use_count();
    }

    void reset() noexcept {
        psram_shared_ptr().swap(*this);
    }

    void swap(psram_shared_ptr& __other) noexcept {
        std::swap(_M_ptr, __other._M_ptr);
        _M_refcount._M_swap(__other._M_refcount);
    }

    explicit operator bool() const noexcept {
        return _M_ptr != nullptr;
    }

    bool operator==(std::nullptr_t) const noexcept {
        return _M_ptr == nullptr;
    }

    bool operator!=(std::nullptr_t) const noexcept {
        return _M_ptr != nullptr;
    }

private:
    template<typename _Tp1, typename _Alloc, typename... _Args>
    friend psram_shared_ptr<_Tp1> allocate_psram_shared(const _Alloc& __a, _Args&&... __args);

    template<typename _Alloc, typename... _Args>
    psram_shared_ptr(const _Alloc& __a, _Args&&... __args)
    : _M_ptr(nullptr), _M_refcount() {
        _M_refcount = psram_shared_count(_M_ptr, __a, std::forward<_Args>(__args)...);
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "psram_shared_ptr(alloc, args...) CREATED ptr=%p", (void*)_M_ptr);
#endif
    }

    element_type* _M_ptr;
    psram_shared_count _M_refcount;
};

// Non-member operator! for compatibility
template<typename _Tp>
inline bool operator!(const psram_shared_ptr<_Tp>& __p) noexcept {
    return __p.get() == nullptr;
}

// Allocator that uses ESP32 PSRAM
template<typename _Tp>
class PSRAMAllocator {
public:
    using value_type = _Tp;

    PSRAMAllocator() noexcept = default;
    template<typename _Up> PSRAMAllocator(const PSRAMAllocator<_Up>&) noexcept {}

    _Tp* allocate(std::size_t __n) {
        size_t bytes = __n * sizeof(_Tp);
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "PSRAMAllocator::allocate(%zu bytes) = %p", bytes, p);
#endif
        if (!p)
            throw std::bad_alloc();
        return static_cast<_Tp*>(p);
    }

    void deallocate(_Tp* __p, std::size_t) noexcept {
#if PSRAM_SHARED_PTR_VERBOSE
        ESP_LOGI(TAG, "PSRAMAllocator::deallocate(%p)", (void*)__p);
#endif
        heap_caps_free(__p);
    }
};

template<typename _Tp, typename _Up>
inline bool operator==(const PSRAMAllocator<_Tp>&, const PSRAMAllocator<_Up>&) noexcept {
    return true;
}

template<typename _Tp, typename _Up>
inline bool operator!=(const PSRAMAllocator<_Tp>&, const PSRAMAllocator<_Up>&) noexcept {
    return false;
}

// Helper function to create PSRAM-allocated shared pointers
template<typename _Tp, typename _Alloc, typename... _Args>
inline psram_shared_ptr<_Tp> allocate_psram_shared(const _Alloc& __a, _Args&&... __args) {
    return psram_shared_ptr<_Tp>(__a, std::forward<_Args>(__args)...);
}

template<typename _Tp, typename... _Args>
inline psram_shared_ptr<_Tp> make_psram_shared(_Args&&... __args) {
    using _Alloc = PSRAMAllocator<_Tp>;
    _Alloc __a;
    return allocate_psram_shared<_Tp>(__a, std::forward<_Args>(__args)...);
}

} // namespace psram

#endif // PSRAM_SHARED_PTR_HPP
