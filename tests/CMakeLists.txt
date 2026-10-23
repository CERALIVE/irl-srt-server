add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${PROJECT_SOURCE_DIR}/lib/doctest)

add_executable(sls_tests
    test_main.cpp
    test_sls_sid.cpp
    test_recycle_array.cpp
    test_auth_reject_cache.cpp
    test_bitrate_limit.cpp
    test_idle.cpp
    test_push_url.cpp
    test_ip_acl.cpp
    test_ts_parser.cpp
    test_timecode.cpp
    test_map_data.cpp
    test_log_rate_limiter.cpp
    test_player_registry.cpp
)
target_link_libraries(sls_tests PRIVATE sls_core doctest srt)
add_test(NAME unit COMMAND sls_tests)

# Live sockets stay outside the pure unit harness.
add_executable(sls_pusher_tests test_main.cpp test_pusher_connect.cpp)
target_link_libraries(sls_pusher_tests PRIVATE sls_core doctest srt)
add_test(NAME pusher_connect COMMAND sls_pusher_tests)
set_tests_properties(pusher_connect PROPERTIES TIMEOUT 30)
