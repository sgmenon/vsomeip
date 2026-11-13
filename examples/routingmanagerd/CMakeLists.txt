# Copyright (C) 2015-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Daemon
if (VSOMEIP_STATIC_PLUGINS)
    add_executable(routingmanagerd routingmanagerd.cpp
        ${PROJECT_SOURCE_DIR}/implementation/plugin/registration/register_plugins.cpp)
    # The list is defined in the executable. Export it so libvsomeip can call it.
    target_link_libraries(routingmanagerd ${VSOMEIP_NAME}-sd ${VSOMEIP_NAME}-e2e ${VSOMEIP_NAME}-cfg -Wl,-export-dynamic)
else ()
    add_executable(routingmanagerd routingmanagerd.cpp)
endif ()
target_link_libraries(routingmanagerd ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY} ${DLT_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_libraries(routingmanagerd stdc++)
endif()
if(${CMAKE_SYSTEM_NAME} MATCHES "QNX")
    target_link_libraries(routingmanagerd socket)
endif()
add_dependencies(routingmanagerd ${VSOMEIP_NAME})

option(VSOMEIP_INSTALL_ROUTINGMANAGERD "Whether or not to install the routing manager daemon.")

if (VSOMEIP_INSTALL_ROUTINGMANAGERD)
    install (
        TARGETS routingmanagerd
        RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin
    )
endif()



###################################################################################################
