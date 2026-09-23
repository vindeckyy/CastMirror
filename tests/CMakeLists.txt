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
    test_http_fallback.cc
)

if(WIN32)
    list(REMOVE_ITEM TEST_SOURCES test_portal_source.cc)
endif()

add_executable(castmirror_tests ${TEST_SOURCES})
target_link_libraries(castmirror_tests PRIVATE castcore GTest::gtest)

gtest_discover_tests(castmirror_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES ENVIRONMENT "PATH=C:/msys64/ucrt64/bin;$ENV{PATH}"
)
add_test(NAME CastMirrorAllTests COMMAND castmirror_tests)
