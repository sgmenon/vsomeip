// Copyright (C) 2014-2021 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_V3_BUFFER_HPP_
#define VSOMEIP_V3_BUFFER_HPP_

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <numeric>
#include <set>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <vsomeip/defines.hpp>
#include <vsomeip/payload.hpp>
#include <vsomeip/primitive_types.hpp>

#if defined(_WIN32) && !defined(_MSVC_LANG)
#define DEFAULT_NANOSECONDS_MAX 1000000000
#else
#define DEFAULT_NANOSECONDS_MAX std::chrono::nanoseconds::max()
#endif

namespace vsomeip_v3 {

using message_buffer_t = std::vector<byte_t>;
using message_buffer_ptr_t = std::shared_ptr<message_buffer_t>;

/**
 * Latch for optional send/notify completion handlers.
 * Use begin()/add_pending()/end() around synchronous queueing, then each
 * async write completion calls complete().
 */
struct send_completion_state {
    explicit send_completion_state(std::function<void(bool)> _handler) : handler_(std::move(_handler)) { }

    void begin() { remaining_.fetch_add(1, std::memory_order_acq_rel); }

    void add_pending(int _n = 1) {
        if (_n > 0) {
            remaining_.fetch_add(_n, std::memory_order_acq_rel);
        }
    }

    void end() { complete(true); }

    void complete(bool _success) {
        if (!_success) {
            ok_.store(false, std::memory_order_relaxed);
        }
        const int prev = remaining_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            bool expected = false;
            if (fired_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                if (handler_) {
                    handler_(ok_.load(std::memory_order_relaxed));
                }
            }
        }
    }

private:
    std::function<void(bool)> handler_;
    std::atomic<int> remaining_{0};
    std::atomic<bool> ok_{true};
    std::atomic<bool> fired_{false};
};

typedef std::shared_ptr<send_completion_state> send_completion_state_ptr_t;

/**
 * Shared buffer + explicit byte range. One type for routing receive ingress
 * and for pieces of a buffer_sequence.
 *
 * length is always explicit (never "0 means whole buffer"). Use whole() /
 * is_whole() when the slice covers the entire buffer.
 */
struct owned_buffer_slice {
    message_buffer_ptr_t buffer;
    std::size_t offset{0};
    std::size_t length{0};

    const byte_t* data() const {
        if (!buffer) {
            return nullptr;
        }
        return buffer->data() + offset;
    }

    std::size_t size() const { return valid() ? length : 0; }

    bool valid() const { return buffer && offset + length <= buffer->size(); }

    bool empty() const { return size() == 0; }

    bool is_whole() const { return buffer && offset == 0 && length == buffer->size(); }

    // Copies the shared_ptr so callers can pin multiple slices of one datagram.
    static owned_buffer_slice whole(const message_buffer_ptr_t& _buffer) {
        owned_buffer_slice its_slice;
        if (_buffer) {
            its_slice.buffer = _buffer;
            its_slice.offset = 0;
            its_slice.length = _buffer->size();
        }
        return its_slice;
    }

    // Copies the shared_ptr (safe for multi-message datagrams).
    static owned_buffer_slice slice(const message_buffer_ptr_t& _buffer, std::size_t _offset, std::size_t _length) {
        owned_buffer_slice its_slice;
        if (_buffer && _offset + _length <= _buffer->size()) {
            its_slice.buffer = _buffer;
            its_slice.offset = _offset;
            its_slice.length = _length;
        }
        return its_slice;
    }

    // Edge adapter when the caller only has a temporary pointer.
    static owned_buffer_slice copy_of(const byte_t* _data, std::size_t _size) {
        if (!_data || _size == 0) {
            return owned_buffer_slice{};
        }
        return whole(std::make_shared<message_buffer_t>(_data, _data + _size));
    }

    // Prefer a pin covering [_data, _data+_size); otherwise copy_of.
    static owned_buffer_slice from_pointer(const byte_t* _data, std::size_t _size, const message_buffer_ptr_t& _pin) {
        if (!_data || _size == 0) {
            return owned_buffer_slice{};
        }
        if (_pin && _data >= _pin->data() && (_data + _size) <= (_pin->data() + _pin->size())) {
            return slice(_pin, static_cast<std::size_t>(_data - _pin->data()), _size);
        }
        return copy_of(_data, _size);
    }
};

/**
 * Multi-buffer unit for asio ConstBufferSequence I/O (send and local forward).
 * Holds shared_ptrs to segment storage until the async completion handler runs.
 *
 * - append / prepend / append_bytes: exclusive buffers (headers, meta, copies).
 *   Asserts use_count == 1 — callers must std::move the only remaining ref.
 * - append_buffer_slice / append(owned_buffer_slice): shared pins (recv buffer,
 *   payload backing store).
 */
struct buffer_sequence {
    buffer_sequence() = default;

    explicit buffer_sequence(message_buffer_ptr_t _buffer) {
        if (_buffer && !_buffer->empty()) {
            append(std::move(_buffer));
        }
    }

    buffer_sequence(const byte_t* _data, std::size_t _size) {
        if (_data && _size > 0) {
            append_bytes(_data, _size);
        }
    }

    void append(message_buffer_ptr_t _buffer) {
        if (!_buffer || _buffer->empty()) {
            return;
        }
        // Exclusive: this use case is for the only remaining shared_ptr to these bytes.
        assert(_buffer.use_count() == 1);
        segments_.push_back(owned_buffer_slice::whole(std::move(_buffer)));
        rebuild_buffers();
    }

    void append(owned_buffer_slice _slice) {
        if (_slice.empty()) {
            return;
        }
        segments_.push_back(std::move(_slice));
        rebuild_buffers();
    }

    /**
     * Pin a sub-range of a shared buffer (0-copy). Keeps `_buffer` alive for
     * the lifetime of this sequence. May share with other slices / payloads.
     */
    void append_buffer_slice(message_buffer_ptr_t _buffer, std::size_t _offset, std::size_t _length) {
        append(owned_buffer_slice::slice(std::move(_buffer), _offset, _length));
    }

    void append_bytes(const byte_t* _data, std::size_t _size) {
        if (!_data || _size == 0) {
            return;
        }
        append(std::make_shared<message_buffer_t>(_data, _data + _size));
    }

    void prepend(message_buffer_ptr_t _buffer) {
        if (!_buffer || _buffer->empty()) {
            return;
        }
        assert(_buffer.use_count() == 1);
        segments_.insert(segments_.begin(), owned_buffer_slice::whole(std::move(_buffer)));
        rebuild_buffers();
    }

    void append_sequence(const buffer_sequence& _other) {
        segments_.insert(segments_.end(), _other.segments_.begin(), _other.segments_.end());
        completions_.insert(completions_.end(), _other.completions_.begin(), _other.completions_.end());
        rebuild_buffers();
    }

    void attach_completion(send_completion_state_ptr_t _completion) {
        if (_completion) {
            completions_.push_back(std::move(_completion));
        }
    }

    /**
     * Notify all attached completion latches (e.g. from endpoint send_cbk).
     * May be called once per async write of this sequence (fan-out may share
     * one sequence across targets; the latch remaining count absorbs that).
     */
    void complete(bool _success) {
        for (auto& its_completion : completions_) {
            if (its_completion) {
                its_completion->complete(_success);
            }
        }
    }

    void clear_completions() { completions_.clear(); }

    bool has_completion() const { return !completions_.empty(); }

    std::size_t size() const {
        return std::accumulate(segments_.begin(), segments_.end(), std::size_t{0},
                               [](std::size_t sum, const owned_buffer_slice& s) { return sum + s.size(); });
    }

    bool empty() const { return size() == 0; }

    const std::vector<boost::asio::const_buffer>& buffers() const { return buffers_; }

    const std::vector<owned_buffer_slice>& segments() const { return segments_; }

    /**
     * Read a big-endian uint16 from a logical offset across the sequence.
     * Returns false if the sequence is shorter than offset+2.
     */
    bool read_uint16_be(std::size_t _offset, uint16_t& _out) const {
        byte_t high = 0;
        byte_t low = 0;
        if (!read_byte(_offset, high) || !read_byte(_offset + 1, low)) {
            return false;
        }
        _out = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8) | low);
        return true;
    }

    bool read_byte(std::size_t _offset, byte_t& _out) const {
        std::size_t remaining = _offset;
        for (const auto& its_segment : segments_) {
            const std::size_t its_size = its_segment.size();
            if (remaining < its_size) {
                _out = its_segment.data()[remaining];
                return true;
            }
            remaining -= its_size;
        }
        return false;
    }

    /**
     * Ensure the sequence is a single contiguous buffer (copies only when needed).
     * This is a compatibility fallback for legacy pointer-arithmetic paths
     * (e.g. SOME/IP-TP split / trace callbacks). Normal send uses buffers().
     */
    message_buffer_ptr_t flatten() const {
        if (segments_.size() == 1 && segments_.front().is_whole()) {
            return segments_.front().buffer;
        }
        auto flat = std::make_shared<message_buffer_t>();
        flat->reserve(size());
        for (const auto& its_segment : segments_) {
            const byte_t* its_data = its_segment.data();
            const auto its_length = its_segment.size();
            if (its_data && its_length > 0) {
                flat->insert(flat->end(), its_data, its_data + its_length);
            }
        }
        return flat;
    }

private:
    void rebuild_buffers() {
        buffers_.clear();
        buffers_.reserve(segments_.size());
        for (const auto& its_segment : segments_) {
            const byte_t* its_data = its_segment.data();
            const auto its_length = its_segment.size();
            if (its_data && its_length > 0) {
                buffers_.emplace_back(boost::asio::buffer(its_data, its_length));
            }
        }
    }

    std::vector<owned_buffer_slice> segments_;
    std::vector<boost::asio::const_buffer> buffers_;
    std::vector<send_completion_state_ptr_t> completions_;
};

typedef std::shared_ptr<buffer_sequence> buffer_sequence_ptr_t;

struct train {
    train() :
        sequence_(std::make_shared<buffer_sequence>()), minimal_debounce_time_(DEFAULT_NANOSECONDS_MAX),
        minimal_max_retention_time_(DEFAULT_NANOSECONDS_MAX), departure_(std::chrono::steady_clock::now() + std::chrono::hours(6)) { }

    void reset() {
        sequence_ = std::make_shared<buffer_sequence>();
        passengers_.clear();
        minimal_debounce_time_ = DEFAULT_NANOSECONDS_MAX;
        minimal_max_retention_time_ = DEFAULT_NANOSECONDS_MAX;
        departure_ = std::chrono::steady_clock::now() + std::chrono::hours(6);
    }

    buffer_sequence_ptr_t sequence_;
    std::set<std::pair<service_t, method_t>> passengers_;

    std::chrono::nanoseconds minimal_debounce_time_;
    std::chrono::nanoseconds minimal_max_retention_time_;

    std::chrono::steady_clock::time_point departure_;
};

} // namespace vsomeip_v3

#endif // VSOMEIP_V3_BUFFER_HPP_
