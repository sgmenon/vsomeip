// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/io_context.hpp>

#include <vsomeip/constants.hpp>
#include <vsomeip/primitive_types.hpp>

#include "../../../implementation/configuration/include/configuration_impl.hpp"
#include "../../../implementation/endpoints/include/client_endpoint.hpp"
#include "../../../implementation/message/include/serializer.hpp"
#include "../../../implementation/routing/include/eventgroupinfo.hpp"
#include "../../../implementation/service_discovery/include/runtime.hpp"
#include "../../../implementation/service_discovery/include/deserializer.hpp"
#include "../../../implementation/service_discovery/include/eventgroupentry_impl.hpp"
#include "../../../implementation/service_discovery/include/ipv4_option_impl.hpp"
#include "../../../implementation/service_discovery/include/message_impl.hpp"
#include "../../../implementation/service_discovery/include/service_discovery_host.hpp"
#include "../../../implementation/service_discovery/include/service_discovery_impl.hpp"
#include "../../../implementation/service_discovery/include/serviceentry_impl.hpp"

namespace {

constexpr vsomeip_v3::service_t k_service = 0x1234;
constexpr vsomeip_v3::instance_t k_instance = 0x0001;
constexpr vsomeip_v3::eventgroup_t k_eventgroup_a = 0x8001;
constexpr vsomeip_v3::eventgroup_t k_eventgroup_b = 0x8003;
constexpr std::uint16_t k_port_a = 50046;
constexpr std::uint16_t k_port_b = 50047;
constexpr std::uint16_t k_shared_port = 40000;
constexpr vsomeip_v3::major_version_t k_major = 1;
constexpr vsomeip_v3::minor_version_t k_minor = 0;
constexpr vsomeip_v3::ttl_t k_ttl = 3;
constexpr vsomeip_v3::client_t k_client = 0x1111;

class fake_client_endpoint : public vsomeip_v3::client_endpoint {
public:
    fake_client_endpoint(std::uint16_t local_port, bool reliable, boost::asio::ip::address remote, bool established) :
        local_port_(local_port), reliable_(reliable), remote_(std::move(remote)), established_(established) { }

    void start() override { }
    void restart(bool) override { }
    void stop(bool) override { }
    void prepare_stop(const prepare_stop_handler_t&, vsomeip_v3::service_t) override { }
    bool is_established() const override { return established_; }
    bool is_established_or_connected() const override { return established_; }
    bool send(const vsomeip_v3::buffer_sequence_ptr_t&) override { return false; }
    bool send_to(const std::shared_ptr<vsomeip_v3::endpoint_definition>, const vsomeip_v3::buffer_sequence_ptr_t&) override { return false; }
    bool send_error(const std::shared_ptr<vsomeip_v3::endpoint_definition>, const vsomeip_v3::byte_t*, uint32_t) override {
        return false;
    }
    void receive() override { }
    void add_default_target(vsomeip_v3::service_t, const std::string&, std::uint16_t) override { }
    void remove_default_target(vsomeip_v3::service_t) override { }
    void remove_stop_handler(vsomeip_v3::service_t) override { }
    std::uint16_t get_local_port() const override { return local_port_; }
    bool is_reliable() const override { return reliable_; }
    bool is_local() const override { return false; }
    void register_error_handler(const error_handler_t&) override { }
    void print_status() override { }
    size_t get_queue_size() const override { return 0; }
    void set_established(bool established) override { established_ = established; }
    void set_connected(bool) override { }
    bool get_remote_address(boost::asio::ip::address& address) const override {
        address = remote_;
        return !remote_.is_unspecified();
    }
    std::uint16_t get_remote_port() const override { return 30509; }

private:
    std::uint16_t local_port_;
    bool reliable_;
    boost::asio::ip::address remote_;
    bool established_;
};

struct endpoint_lookup {
    vsomeip_v3::service_t service;
    vsomeip_v3::instance_t instance;
    vsomeip_v3::eventgroup_t eventgroup;
    bool reliable;
    bool saw_eventgroup;
};

struct subscribe_option {
    vsomeip_v3::eventgroup_t eventgroup;
    std::uint16_t port;
};

class recording_host : public vsomeip_v3::sd::service_discovery_host {
public:
    explicit recording_host(boost::asio::io_context& io) : remote_(boost::asio::ip::make_address("10.1.2.3")), io_(io) { }

    boost::asio::io_context& get_io() override { return io_; }

    std::shared_ptr<vsomeip_v3::endpoint> create_service_discovery_endpoint(const std::string&, std::uint16_t, bool) override {
        return nullptr;
    }

    vsomeip_v3::services_t get_offered_services() const override { return {}; }
    std::shared_ptr<vsomeip_v3::serviceinfo> get_offered_service(vsomeip_v3::service_t, vsomeip_v3::instance_t) const override {
        return nullptr;
    }
    std::map<vsomeip_v3::instance_t, std::shared_ptr<vsomeip_v3::serviceinfo>>
    get_offered_service_instances(vsomeip_v3::service_t) const override {
        return {};
    }

    std::shared_ptr<vsomeip_v3::eventgroupinfo> find_eventgroup(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance,
                                                               vsomeip_v3::eventgroup_t eventgroup) const override {
        auto found = eventgroups_.find({service, instance, eventgroup});
        return found == eventgroups_.end() ? nullptr : found->second;
    }

    bool send(vsomeip_v3::client_t, std::shared_ptr<vsomeip_v3::message>, bool) override { return true; }

    bool send_via_sd(const std::shared_ptr<vsomeip_v3::endpoint_definition>&, vsomeip_v3::message_buffer_ptr_t frame,
                     std::uint16_t) override {
        if (frame) {
            sent_frames_.push_back(*frame);
        }
        return true;
    }

    void add_routing_info(vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::major_version_t, vsomeip_v3::minor_version_t,
                          vsomeip_v3::ttl_t, const boost::asio::ip::address&, std::uint16_t, const boost::asio::ip::address&, std::uint16_t,
                          bool, bool) override { }

    void del_routing_info(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance, bool, bool, bool) override {
        ++del_routing_info_count_;
        for (auto it = endpoints_.begin(); it != endpoints_.end();) {
            if (std::get<0>(it->first) == service && std::get<1>(it->first) == instance) {
                it = endpoints_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void update_routing_info(std::chrono::milliseconds) override { }

    void is_remote_service_known(vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::major_version_t, vsomeip_v3::minor_version_t,
                                 const boost::asio::ip::address&, std::uint16_t, bool& reliable_known, const boost::asio::ip::address&,
                                 std::uint16_t, bool& unreliable_known, bool& drop_offer) override {
        reliable_known = false;
        unreliable_known = true;
        drop_offer = false;
    }

    void on_remote_subscribe(std::shared_ptr<vsomeip_v3::remote_subscription>&, const vsomeip_v3::remote_subscription_callback_t&) override { }
    void on_remote_unsubscribe(std::shared_ptr<vsomeip_v3::remote_subscription>&) override { }
    void on_subscribe_ack(vsomeip_v3::client_t, vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::eventgroup_t, vsomeip_v3::event_t,
                          vsomeip_v3::remote_subscription_id_t) override { }
    void on_subscribe_ack_with_multicast(vsomeip_v3::service_t, vsomeip_v3::instance_t, const boost::asio::ip::address&,
                                         const boost::asio::ip::address&, std::uint16_t) override { }
    void on_subscribe_nack(vsomeip_v3::client_t, vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::eventgroup_t, bool,
                           vsomeip_v3::remote_subscription_id_t) override { }
    void expire_subscriptions(const boost::asio::ip::address&) override { }
    void expire_subscriptions(const boost::asio::ip::address&, std::uint16_t, bool) override { }
    void expire_services(const boost::asio::ip::address&) override { }
    void expire_services(const boost::asio::ip::address&, std::uint16_t, bool) override { }
    std::chrono::steady_clock::time_point expire_subscriptions(bool) override {
        return std::chrono::steady_clock::now() + std::chrono::hours(24);
    }

    std::set<vsomeip_v3::eventgroup_t> get_subscribed_eventgroups(vsomeip_v3::service_t, vsomeip_v3::instance_t) override { return {}; }

    void remember(const std::shared_ptr<vsomeip_v3::eventgroupinfo>& info) {
        eventgroups_[{info->get_service(), info->get_instance(), info->get_eventgroup()}] = info;
    }

    std::shared_ptr<vsomeip_v3::endpoint> endpoint_for(vsomeip_v3::eventgroup_t eventgroup, bool reliable) const {
        auto found = endpoints_.find({k_service, k_instance, eventgroup, reliable});
        return found == endpoints_.end() ? nullptr : found->second;
    }

    std::vector<vsomeip_v3::message_buffer_t> sent_frames_;
    std::vector<endpoint_lookup> lookups_;
    int del_routing_info_count_{0};
    bool create_established_{false};
    boost::asio::ip::address remote_;

protected:
    std::shared_ptr<vsomeip_v3::endpoint> cached_endpoint(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance,
                                                         vsomeip_v3::eventgroup_t eventgroup, bool reliable, std::uint16_t port,
                                                         bool saw_eventgroup) {
        lookups_.push_back({service, instance, eventgroup, reliable, saw_eventgroup});
        if (port == 0) {
            return nullptr;
        }
        const auto key = std::make_tuple(service, instance, eventgroup, reliable);
        auto found = endpoints_.find(key);
        if (found != endpoints_.end()) {
            return found->second;
        }
        auto endpoint = std::make_shared<fake_client_endpoint>(port, reliable, remote_, create_established_);
        endpoints_[key] = endpoint;
        return endpoint;
    }

    boost::asio::io_context& io_;
    std::map<std::tuple<vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::eventgroup_t>, std::shared_ptr<vsomeip_v3::eventgroupinfo>>
            eventgroups_;
    std::map<std::tuple<vsomeip_v3::service_t, vsomeip_v3::instance_t, vsomeip_v3::eventgroup_t, bool>, std::shared_ptr<vsomeip_v3::endpoint>>
            endpoints_;
};

class per_eventgroup_host : public recording_host {
public:
    using recording_host::recording_host;

    std::shared_ptr<vsomeip_v3::endpoint> find_or_create_remote_client(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance,
                                                                      bool reliable) override {
        return cached_endpoint(service, instance, vsomeip_v3::ANY_EVENTGROUP, reliable, reliable ? 0 : k_shared_port, false);
    }

    std::shared_ptr<vsomeip_v3::endpoint> find_or_create_remote_client(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance,
                                                                      vsomeip_v3::eventgroup_t eventgroup, bool reliable) override {
        std::uint16_t port = 0;
        if (!reliable) {
            if (eventgroup == k_eventgroup_a) {
                port = k_port_a;
            } else if (eventgroup == k_eventgroup_b) {
                port = k_port_b;
            }
        }
        return cached_endpoint(service, instance, eventgroup, reliable, port, true);
    }
};

// Overrides only the three-argument lookup. The eventgroup overload's default
// forwards here, which is the routing-manager behavior.
class legacy_host : public recording_host {
public:
    using recording_host::recording_host;

    std::shared_ptr<vsomeip_v3::endpoint> find_or_create_remote_client(vsomeip_v3::service_t service, vsomeip_v3::instance_t instance,
                                                                      bool reliable) override {
        return cached_endpoint(service, instance, vsomeip_v3::ANY_EVENTGROUP, reliable, reliable ? 0 : k_shared_port, false);
    }
};

std::vector<vsomeip_v3::byte_t> make_offer(vsomeip_v3::ttl_t ttl, vsomeip_v3::session_t session) {
    auto message = std::make_shared<vsomeip_v3::sd::message_impl>();
    message->set_reboot_flag(true);
    message->set_unicast_flag(true);
    message->set_session(session);

    auto entry = std::make_shared<vsomeip_v3::sd::serviceentry_impl>();
    entry->set_type(vsomeip_v3::sd::entry_type_e::OFFER_SERVICE);
    entry->set_service(k_service);
    entry->set_instance(k_instance);
    entry->set_major_version(k_major);
    entry->set_minor_version(k_minor);
    entry->set_ttl(ttl);

    const auto address = boost::asio::ip::make_address("10.1.2.3");
    std::vector<std::shared_ptr<vsomeip_v3::sd::option_impl>> options;
    options.push_back(std::make_shared<vsomeip_v3::sd::ipv4_option_impl>(address, 30509, false));
    EXPECT_TRUE(message->add_entry_data(entry, options));

    vsomeip_v3::serializer serializer(0);
    EXPECT_TRUE(serializer.serialize(message.get()));
    return {serializer.get_data(), serializer.get_data() + serializer.get_size()};
}

std::map<vsomeip_v3::eventgroup_t, std::uint16_t> subscribe_ports(const vsomeip_v3::message_buffer_t& frame) {
    std::map<vsomeip_v3::eventgroup_t, std::uint16_t> ports;
    vsomeip_v3::sd::deserializer deserializer(0);
    deserializer.set_data(frame.data(), frame.size());
    std::unique_ptr<vsomeip_v3::sd::message_impl> message(deserializer.deserialize_sd_message());
    if (!message) {
        return ports;
    }
    for (const auto& entry : message->get_entries()) {
        auto eventgroup_entry = std::dynamic_pointer_cast<vsomeip_v3::sd::eventgroupentry_impl>(entry);
        if (!eventgroup_entry || eventgroup_entry->get_type() != vsomeip_v3::sd::entry_type_e::SUBSCRIBE_EVENTGROUP
            || eventgroup_entry->get_ttl() == 0) {
            continue;
        }
        for (const auto run : {std::uint8_t{1}, std::uint8_t{2}}) {
            for (const auto index : eventgroup_entry->get_options(run)) {
                auto option = std::dynamic_pointer_cast<vsomeip_v3::sd::ipv4_option_impl>(message->get_option(index));
                if (!option || option->get_layer_four_protocol() != vsomeip_v3::sd::layer_four_protocol_e::UDP) {
                    continue;
                }
                ports[eventgroup_entry->get_eventgroup()] = option->get_port();
            }
        }
    }
    return ports;
}

std::map<vsomeip_v3::eventgroup_t, std::uint16_t> last_subscribe_ports(const std::vector<vsomeip_v3::message_buffer_t>& frames) {
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        auto ports = subscribe_ports(*it);
        if (!ports.empty()) {
            return ports;
        }
    }
    return {};
}

class per_eventgroup_subscriber_endpoints : public ::testing::Test {
protected:
    void SetUp() override {
        configuration_ = std::make_shared<vsomeip_v3::cfg::configuration_impl>("");
        discovery_ = std::make_shared<vsomeip_v3::sd::service_discovery_impl>(&host_, configuration_);
        discovery_->init();
    }

    void TearDown() override {
        if (discovery_) {
            discovery_->stop();
        }
        io_.poll();
        discovery_.reset();
    }

    std::shared_ptr<vsomeip_v3::eventgroupinfo> make_eventgroup(vsomeip_v3::eventgroup_t eventgroup) {
        auto info = std::make_shared<vsomeip_v3::eventgroupinfo>(k_service, k_instance, eventgroup, k_major, k_ttl, 0);
        info->set_reliability(vsomeip_v3::reliability_type_e::RT_UNRELIABLE);
        host_.remember(info);
        infos_.push_back(info);
        return info;
    }

    void subscribe_both() {
        discovery_->subscribe(k_service, k_instance, k_eventgroup_a, k_major, k_ttl, k_client, make_eventgroup(k_eventgroup_a));
        discovery_->subscribe(k_service, k_instance, k_eventgroup_b, k_major, k_ttl, k_client, make_eventgroup(k_eventgroup_b));
    }

    void inject_offer(vsomeip_v3::ttl_t ttl) {
        auto frame = make_offer(ttl, session_++);
        discovery_->on_message(frame.data(), static_cast<vsomeip_v3::length_t>(frame.size()), host_.remote_, true);
    }

    bool saw_eventgroup_lookup(vsomeip_v3::eventgroup_t eventgroup, bool reliable) const {
        for (const auto& lookup : host_.lookups_) {
            if (lookup.saw_eventgroup && lookup.eventgroup == eventgroup && lookup.reliable == reliable && lookup.service == k_service
                && lookup.instance == k_instance) {
                return true;
            }
        }
        return false;
    }

    boost::asio::io_context io_;
    per_eventgroup_host host_{io_};
    std::shared_ptr<vsomeip_v3::cfg::configuration_impl> configuration_;
    std::shared_ptr<vsomeip_v3::sd::service_discovery_impl> discovery_;
    std::vector<std::shared_ptr<vsomeip_v3::eventgroupinfo>> infos_;
    vsomeip_v3::session_t session_{1};
};

TEST_F(per_eventgroup_subscriber_endpoints, offer_after_subscribe_uses_each_eventgroup_port) {
    subscribe_both();
    EXPECT_TRUE(host_.sent_frames_.empty());
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_a, false));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, false));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_a, true));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, true));

    host_.sent_frames_.clear();
    inject_offer(k_ttl);

    const auto ports = last_subscribe_ports(host_.sent_frames_);
    ASSERT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports.at(k_eventgroup_a), k_port_a);
    EXPECT_EQ(ports.at(k_eventgroup_b), k_port_b);
}

TEST_F(per_eventgroup_subscriber_endpoints, multicast_resubscribe_keeps_ports_per_eventgroup) {
    subscribe_both();
    inject_offer(k_ttl);
    host_.lookups_.clear();
    host_.sent_frames_.clear();

    inject_offer(k_ttl);

    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_a, false));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, false));
    const auto ports = last_subscribe_ports(host_.sent_frames_);
    ASSERT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports.at(k_eventgroup_a), k_port_a);
    EXPECT_EQ(ports.at(k_eventgroup_b), k_port_b);
}

TEST_F(per_eventgroup_subscriber_endpoints, endpoint_connected_sends_the_other_eventgroup_on_its_own_port) {
    auto established = make_eventgroup(k_eventgroup_a);
    auto pending = make_eventgroup(k_eventgroup_b);
    host_.create_established_ = true;
    discovery_->subscribe(k_service, k_instance, k_eventgroup_a, k_major, k_ttl, k_client, established);
    host_.create_established_ = false;
    discovery_->subscribe(k_service, k_instance, k_eventgroup_b, k_major, k_ttl, k_client, pending);

    const auto already_sent = last_subscribe_ports(host_.sent_frames_);
    ASSERT_EQ(already_sent.size(), 1u);
    EXPECT_EQ(already_sent.at(k_eventgroup_a), k_port_a);

    auto pending_endpoint = host_.endpoint_for(k_eventgroup_b, false);
    ASSERT_NE(pending_endpoint, nullptr);
    pending_endpoint->set_established(true);
    host_.sent_frames_.clear();
    host_.lookups_.clear();
    discovery_->on_endpoint_connected(k_service, k_instance, pending_endpoint);

    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, false));
    const auto ports = last_subscribe_ports(host_.sent_frames_);
    ASSERT_EQ(ports.count(k_eventgroup_b), 1u);
    EXPECT_EQ(ports.at(k_eventgroup_b), k_port_b);
    if (ports.count(k_eventgroup_a) != 0) {
        EXPECT_EQ(ports.at(k_eventgroup_a), k_port_a);
    }
}

TEST_F(per_eventgroup_subscriber_endpoints, stop_offer_then_reoffer_looks_up_each_eventgroup_again) {
    subscribe_both();
    inject_offer(k_ttl);
    EXPECT_EQ(host_.del_routing_info_count_, 0);

    inject_offer(0);
    EXPECT_EQ(host_.del_routing_info_count_, 1);
    EXPECT_EQ(host_.endpoint_for(k_eventgroup_a, false), nullptr);

    host_.lookups_.clear();
    host_.sent_frames_.clear();
    host_.create_established_ = true;
    // StopOffer removed the SD subscriptions. A routing manager re-subscribes
    // from on_availability when the offer returns; do that here, then re-offer.
    discovery_->subscribe(k_service, k_instance, k_eventgroup_a, k_major, k_ttl, k_client, infos_[0]);
    discovery_->subscribe(k_service, k_instance, k_eventgroup_b, k_major, k_ttl, k_client, infos_[1]);
    inject_offer(k_ttl);

    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_a, false));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, false));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_a, true));
    EXPECT_TRUE(saw_eventgroup_lookup(k_eventgroup_b, true));
    const auto ports = last_subscribe_ports(host_.sent_frames_);
    ASSERT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports.at(k_eventgroup_a), k_port_a);
    EXPECT_EQ(ports.at(k_eventgroup_b), k_port_b);
}

TEST(legacy_three_argument_host, both_eventgroups_share_one_port) {
    boost::asio::io_context io;
    legacy_host host(io);
    auto configuration = std::make_shared<vsomeip_v3::cfg::configuration_impl>("");
    auto discovery = std::make_shared<vsomeip_v3::sd::service_discovery_impl>(&host, configuration);
    discovery->init();

    auto info_a = std::make_shared<vsomeip_v3::eventgroupinfo>(k_service, k_instance, k_eventgroup_a, k_major, k_ttl, 0);
    auto info_b = std::make_shared<vsomeip_v3::eventgroupinfo>(k_service, k_instance, k_eventgroup_b, k_major, k_ttl, 0);
    info_a->set_reliability(vsomeip_v3::reliability_type_e::RT_UNRELIABLE);
    info_b->set_reliability(vsomeip_v3::reliability_type_e::RT_UNRELIABLE);
    host.remember(info_a);
    host.remember(info_b);
    discovery->subscribe(k_service, k_instance, k_eventgroup_a, k_major, k_ttl, k_client, info_a);
    discovery->subscribe(k_service, k_instance, k_eventgroup_b, k_major, k_ttl, k_client, info_b);

    for (const auto& lookup : host.lookups_) {
        EXPECT_FALSE(lookup.saw_eventgroup);
        EXPECT_EQ(lookup.eventgroup, vsomeip_v3::ANY_EVENTGROUP);
    }

    auto frame = make_offer(k_ttl, 1);
    discovery->on_message(frame.data(), static_cast<vsomeip_v3::length_t>(frame.size()), host.remote_, true);

    const auto ports = last_subscribe_ports(host.sent_frames_);
    ASSERT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports.at(k_eventgroup_a), k_shared_port);
    EXPECT_EQ(ports.at(k_eventgroup_b), k_shared_port);
    EXPECT_EQ(host.endpoint_for(k_eventgroup_a, false), nullptr);
    EXPECT_NE(host.endpoint_for(vsomeip_v3::ANY_EVENTGROUP, false), nullptr);

    discovery->stop();
    io.poll();
}

} // namespace
