# Copyright (C) 2026 GM Global Technology Operations LLC.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set(configuration_files
    multicast_isolation_test_service_a.json
    multicast_isolation_test_service_b.json
    multicast_isolation_test_client_a.json
    multicast_isolation_test_client_b.json
    multicast_isolation_test_service_starter.sh
    multicast_isolation_test_client_starter.sh
)
configure_files("${configuration_files}")

add_executable(multicast_isolation_test_service
    multicast_isolation_test_service.cpp
)

add_executable(multicast_isolation_test_client
    multicast_isolation_test_client.cpp
)

set(executables
    multicast_isolation_test_service
    multicast_isolation_test_client
)
targets_link_default_libraries("${executables}")
targets_add_default_dependencies("${executables}")

add_custom_test(
    NAME multicast_isolation_test
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multicast_isolation_test_service_starter.sh
    TIMEOUT 90
)
