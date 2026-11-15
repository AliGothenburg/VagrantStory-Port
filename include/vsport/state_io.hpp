#pragma once

// Save state serialization: one function per class both writes and reads its state (StateIO
// carries the direction), so the two can never get out of step.

#include "vsport/types.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

namespace vsport {

class StateIO {
public:
    // Writer
    StateIO() : saving_(true) {}
    // Reader over a saved buffer
    explicit StateIO(std::vector<u8> data) : saving_(false), buf_(std::move(data)) {}

    [[nodiscard]] bool saving() const noexcept { return saving_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::vector<u8>& buffer() noexcept { return buf_; }

    void raw(void* p, std::size_t n) {
        if (saving_) {
            const auto* b = static_cast<const u8*>(p);
            buf_.insert(buf_.end(), b, b + n);
        } else if (pos_ + n <= buf_.size()) {
            std::memcpy(p, buf_.data() + pos_, n);
            pos_ += n;
        } else {
            ok_ = false;
            std::memset(p, 0, n);
        }
    }

    template <typename T>
    void pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
        raw(&v, sizeof(T));
    }

    template <typename T>
    void vec(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        u64 n = v.size();
        pod(n);
        if (!saving_) {
            if (n * sizeof(T) > buf_.size() - std::min(pos_, buf_.size())) { ok_ = false; return; }
            v.resize(static_cast<std::size_t>(n));
        }
        if (n) raw(v.data(), static_cast<std::size_t>(n) * sizeof(T));
    }

    template <typename T>
    void deq(std::deque<T>& d) {
        std::vector<T> tmp(d.begin(), d.end());
        vec(tmp);
        if (!saving_) d.assign(tmp.begin(), tmp.end());
    }

    void str(std::string& s) {
        std::vector<char> tmp(s.begin(), s.end());
        vec(tmp);
        if (!saving_) s.assign(tmp.begin(), tmp.end());
    }

    // Section marker: catches a reader and writer that got out of step (or a damaged file)
    void tag(u32 id) {
        u32 v = id;
        pod(v);
        if (!saving_ && v != id) ok_ = false;
    }

private:
    bool saving_;
    bool ok_ = true;
    std::vector<u8> buf_;
    std::size_t pos_ = 0;
};

} // namespace vsport
