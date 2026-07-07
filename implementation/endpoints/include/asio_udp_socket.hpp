// Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_V3_ASIO_UDP_SOCKET_HPP_
#define VSOMEIP_V3_ASIO_UDP_SOCKET_HPP_

#include "udp_socket.hpp"

#include <boost/asio/ip/udp.hpp>

#include <memory>

namespace vsomeip_v3 {

class asio_udp_socket final : public udp_socket {
public:
    explicit asio_udp_socket(boost::asio::io_context& _io) : socket_(std::make_shared<boost::asio::ip::udp::socket>(_io)) { }

    [[nodiscard]] bool is_open() const override { return socket_->is_open(); }
    void open(boost::asio::ip::udp::endpoint::protocol_type _pt, boost::system::error_code& _ec) override { socket_->open(_pt, _ec); }
    void bind(const endpoint_type& _ep, boost::system::error_code& _ec) override { socket_->bind(_ep, _ec); }
    void close(boost::system::error_code& _ec) override { socket_->close(_ec); }
    void cancel(boost::system::error_code& _ec) override { socket_->cancel(_ec); }

    endpoint_type local_endpoint(boost::system::error_code& _ec) const override { return socket_->local_endpoint(_ec); }

    void set_option(boost::asio::socket_base::reuse_address _opt, boost::system::error_code& _ec) override {
        socket_->set_option(_opt, _ec);
    }
    void set_option(boost::asio::socket_base::broadcast _opt, boost::system::error_code& _ec) override { socket_->set_option(_opt, _ec); }

    void async_send(const std::vector<boost::asio::const_buffer>& _buffers, rw_handler _handler) override {
        auto socket = socket_;
        socket->async_send(_buffers, [f = std::move(_handler), socket](auto const& _ec, size_t _bytes) { f(_ec, _bytes); });
    }
    void async_send_to(const std::vector<boost::asio::const_buffer>& _buffers, const endpoint_type& _destination,
                       rw_handler _handler) override {
        auto socket = socket_;
        socket->async_send_to(_buffers, _destination, [f = std::move(_handler), socket](auto const& _ec, size_t _bytes) { f(_ec, _bytes); });
    }
    void async_receive_from(boost::asio::mutable_buffer _buffer, endpoint_type& _sender, rw_handler _handler) override {
        auto socket = socket_;
        socket->async_receive_from(_buffer, _sender, [f = std::move(_handler), socket](auto const& _ec, size_t _bytes) { f(_ec, _bytes); });
    }

    boost::asio::ip::udp::socket& native() override { return *socket_; }

private:
    std::shared_ptr<boost::asio::ip::udp::socket> socket_;
};

} // namespace vsomeip_v3

#endif
