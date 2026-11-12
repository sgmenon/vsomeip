# Copyright (C) 2026 GM GLOBAL TECHNOLOGY OPERATIONS LLC ALL RIGHTS RESERVED.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

project(wait_route_netlink_tests LANGUAGES CXX)

set(configuration_files
    wait_route_netlink_netsetup.sh
    wait_route_netlink_wait_false.json
    wait_route_netlink_wait_true.json
    wait_route_netlink_wait_false_starter.sh
    wait_route_netlink_wait_true_starter.sh
)
configure_files("${configuration_files}")

foreach(wait_route_netlink_script
    wait_route_netlink_netsetup.sh
    wait_route_netlink_wait_false_starter.sh
    wait_route_netlink_wait_true_starter.sh
)
    # configure_file writes these as 0644. file(CHMOD) sets the mode; it does
    # not take a chmod-style "u+x".
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/${wait_route_netlink_script}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)
endforeach()

add_custom_target(wait_route_netlink_tests_scripts ALL)
add_dependencies(build_network_tests wait_route_netlink_tests_scripts)

add_custom_test(
    NAME wait_route_netlink_wait_false_test
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/wait_route_netlink_wait_false_starter.sh
    TIMEOUT 45
)

add_custom_test(
    NAME wait_route_netlink_wait_true_test
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/wait_route_netlink_wait_true_starter.sh
    TIMEOUT 60
)
