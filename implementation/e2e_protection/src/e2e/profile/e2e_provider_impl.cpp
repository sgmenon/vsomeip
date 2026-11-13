// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iomanip>
#include <sstream>

#include <vsomeip/internal/logger.hpp>
#include <vsomeip/plugins/builtin_plugins.hpp>

#include "../../../../e2e_protection/include/e2e/profile/e2e_provider_impl.hpp"

#include "../../../../e2e_protection/include/e2e/profile/profile01/checker.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile01/profile_01.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile01/protector.hpp"

#include "../../../../e2e_protection/include/e2e/profile/profile04/checker.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile04/profile_04.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile04/protector.hpp"

#include "../../../../e2e_protection/include/e2e/profile/profile05/checker.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile05/profile_05.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile05/protector.hpp"

#include "../../../../e2e_protection/include/e2e/profile/profile_custom/checker.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile_custom/profile_custom.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile_custom/protector.hpp"

#include "../../../../e2e_protection/include/e2e/profile/profile07/checker.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile07/profile_07.hpp"
#include "../../../../e2e_protection/include/e2e/profile/profile07/protector.hpp"

namespace {

constexpr vsomeip_v3::service_t k_sd_service = 0xFFFF;

template<typename value_t>
value_t read_value_from_config(const std::shared_ptr<vsomeip_v3::cfg::e2e>& _config, const std::string& _name,
                               value_t _default_value = value_t()) {

    if (_config && _config->custom_parameters.count(_name)) {

        std::stringstream its_converter;
        std::string its_value(_config->custom_parameters[_name]);

        if (its_value.size() > 1 && its_value[0] == '0' && its_value[1] == 'x') {
            its_converter << std::hex << its_value;
        } else {
            its_converter << std::dec << its_value;
        }

        value_t its_converted_value;
        its_converter >> its_converted_value;
        return its_converted_value;
    }

    return _default_value;
}

} // namespace

// See runtime_impl.cpp: this symbol is shared by every plugin, so it is
// omitted from the static build.
#ifndef VSOMEIP_STATIC_PLUGINS
VSOMEIP_PLUGIN(vsomeip_v3::e2e::e2e_provider_impl)
#endif

namespace vsomeip_v3 {

std::shared_ptr<plugin> create_e2e_plugin() {
    return e2e::e2e_provider_impl::get_plugin();
}

namespace e2e {

e2e_provider_impl::e2e_provider_impl() : plugin_impl("vsomeip e2e plugin", 1, plugin_type_e::E2E_PLUGIN) { }

e2e_provider_impl::~e2e_provider_impl() { }

bool e2e_provider_impl::add_configuration(std::shared_ptr<cfg::e2e> config) {
    if (!config ||config->service_id == k_sd_service) {
        return false;
    }

    if (config->profile == "CRC8" || config->profile == "P01") {
        process_e2e_profile<profile01::profile_config, profile01::profile_01_checker, profile01::protector>(config);
        return true;
    }

    if (config->profile == "CRC32" || config->profile == "CSTM") {
        process_e2e_profile<profile_custom::profile_config, profile_custom::profile_custom_checker, profile_custom::protector>(config);
        return true;
    }

    if (config->profile == "P04") {
        process_e2e_profile<profile04::profile_config, profile04::profile_04_checker, profile04::protector>(config);
        return true;
    }

    if (config->profile == "P05") {
        process_e2e_profile<profile05::profile_config, profile05::profile_05_checker, profile05::protector>(config);
        return true;
    }

    if (config->profile == "P07") {
        process_e2e_profile<profile07::profile_config, profile07::profile_07_checker, profile07::protector>(config);
        return true;
    }

    return false;
}

bool e2e_provider_impl::is_protected(e2exf::data_identifier_t id) const {
    return custom_protectors_.count(id) > 0;
}

bool e2e_provider_impl::is_checked(e2exf::data_identifier_t id) const {
    return custom_checkers_.count(id) > 0;
}

protect_result e2e_provider_impl::protect(e2exf::data_identifier_t id, buffer_view someip_header, buffer_view app_payload,
                                          instance_t instance) {
    auto protector = custom_protectors_.find(id);
    if (protector != custom_protectors_.end()) {
        return protector->second->protect(someip_header, app_payload, instance);
    }
    return protect_result{};
}

check_result e2e_provider_impl::check(e2exf::data_identifier_t id, buffer_view _message, instance_t _instance) {
    auto checker = custom_checkers_.find(id);
    if (checker == custom_checkers_.end()) {
        return check_result{};
    }

    std::size_t its_base = 0;
    if (const auto found_base = custom_bases_.find(id); found_base != custom_bases_.end()) {
        its_base = found_base->second;
    }
    if (_message.size() < its_base) {
        return check_result{};
    }

    return checker->second->check(_message.subspan(its_base), _instance);
}

template<>
vsomeip_v3::e2e::profile01::profile_config e2e_provider_impl::make_e2e_profile_config(const std::shared_ptr<cfg::e2e>& _config) {
    uint16_t data_id = read_value_from_config<uint16_t>(_config, "data_id");
    uint16_t crc_offset = read_value_from_config<uint16_t>(_config, "crc_offset");
    uint16_t data_length = read_value_from_config<uint16_t>(_config, "data_length");

    // counter field behind CRC8
    uint16_t counter_offset = read_value_from_config<uint16_t>(_config, "counter_offset", 8);

    // data id nibble behind 4 bit counter value
    uint16_t data_id_nibble_offset = read_value_from_config<uint16_t>(_config, "data_id_nibble_offset", 12);

    e2e::profile01::p01_data_id_mode data_id_mode =
            static_cast<e2e::profile01::p01_data_id_mode>(read_value_from_config<uint16_t>(_config, "data_id_mode"));

    return e2e::profile01::profile_config(crc_offset, data_id, data_id_mode, data_length, counter_offset, data_id_nibble_offset);
}

template<>
vsomeip_v3::e2e::profile04::profile_config e2e_provider_impl::make_e2e_profile_config(const std::shared_ptr<cfg::e2e>& _config) {

    uint32_t data_id = read_value_from_config<uint32_t>(_config, "data_id");

    size_t offset = read_value_from_config<size_t>(_config, "crc_offset");
    if (offset % 8)
        VSOMEIP_ERROR << "Offset in E2E P04 configuration must be multiple of 8"
                         " ("
                      << offset << ")";
    offset /= 8;

    size_t min_data_length = read_value_from_config<size_t>(_config, "min_data_length", 0);

    size_t max_data_length = read_value_from_config<size_t>(_config, "max_data_length", size_t(0xffff));

    uint16_t max_delta_counter = read_value_from_config<uint16_t>(_config, "max_delta_counter", uint16_t(0xffff));

    return e2e::profile04::profile_config(data_id, offset, min_data_length, max_data_length, max_delta_counter);
}

template<>
vsomeip_v3::e2e::profile05::profile_config e2e_provider_impl::make_e2e_profile_config(const std::shared_ptr<cfg::e2e>& _config) {

    uint32_t data_id = read_value_from_config<uint32_t>(_config, "data_id");
    uint16_t data_length = read_value_from_config<uint16_t>(_config, "data_length");

    size_t offset = read_value_from_config<size_t>(_config, "crc_offset");
    if (offset % 8)
        VSOMEIP_ERROR << "Offset in E2E P05 configuration must be multiple of 8"
                         " ("
                      << offset << ")";
    offset /= 8;

    uint16_t max_delta_counter = read_value_from_config<uint16_t>(_config, "max_delta_counter", uint16_t(0xffff));

    return e2e::profile05::profile_config(data_id, data_length, offset, max_delta_counter);
}

template<>
e2e::profile_custom::profile_config e2e_provider_impl::make_e2e_profile_config(const std::shared_ptr<cfg::e2e>& config) {
    uint16_t crc_offset = read_value_from_config<uint16_t>(config, "crc_offset");
    return e2e::profile_custom::profile_config(crc_offset);
}

template<>
vsomeip_v3::e2e::profile07::profile_config e2e_provider_impl::make_e2e_profile_config(const std::shared_ptr<cfg::e2e>& _config) {

    uint32_t data_id = read_value_from_config<uint32_t>(_config, "data_id");

    size_t offset = read_value_from_config<size_t>(_config, "crc_offset");
    if (offset % 8)
        VSOMEIP_ERROR << "Offset in E2E P07 configuration must be multiple of 8"
                         " ("
                      << offset << ")";
    offset /= 8;

    size_t min_data_length = read_value_from_config<size_t>(_config, "min_data_length", 0);

    size_t max_data_length = read_value_from_config<size_t>(_config, "max_data_length", size_t(0xffffffff));

    uint32_t max_delta_counter = read_value_from_config<uint32_t>(_config, "max_delta_counter", uint32_t(0xffffffff));

    return e2e::profile07::profile_config(data_id, offset, min_data_length, max_data_length, max_delta_counter);
}

} // namespace e2e
} // namespace vsomeip_v3
