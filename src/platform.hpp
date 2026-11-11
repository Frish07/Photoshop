#pragma once
#include <windows.h>
#include <utility>

template<class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    Com& operator=(Com&& other) noexcept {
        if (this != &other) { reset(); ptr_ = std::exchange(other.ptr_, nullptr); }
        return *this;
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T** put() { reset(); return &ptr_; }
    void reset() { if (ptr_) std::exchange(ptr_, nullptr)->Release(); }
private:
    T* ptr_{};
};
