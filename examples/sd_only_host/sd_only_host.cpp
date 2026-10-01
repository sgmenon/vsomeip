// Copyright (C) 2026 GM Global Technology Operations LLC.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Standalone Service-Discovery host.
//
// Links against `libvsomeip3-core.so` (the wire-level subset of vsomeip) and
// `libvsomeip3-sd.so.3` (the SD plugin). Does NOT link against
// `libvsomeip3.so`: no routing_manager_impl, no application_impl, no
// local-UDS endpoints, no netlink_connector. The process should open exactly
// one UDP socket (the SD port) — no AF_NETLINK socket, no AF_UNIX socket, no
// per-service data ports.
//
// Verify with:
//   strace -f -e trace=socket,bind,connect ./sd_only_host
//   ss -anp | grep $(pgrep sd_only_host)
//
// -----------------------------------------------------------------------------
// What this example demonstrates
// -----------------------------------------------------------------------------
//
// A "standalone SD host" is a process that participates in the SOME/IP
// service-discovery protocol as a peer (offering services, sending FIND
// entries, receiving OFFERs) but delegates the data plane — the actual UDP
// sockets that carry SOME/IP method calls and notifications — to some
// other process. Typical shapes:
//
//   * an SD authority / broker that centralises the SD PDU stream on behalf
//     of a fleet of routing managers behind it;
//   * a diagnostic sniffer that observes the SD layer to build a live map
//     of the SOME/IP topology on the network;
//   * a bring-up scaffold that lets you test the SD wire behaviour of a
//     component before its data plane is functional.
//
// This example is closest to the third: it prints a running routing table
// derived from the OFFERs it hears on the wire.
//
// -----------------------------------------------------------------------------
// Real vs phantom remote-client endpoints
// -----------------------------------------------------------------------------
//
// `service_discovery_impl::find_or_create_remote_client` asks the host for a
// client-side endpoint to a peer's advertised service. The SD engine reads
// `get_local_port()` off it (for the subscriber endpoint option in an
// outbound SubscribeEventgroup PDU) and `get_remote_address()` for the PDU
// destination. It does NOT push any bytes through this endpoint — the SD
// PDU itself goes out via the SD UDP socket (see `send_via_sd`) — so the
// endpoint just needs to answer those two accessors.
//
// For pedagogical clarity this example uses a real `udp/tcp_client_endpoint_impl`
// the learned peer's address+port and a configured local port, but we
// deliberately *never call `start()` on it*. Because `start()` is what
// opens the OS socket and binds the local port, skipping it means no data
// port is ever opened by this process. The endpoint object still answers
// `get_local_port()` and `get_remote_address()` with the values passed
// through its constructor, which is all SD needs.
//
// Side effect: `is_established()` returns false on an unstarted client
// endpoint, so SD won't actually emit a SubscribeEventgroup PDU from this
// host. That's fine for a demo — the point is to observe SD traffic, not
// to route notifications.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>

// vsomeip's public headers must come before the cfg::* structs.
#include <vsomeip/constants.hpp>
#include <vsomeip/internal/serializable.hpp>
#include <vsomeip/vsomeip_sec.h>

#include <vsomeip/implementation/configuration/include/client.hpp>
#include <vsomeip/implementation/configuration/include/configuration_impl.hpp>
#include <vsomeip/implementation/configuration/include/eventgroup.hpp>
#include <vsomeip/implementation/configuration/include/service.hpp>
#include <vsomeip/implementation/endpoints/include/endpoint_host.hpp>
#include <vsomeip/implementation/endpoints/include/tcp_client_endpoint_impl.hpp>
#include <vsomeip/implementation/endpoints/include/udp_client_endpoint_impl.hpp>
#include <vsomeip/implementation/endpoints/include/udp_server_endpoint_impl.hpp>
#include <vsomeip/implementation/message/include/serializer.hpp>
#include <vsomeip/implementation/routing/include/eventgroupinfo.hpp>
#include <vsomeip/implementation/routing/include/routing_host.hpp>
#include <vsomeip/implementation/routing/include/serviceinfo.hpp>
#include <vsomeip/implementation/service_discovery/include/defines.hpp>
#include <vsomeip/implementation/service_discovery/include/runtime.hpp>
#include <vsomeip/implementation/service_discovery/include/service_discovery_host.hpp>
#include <vsomeip/implementation/service_discovery/include/service_discovery_impl.hpp>

namespace v3 = vsomeip_v3;

namespace {

// -----------------------------------------------------------------------------
// Description of one service this host advertises.
// -----------------------------------------------------------------------------
struct offered_service {
    v3::service_t service_id{0};
    v3::instance_t instance_id{0};
    v3::major_version_t major_version{1};
    v3::minor_version_t minor_version{0};
    std::uint16_t unreliable_port{0}; // data port carried in the OFFER PDU
    std::vector<v3::eventgroup_t> eventgroups;
};

// Fired when an inbound OfferService arrives from a remote peer. This is the
// main "we learned something new about the network" hook — an SD-only
// consumer typically records the (service, instance, addr:port) tuple into
// a routing table it exposes to whoever owns the data plane.
using on_remote_offer_hook_t = std::function<void(v3::service_t, v3::instance_t, v3::major_version_t)>;

// -----------------------------------------------------------------------------
// endpoint_host_stub / routing_host_stub
//
// `udp_server_endpoint_impl` (used for the SD multicast socket) reaches a
// small set of callbacks on these two interfaces. None of them require a
// routing manager in an SD-only host — see the per-method comments.
// -----------------------------------------------------------------------------
class endpoint_host_stub : public v3::endpoint_host {
public:
    explicit endpoint_host_stub(boost::asio::io_context& io) : io_(io) {}

    void on_connect(std::shared_ptr<v3::endpoint>) override {}
    void on_disconnect(std::shared_ptr<v3::endpoint>) override {}
    bool on_bind_error(std::shared_ptr<v3::endpoint>, const boost::asio::ip::address&, uint16_t, uint16_t&) override {
        return false;
    }
    void on_error(const v3::byte_t*, v3::length_t, v3::endpoint*, const boost::asio::ip::address& addr, std::uint16_t port) override {
        std::cout << "[endpoint_host] malformed SOME/IP packet from " << addr.to_string() << ':' << port << '\n';
    }
    void release_port(uint16_t, bool) override {}
    v3::client_t get_client() const override { return 0; }
    std::string get_client_host() const override { return "sd_only_host"; }
    v3::instance_t find_instance(v3::service_t, v3::endpoint* const) const override { return v3::ANY_INSTANCE; }

    // The SD endpoint's join()/leave() delegate IP_ADD_MEMBERSHIP here (see
    // udp_server_endpoint_impl::join_unlocked -> endpoint_host->
    // add_multicast_option). In a full vsomeip app endpoint_manager_impl
    // queues this onto a worker that calls set_multicast_option; an SD-only
    // host has no such worker, so apply it ourselves. Posting via the
    // io_context avoids the deadlock caused by re-entering
    // udp_server_endpoint_impl's non-recursive mutex.
    void add_multicast_option(const v3::multicast_option_t& option) override {
        boost::asio::post(io_,
                          [ep = option.endpoint_, is_join = option.is_join_, address = option.address_]() {
                              auto server = std::dynamic_pointer_cast<v3::udp_server_endpoint_impl>(ep);
                              if (!server) return;
                              boost::system::error_code ec;
                              server->set_multicast_option(address, is_join, ec);
                              std::cout << "[endpoint_host] " << (is_join ? "joined" : "left") << " multicast "
                                        << address.to_string() << (ec ? (" (error: " + ec.message() + ")") : "") << '\n';
                          });
    }

private:
    boost::asio::io_context& io_;
};

class routing_host_stub : public v3::routing_host {
public:
    using on_message_cbk_t = std::function<void(const v3::byte_t*, v3::length_t, bool, const boost::asio::ip::address&, std::uint16_t)>;

    void set_on_message(on_message_cbk_t cbk) { on_message_ = std::move(cbk); }

    // The SD multicast endpoint delivers every received packet here. Forward
    // it straight into service_discovery_impl (which the owner installs via
    // set_on_message) — mirrors what routing_manager_impl::on_message does
    // for service id 0xFFFF in a full vsomeip app. SD parses and validates
    // the PDU itself.
    void on_message(v3::owned_buffer_slice frame, v3::endpoint*, bool is_multicast, v3::client_t, const vsomeip_sec_client_t*,
                    const boost::asio::ip::address& remote_address, std::uint16_t remote_port) override {
        if (on_message_ && frame.valid() && !frame.empty())
            on_message_(frame.data(), static_cast<v3::length_t>(frame.length), is_multicast, remote_address, remote_port);
    }
    v3::client_t get_client() const override { return 0; }
    void add_known_client(v3::client_t, const std::string&) override {}
    v3::client_t get_guest_by_address(const boost::asio::ip::address&, v3::port_t) const override { return VSOMEIP_CLIENT_UNSET; }
    void add_guest(v3::client_t, const boost::asio::ip::address&, v3::port_t) override {}
    void remove_local(v3::client_t, bool, bool) override {}
    std::string get_env(v3::client_t) const override { return {}; }
    void remove_subscriptions(v3::port_t, const boost::asio::ip::address&, v3::port_t) override {}

private:
    on_message_cbk_t on_message_;
};

// -----------------------------------------------------------------------------
// sd_only_host
//
// `service_discovery_host` implementation for a process that speaks SD but
// owns no data plane. Structure mirrors the Cruise repo's `BrokerSDHost` —
// offered_services vector in, learned_offers table populated as OFFERs
// arrive, on_remote_offer callback fires inline in add_routing_info. The
// remote-client factory returns real `udp_client_endpoint_impl` /
// `tcp_client_endpoint_impl` objects that are never started (no OS socket
// is opened by this process for the data plane).
// -----------------------------------------------------------------------------
class sd_only_host : public v3::sd::service_discovery_host {
public:
    sd_only_host(std::shared_ptr<v3::cfg::configuration_impl> configuration, boost::asio::io_context& io)
        : configuration_(std::move(configuration)),
          io_(io),
          endpoint_host_(std::make_shared<endpoint_host_stub>(io)),
          routing_host_(std::make_shared<routing_host_stub>()) {
        routing_host_->set_on_message([this](const v3::byte_t* data, v3::length_t len, bool is_mcast,
                                             const boost::asio::ip::address& addr, std::uint16_t) {
            if (service_discovery_) {
                service_discovery_->on_message(data, len, addr, is_mcast);
            }
        });
    }

    void set_service_discovery(v3::sd::service_discovery* sd) { service_discovery_ = sd; }
    void set_on_remote_offer(on_remote_offer_hook_t hook) { on_remote_offer_ = std::move(hook); }

    // Register the server-side services to advertise. Builds the
    // `serviceinfo` / `eventgroupinfo` objects `get_offered_services()` and
    // `find_eventgroup()` answer from. Call before `service_discovery::start()`.
    void set_offered_services(const std::vector<offered_service>& services) {
        offered_services_.clear();
        offered_eventgroups_.clear();
        const auto sd_ttl = static_cast<v3::ttl_t>(v3::DEFAULT_TTL);
        for (const auto& svc : services) {
            auto info = std::make_shared<v3::serviceinfo>(svc.service_id, svc.instance_id, svc.major_version, svc.minor_version, sd_ttl,
                                                          /*_is_local=*/true);
            // The OfferService PDU carries an IPv4 endpoint option whose
            // port is read off the serviceinfo's endpoint via
            // `insert_offer_service -> get_endpoint(reliable)->get_local_port()`.
            // We haven't opened the data port on this host (that's owned by
            // whoever runs the actual service), so wrap a real
            // udp_client_endpoint_impl with the correct local port and
            // never start it. `get_local_port()` still returns the value we
            // configured.
            if (svc.unreliable_port != 0 && svc.unreliable_port != v3::ILLEGAL_PORT) {
                info->set_endpoint(make_unstarted_client_endpoint(/*reliable=*/false, svc.unreliable_port,
                                                                  /*remote=*/boost::asio::ip::address{}, /*remote_port=*/0),
                                   /*_reliable=*/false);
            }
            offered_services_[svc.service_id][svc.instance_id] = info;

            auto& eg_map = offered_eventgroups_[{svc.service_id, svc.instance_id}];
            for (const auto eg : svc.eventgroups) {
                eg_map[eg] = std::make_shared<v3::eventgroupinfo>(svc.service_id, svc.instance_id, eg, svc.major_version, sd_ttl,
                                                                  /*_max_remote_subscribers=*/0);
            }
        }
        std::cout << "[sd_only_host] registered " << services.size() << " offered service(s)\n";
    }

    // Return a snapshot of the routing table this host has learned from
    // inbound OFFERs. Cheap to call — used by the periodic printer in main.
    struct known_offer {
        v3::service_t service{0};
        v3::instance_t instance{0};
        v3::major_version_t major{0};
        boost::asio::ip::address reliable_address;
        boost::asio::ip::address unreliable_address;
        std::uint16_t reliable_port{0};
        std::uint16_t unreliable_port{0};
    };
    std::vector<known_offer> snapshot_learned_offers() const {
        std::lock_guard lock{learned_mutex_};
        std::vector<known_offer> out;
        out.reserve(learned_offers_.size());
        for (const auto& [key, val] : learned_offers_) out.push_back(val);
        return out;
    }

    // ---- service_discovery_host interface ----

    boost::asio::io_context& get_io() override { return io_; }

    std::shared_ptr<v3::endpoint> create_service_discovery_endpoint(const std::string& address, uint16_t port, bool reliable) override {
        if (reliable) {
            std::cerr << "[sd_only_host] SD over TCP not supported\n";
            return nullptr;
        }
        auto ep = std::make_shared<v3::udp_server_endpoint_impl>(endpoint_host_, routing_host_, io_, configuration_);
        const auto& unicast = configuration_->get_unicast_address();
        boost::asio::ip::udp::endpoint local{unicast, port};
        boost::system::error_code ec;
        ep->init(local, ec);
        if (ec) {
            std::cerr << "[sd_only_host] SD endpoint init failed: " << ec.message() << '\n';
            return nullptr;
        }
        ep->start();
        ep->add_default_target(VSOMEIP_SD_SERVICE, address, port);

        // Feed the SD engine its OWN sent/looped-back offers, exactly as
        // routing_manager_impl does when it creates the SD endpoint. When
        // SD sends (or receives back via own-multicast) an OfferService,
        // service_discovery_impl::check_sent_offers marks the local
        // serviceinfo as accepting remote subscriptions — without this
        // wiring, inbound SubscribeEventgroup PDUs are NACKed with ttl=0.
        if (service_discovery_) {
            auto* sd = service_discovery_;
            const auto forward = [sd](const v3::byte_t* data, v3::length_t size, const boost::asio::ip::address& remote) {
                sd->sent_messages(data, size, remote);
            };
            ep->set_unicast_sent_callback(forward);
            ep->set_sent_multicast_received_callback(forward);
        }

        ep->set_receive_own_multicast_messages(true);
        ep->join(address);

        std::cout << "[sd_only_host] SD endpoint bound to " << unicast.to_string() << ':' << port << ", joined " << address << '\n';
        sd_endpoint_ = ep;
        return ep;
    }

    // Inbound OfferService parsed by service_discovery_impl. Two things
    // happen here:
    //   1. Record the peer's (service, instance) -> address:port mapping
    //      into `learned_offers_` — this is the routing table the demo
    //      prints.
    //   2. Fire the on_remote_offer hook so a caller can react
    //      synchronously (log, forward to a downstream RM, whatever).
    // Only overwrite a side when this OFFER actually carries it, so a
    // reliable-only cyclic re-offer doesn't wipe a previously learned
    // unreliable side.
    void add_routing_info(v3::service_t service, v3::instance_t instance, v3::major_version_t major, v3::minor_version_t /*minor*/,
                          v3::ttl_t /*ttl*/, const boost::asio::ip::address& reliable_addr, uint16_t reliable_port,
                          const boost::asio::ip::address& unreliable_addr, uint16_t unreliable_port, bool /*is_reliable_known*/,
                          bool /*is_unreliable_known*/) override {
        {
            std::lock_guard lock{learned_mutex_};
            auto& entry = learned_offers_[{service, instance}];
            entry.service = service;
            entry.instance = instance;
            entry.major = major;
            if (reliable_port != 0 && !reliable_addr.is_unspecified()) {
                entry.reliable_address = reliable_addr;
                entry.reliable_port = reliable_port;
            }
            if (unreliable_port != 0 && !unreliable_addr.is_unspecified()) {
                entry.unreliable_address = unreliable_addr;
                entry.unreliable_port = unreliable_port;
            }
        }
        std::ostringstream os;
        os << "[sd_only_host] OFFER learned [" << format_id(service) << '.' << format_id(instance) << "] major=" << +major;
        if (reliable_port != 0) os << " tcp=" << reliable_addr.to_string() << ':' << reliable_port;
        if (unreliable_port != 0) os << " udp=" << unreliable_addr.to_string() << ':' << unreliable_port;
        std::cout << os.str() << '\n';

        if (on_remote_offer_) on_remote_offer_(service, instance, static_cast<v3::major_version_t>(major));
    }

    // Every OFFER is recorded, including one that moves to a new endpoint,
    // so nothing is ever reported as known and no offer is dropped.
    void is_remote_service_known(v3::service_t, v3::instance_t, v3::major_version_t, v3::minor_version_t, const boost::asio::ip::address&,
                                 uint16_t, bool& reliable_known, const boost::asio::ip::address&, uint16_t, bool& unreliable_known,
                                 bool& drop_offer) override {
        reliable_known = false;
        unreliable_known = false;
        drop_offer = false;
    }

    void del_routing_info(v3::service_t service, v3::instance_t instance, bool /*has_reliable*/, bool /*has_unreliable*/,
                          bool /*trigger_availability*/) override {
        std::lock_guard lock{learned_mutex_};
        learned_offers_.erase({service, instance});
        std::cout << "[sd_only_host] OFFER expired [" << format_id(service) << '.' << format_id(instance) << "]\n";
    }

    void update_routing_info(std::chrono::milliseconds /*elapsed*/) override {}

    v3::services_t get_offered_services() const override { return offered_services_; }

    std::shared_ptr<v3::serviceinfo> get_offered_service(v3::service_t service, v3::instance_t instance) const override {
        auto svc = offered_services_.find(service);
        if (svc == offered_services_.end()) return nullptr;
        auto inst = svc->second.find(instance);
        return inst == svc->second.end() ? nullptr : inst->second;
    }

    std::map<v3::instance_t, std::shared_ptr<v3::serviceinfo>>
    get_offered_service_instances(v3::service_t service) const override {
        auto svc = offered_services_.find(service);
        return svc == offered_services_.end() ? std::map<v3::instance_t, std::shared_ptr<v3::serviceinfo>>{} : svc->second;
    }

    std::shared_ptr<v3::eventgroupinfo> find_eventgroup(v3::service_t service, v3::instance_t instance,
                                                        v3::eventgroup_t eventgroup) const override {
        auto it = offered_eventgroups_.find({service, instance});
        if (it == offered_eventgroups_.end()) return nullptr;
        auto eg = it->second.find(eventgroup);
        return eg == it->second.end() ? nullptr : eg->second;
    }

    // Called by service_discovery_impl::send() for every outbound SD PDU
    // (OfferService, FindService, SubscribeEventgroup, SubscribeAck, ...).
    // Serialize the message and hand the bytes to the SD UDP endpoint,
    // which routes them to its default target (the SD multicast group).
    // Mirrors routing_manager_base::send() + routing_manager_impl::send()
    // for the SD path.
    bool send(v3::client_t, std::shared_ptr<v3::message> message, bool) override {
        if (!sd_endpoint_) return false;
        auto* serializable_message = dynamic_cast<const v3::serializable*>(message.get());
        if (!serializable_message) return false;
        v3::serializer ser{/*_buffer_shrink_threshold=*/0};
        if (!ser.serialize(serializable_message)) {
            std::cerr << "[sd_only_host] SD message serialize failed\n";
            return false;
        }
        return sd_endpoint_->send(ser.get_data(), ser.get_size());
    }

    bool send_via_sd(const std::shared_ptr<v3::endpoint_definition>& target, v3::message_buffer_ptr_t frame,
                     uint16_t /*sd_port*/) override {
        if (!sd_endpoint_ || !frame || frame->empty())
            return false;
        return sd_endpoint_->send_to(target, frame->data(), static_cast<uint32_t>(frame->size()));
    }

    // Return a client-side endpoint pointing at a peer whose OfferService
    // we've received. Constructs a real udp/tcp_client_endpoint_impl but
    // never calls start() on it, so no OS socket is bound. The endpoint's
    // get_local_port() and get_remote_address() still return the values we
    // seeded the constructor with, which is everything the SD engine reads.
    std::shared_ptr<v3::endpoint> find_or_create_remote_client(v3::service_t service, v3::instance_t instance, bool reliable) override {
        const auto cache_key = std::make_tuple(service, instance, reliable);
        if (auto it = remote_clients_.find(cache_key); it != remote_clients_.end()) return it->second;

        boost::asio::ip::address remote_address;
        std::uint16_t remote_port = 0;
        {
            std::lock_guard lock{learned_mutex_};
            auto offer = learned_offers_.find({service, instance});
            if (offer == learned_offers_.end()) return nullptr;
            remote_address = reliable ? offer->second.reliable_address : offer->second.unreliable_address;
            remote_port = reliable ? offer->second.reliable_port : offer->second.unreliable_port;
        }
        if (remote_address.is_unspecified() || remote_port == 0) return nullptr;

        // The local port on this endpoint is what the SubscribeEventgroup
        // PDU advertises to the peer as "send my notifications here". In a
        // full vsomeip app it comes from the clients[] configured
        // subscriber-port range; we use the same accessor.
        std::uint16_t local_port = 0;
        std::map<bool, std::set<std::uint16_t>> used_client_ports;
        if (!configuration_->get_client_port(service, instance, remote_port, reliable, used_client_ports, local_port) || local_port == 0) {
            std::cout << "[sd_only_host] no configured client port for [" << format_id(service) << '.' << format_id(instance)
                      << "] reliable=" << reliable << " — cannot construct remote-client endpoint\n";
            return nullptr;
        }

        auto ep = make_unstarted_client_endpoint(reliable, local_port, remote_address, remote_port);
        remote_clients_[cache_key] = ep;
        std::cout << "[sd_only_host] built unstarted " << (reliable ? "tcp" : "udp") << " client endpoint for [" << format_id(service)
                  << '.' << format_id(instance) << "] local=" << local_port << " peer=" << remote_address.to_string() << ':' << remote_port
                  << '\n';
        return ep;
    }

    std::set<v3::eventgroup_t> get_subscribed_eventgroups(v3::service_t, v3::instance_t) override { return {}; }

    // ---- inert observer surface ----
    // Everything below is a no-op: this host doesn't route application
    // traffic, and its SubscribeAck path is uninteresting for a demo. See
    // the Cruise `broker_sd_host` for a version that handles inbound
    // SUBSCRIBEs by ACK'ing them and forwarding to a real subscriber.
    void on_remote_subscribe(std::shared_ptr<v3::remote_subscription>&, const v3::remote_subscription_callback_t&) override {}
    void on_remote_unsubscribe(std::shared_ptr<v3::remote_subscription>&) override {}
    void on_subscribe_ack(v3::client_t, v3::service_t, v3::instance_t, v3::eventgroup_t, v3::event_t, v3::remote_subscription_id_t) override {}
    void on_subscribe_ack_with_multicast(v3::service_t, v3::instance_t, const boost::asio::ip::address&, const boost::asio::ip::address&,
                                          uint16_t) override {}
    void on_subscribe_nack(v3::client_t, v3::service_t, v3::instance_t, v3::eventgroup_t, bool, v3::remote_subscription_id_t) override {}
    void expire_subscriptions(const boost::asio::ip::address&) override {}
    void expire_subscriptions(const boost::asio::ip::address&, std::uint16_t, bool) override {}
    void expire_services(const boost::asio::ip::address&) override {}
    void expire_services(const boost::asio::ip::address&, std::uint16_t, bool) override {}
    std::chrono::steady_clock::time_point expire_subscriptions(bool /*force*/) override {
        // Push the next TTL-expiry check far out; this demo never times
        // learned entries on a clock.
        return std::chrono::steady_clock::now() + std::chrono::hours{24};
    }

private:
    // Build a real udp/tcp_client_endpoint_impl and deliberately do NOT
    // call start(). Because start() is what opens the OS socket and binds
    // the local port, skipping it means no data port is bound by this
    // process; the endpoint object still answers get_local_port() and
    // get_remote_address() with the constructor-seeded values.
    std::shared_ptr<v3::endpoint> make_unstarted_client_endpoint(bool reliable, std::uint16_t local_port,
                                                                 boost::asio::ip::address remote_address, std::uint16_t remote_port) {
        const auto& unicast = configuration_->get_unicast_address();
        if (reliable) {
            boost::asio::ip::tcp::endpoint local{unicast, local_port};
            boost::asio::ip::tcp::endpoint remote{remote_address.is_unspecified() ? unicast : remote_address, remote_port};
            return std::make_shared<v3::tcp_client_endpoint_impl>(endpoint_host_, routing_host_, local, remote, io_, configuration_,
                                                                   /*_use_magic_cookies=*/false);
        }
        boost::asio::ip::udp::endpoint local{unicast, local_port};
        boost::asio::ip::udp::endpoint remote{remote_address.is_unspecified() ? unicast : remote_address, remote_port};
        return std::make_shared<v3::udp_client_endpoint_impl>(endpoint_host_, routing_host_, local, remote, io_, configuration_);
    }

    static std::string format_id(std::uint16_t id) {
        std::ostringstream os;
        os << "0x" << std::hex << std::setw(4) << std::setfill('0') << id;
        return os.str();
    }

    std::shared_ptr<v3::cfg::configuration_impl> configuration_;
    boost::asio::io_context& io_;
    std::shared_ptr<endpoint_host_stub> endpoint_host_;
    std::shared_ptr<routing_host_stub> routing_host_;
    std::shared_ptr<v3::endpoint> sd_endpoint_;

    v3::sd::service_discovery* service_discovery_{nullptr};
    on_remote_offer_hook_t on_remote_offer_;

    v3::services_t offered_services_;
    std::map<std::pair<v3::service_t, v3::instance_t>, std::map<v3::eventgroup_t, std::shared_ptr<v3::eventgroupinfo>>> offered_eventgroups_;

    mutable std::mutex learned_mutex_;
    std::map<std::pair<v3::service_t, v3::instance_t>, known_offer> learned_offers_;

    std::map<std::tuple<v3::service_t, v3::instance_t, bool>, std::shared_ptr<v3::endpoint>> remote_clients_;
};

// -----------------------------------------------------------------------------
// Signal handling
// -----------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

// -----------------------------------------------------------------------------
// Config -> host wiring
//
// Every services[] entry -> an OfferService PDU emitted by this host.
// Every clients[]  entry -> a FindService PDU emitted by this host, plus a
//                           SubscribeEventgroup queued for every eventgroup
//                           declared for that (service, instance) in the
//                           config (SD only puts SUBSCRIBEs on the wire
//                           once a matching OFFER arrives). Because the
//                           remote-client endpoint we hand back is never
//                           started, is_established() is false and the SD
//                           engine ends up NOT actually emitting the
//                           SUBSCRIBE — the outbound-subscribe path is a
//                           demonstration of the plumbing, not a working
//                           subscriber.
// -----------------------------------------------------------------------------
void drive_sd_from_config(v3::cfg::configuration_impl& cfg, v3::sd::service_discovery& sd, sd_only_host& host, v3::ttl_t sd_ttl) {
    // We don't store major/minor in cfg::service (upstream takes them from
    // the application API at runtime). For this example a fixed pair is
    // fine and keeps the wire trace readable.
    constexpr v3::major_version_t k_major = 1;
    constexpr v3::minor_version_t k_minor = 0;

    std::vector<offered_service> offered;
    std::map<std::pair<v3::service_t, v3::instance_t>, std::shared_ptr<v3::cfg::service>> services_by_id;
    for (const auto& svc : cfg.get_services()) {
        services_by_id[{svc->service_, svc->instance_}] = svc;
        offered_service os_entry{svc->service_,
                                 svc->instance_,
                                 k_major,
                                 k_minor,
                                 svc->unreliable_ == v3::ILLEGAL_PORT ? std::uint16_t{0} : svc->unreliable_,
                                 {}};
        for (const auto& [eg_id, _] : svc->eventgroups_) os_entry.eventgroups.push_back(eg_id);
        offered.push_back(std::move(os_entry));
    }
    host.set_offered_services(offered);
    for (const auto& os_entry : offered) {
        auto info = host.get_offered_service(os_entry.service_id, os_entry.instance_id);
        if (info) sd.offer_service(info);
    }

    for (const auto& cli : cfg.get_clients()) {
        sd.request_service(cli->service_, cli->instance_, k_major, k_minor, v3::DEFAULT_TTL);
        std::cout << "[main] requested [0x" << std::hex << std::setw(4) << std::setfill('0') << cli->service_ << '.' << std::setw(4)
                  << cli->instance_ << std::dec << "]\n";
        auto svc_it = services_by_id.find({cli->service_, cli->instance_});
        if (svc_it == services_by_id.end()) continue;
        for (const auto& [eg_id, _] : svc_it->second->eventgroups_) {
            auto eg_info = std::make_shared<v3::eventgroupinfo>(cli->service_, cli->instance_, eg_id, k_major, sd_ttl,
                                                                /*_max_remote_subscribers=*/0);
            sd.subscribe(cli->service_, cli->instance_, eg_id, k_major, sd_ttl, /*VSOMEIP_ROUTING_CLIENT=*/0, eg_info);
        }
    }
}

// Pretty-print the routing table the host has learned so far. Called from
// main() on a wall-clock heartbeat.
void dump_routing_table(const sd_only_host& host, std::chrono::steady_clock::time_point started_at) {
    const auto offers = host.snapshot_learned_offers();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at).count();
    std::ostringstream os;
    os << "\n=== routing table @ t+" << elapsed << "s (" << offers.size() << " learned OFFER"
       << (offers.size() == 1 ? "" : "s") << ") ===\n";
    if (offers.empty()) {
        os << "  (no remote OFFERs received yet)\n";
    } else {
        os << "  " << std::left << std::setw(11) << "service" << ' ' << std::setw(11) << "instance" << ' ' << std::setw(3) << "maj"
           << ' ' << std::setw(24) << "tcp" << ' ' << std::setw(24) << "udp" << '\n';
        for (const auto& o : offers) {
            std::ostringstream tcp, udp, svc, inst;
            svc << "0x" << std::hex << std::setw(4) << std::setfill('0') << o.service;
            inst << "0x" << std::hex << std::setw(4) << std::setfill('0') << o.instance;
            if (o.reliable_port != 0) tcp << o.reliable_address.to_string() << ':' << o.reliable_port;
            else tcp << "-";
            if (o.unreliable_port != 0) udp << o.unreliable_address.to_string() << ':' << o.unreliable_port;
            else udp << "-";
            os << "  " << std::left << std::setw(11) << svc.str() << ' ' << std::setw(11) << inst.str() << ' ' << std::setw(3) << +o.major
               << ' ' << std::setw(24) << tcp.str() << ' ' << std::setw(24) << udp.str() << '\n';
        }
    }
    os << "===\n";
    std::cout << os.str() << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "examples/sd_only_host/sd_only_host.json";

    // configuration_impl::load() initialises the logger as a side effect
    // when it parses the "logging" section, so we don't have to.
    auto configuration = std::make_shared<v3::cfg::configuration_impl>(config_path);
    if (!configuration->load("sd_only_host")) {
        std::cerr << "Failed to load configuration: " << config_path << '\n';
        return 1;
    }

    boost::asio::io_context io;
    auto host = std::make_shared<sd_only_host>(configuration, io);

    host->set_on_remote_offer([](v3::service_t service, v3::instance_t instance, v3::major_version_t major) {
        std::cout << "[on_remote_offer] hook: [0x" << std::hex << std::setw(4) << std::setfill('0') << service << '.' << std::setw(4)
                  << instance << std::dec << "] major=" << +major << '\n';
    });

    // service_discovery_impl takes a raw pointer to the host; the host
    // owns the SD impl below via shared_ptr, so the lifetime is correct.
    auto sd = std::make_shared<v3::sd::service_discovery_impl>(host.get(), configuration);
    host->set_service_discovery(sd.get());
    sd->init();
    sd->start();

    drive_sd_from_config(*configuration, *sd, *host, configuration->get_sd_ttl());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::thread io_thread{[&io]() { io.run(); }};

    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + std::chrono::seconds{60};
    auto next_dump = started_at + std::chrono::seconds{5};
    while (!g_stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        if (std::chrono::steady_clock::now() >= next_dump) {
            dump_routing_table(*host, started_at);
            next_dump += std::chrono::seconds{5};
        }
    }

    dump_routing_table(*host, started_at);

    sd->stop();
    work.reset();
    io.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    return 0;
}
