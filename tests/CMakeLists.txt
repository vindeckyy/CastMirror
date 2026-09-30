cmake_minimum_required(VERSION 3.20)
project(castmirror_tests CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(GTest REQUIRED)
include(GoogleTest)

set(TEST_SOURCES
    test_main.cc
    test_state_machine.cc
    test_config.cc
    test_capability_model.cc
    test_offer_answer.cc
    test_crypto.cc
    test_rtp_rtcp.cc
    test_adaptive.cc
    test_encoders.cc
    test_cast_e2e.cc
    test_device_selection.cc
    test_logger.cc
    test_display_capture.cc
    test_frame_pacer.cc
    test_session_recovery.cc
    test_cast_transport.cc
    test_capture_source.cc
    test_portal_source.cc
    test_source_selection.cc
    test_device_auth.cc
    test_c_api.cc
    test_device_discovery.cc
    test_net_platform.cc
    test_gpu_processor.cc
    test_pixel_convert.cc
    test_parser_robustness.cc
    test_audio_capture.cc
)

if(WIN32)
    list(REMOVE_ITEM TEST_SOURCES test_portal_source.cc)
endif()

add_executable(castmirror_tests ${TEST_SOURCES})
target_link_libraries(castmirror_tests PRIVATE castcore GTest::gtest)

# gtest_discover_tests is the single source of truth for the registered suite:
# it enumerates one ctest case per TEST/TEST_P macro, which is what gives
# per-case pass/fail and what `ctest -N` lists. There is deliberately no
# aggregate `add_test(NAME CastMirrorAllTests COMMAND castmirror_tests)` here -
# registering the binary *and* its discovered cases makes every case run twice
# under ctest and reports the aggregate as if it were a test case of its own.
#
# No ENVIRONMENT property either: a hardcoded interpreter/runtime path such as
# C:/msys64/ucrt64/bin must not be baked into every platform's registration.
# The Windows CI job sets the UCRT64 runtime directory on PATH before invoking
# ctest instead.
gtest_discover_tests(castmirror_tests
    DISCOVERY_TIMEOUT 60
)

# Optional libFuzzer harnesses for the untrusted-input parsers. OFF by default so
# the normal build needs nothing beyond a C++20 compiler; see tests/fuzz/.
option(CASTMIRROR_BUILD_FUZZERS "Build libFuzzer harnesses (requires Clang)" OFF)
add_subdirectory(fuzz)
