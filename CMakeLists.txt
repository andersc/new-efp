cmake_minimum_required(VERSION 3.14)

# Fix for older doctest CMakeLists
if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

project(efp VERSION 1.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Options
option(EFP_BUILD_TESTS "Build unit tests" ON)
option(EFP_BUILD_EXAMPLES "Build examples" ON)
option(EFP_BUILD_NETWORK_EXAMPLES "Build examples using net-tools UDP transport" OFF)
option(EFP_BUILD_C_API "Build C API library" ON)

# Test timeout for stress tests (default 5 minutes)
set(EFP_TEST_TIMEOUT 300 CACHE STRING "Timeout in seconds for stress tests")

# Thread support
find_package(Threads REQUIRED)

# Header-only library target
add_library(efp INTERFACE)
target_include_directories(efp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(efp INTERFACE Threads::Threads)

# Alias for cleaner usage
add_library(efp::efp ALIAS efp)

# C API library
if(EFP_BUILD_C_API)
    add_library(efp_c_api STATIC efp_c_api.cpp)
    target_link_libraries(efp_c_api PUBLIC efp)
    target_include_directories(efp_c_api PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
    add_library(efp::c_api ALIAS efp_c_api)
endif()

# Tests
if(EFP_BUILD_TESTS)
    enable_testing()

    # Fetch doctest
    include(FetchContent)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    FetchContent_MakeAvailable(doctest)

    # Test executable - comprehensive test suite
    add_executable(efp_tests
        tests/test_sender.cpp
        tests/test_receiver.cpp
        tests/test_integration.cpp
        tests/test_edge_cases.cpp
        tests/test_basic.cpp
        tests/test_stress.cpp
        tests/test_fragment_ordering.cpp
        tests/test_packet_loss.cpp
        tests/test_lifecycle.cpp
        tests/test_embedded_data.cpp
        tests/test_bundle.cpp
        tests/test_nack.cpp
        tests/test_bandwidth_manager.cpp
    )

    target_link_libraries(efp_tests PRIVATE
        efp
        doctest::doctest
        Threads::Threads
        $<$<BOOL:${EFP_BUILD_C_API}>:efp_c_api>
    )

    target_include_directories(efp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # C API tests
    if(EFP_BUILD_C_API)
        add_executable(efp_c_tests
            tests/test_c_api.cpp
        )
        target_link_libraries(efp_c_tests PRIVATE
            efp_c_api
            Threads::Threads
        )
    endif()

    # CTest integration
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(efp_tests
        PROPERTIES TIMEOUT ${EFP_TEST_TIMEOUT}
    )

    if(EFP_BUILD_C_API)
        add_test(NAME efp_c_api_tests COMMAND efp_c_tests)
        set_tests_properties(efp_c_api_tests PROPERTIES TIMEOUT ${EFP_TEST_TIMEOUT})
    endif()
endif()

# Examples
if(EFP_BUILD_EXAMPLES)
    add_executable(efp_example examples/basic_usage.cpp)
    target_link_libraries(efp_example PRIVATE efp)

    # C API example
    if(EFP_BUILD_C_API)
        add_executable(efp_c_example examples/c_api_example.cpp)
        target_link_libraries(efp_c_example PRIVATE efp_c_api)
    endif()
endif()

# Optional real-network example. Fetch only net-tools sources needed by NetworkIF,
# avoiding its tests and demo targets when EFP is consumed as a subproject.
if(EFP_BUILD_NETWORK_EXAMPLES)
    include(FetchContent)
    FetchContent_Declare(
        net_tools
        GIT_REPOSITORY https://github.com/andersc/net-tools.git
        # net-tools v1.0.1
        GIT_TAG 06df4df0244ca8704be5ef5d50a13a8a4871c087
        GIT_SHALLOW FALSE
    )
    FetchContent_GetProperties(net_tools)
    if(NOT net_tools_POPULATED)
        # Direct population keeps net-tools' own demos/tests out of EFP's build.
        # CMP0169 is needed only by newer CMake; older supported versions do not know it.
        if(POLICY CMP0169)
            cmake_policy(PUSH)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(net_tools)
        if(POLICY CMP0169)
            cmake_policy(POP)
        endif()
    endif()

    add_library(efp_networkif STATIC
        ${net_tools_SOURCE_DIR}/NetworkIF.cpp
        ${net_tools_SOURCE_DIR}/NetworkAddress.cpp
        ${net_tools_SOURCE_DIR}/NetworkRuntime.cpp
        ${net_tools_SOURCE_DIR}/SocketPlatform.cpp
        ${net_tools_SOURCE_DIR}/NameResolver.cpp
        ${net_tools_SOURCE_DIR}/NetworkInterfaces.cpp
        ${net_tools_SOURCE_DIR}/TcpProbe.cpp
        ${net_tools_SOURCE_DIR}/RouteInspector.cpp
    )
    target_include_directories(efp_networkif PUBLIC ${net_tools_SOURCE_DIR})
    target_link_libraries(efp_networkif PUBLIC Threads::Threads)
    if(WIN32)
        target_link_libraries(efp_networkif PUBLIC ws2_32 iphlpapi)
    endif()

    add_executable(efp_udp_loopback examples/udp_loopback.cpp)
    target_link_libraries(efp_udp_loopback PRIVATE efp efp_networkif)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS efp
    EXPORT efpTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    efp.h
    efp_internal.h
    efp_media_types.h
    efp_c_api.h
    bandwidth_manager.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/efp
)

install(EXPORT efpTargets
    FILE efpTargets.cmake
    NAMESPACE efp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/efp
)

# Package configuration
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/efpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/efpConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/efp
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/efpConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/efpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/efpConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/efp
)

