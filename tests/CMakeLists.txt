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
    test_log_redact.cpp
    test_log_pipeline.cpp
    test_generation_set.cpp
    test_dispatch.cpp
    test_conf_release.cpp
    test_worker_retire.cpp
    test_thread_races.cpp
    test_player_key_conf.cpp
)
target_link_libraries(sls_tests PRIVATE sls_core doctest srt)
add_test(NAME unit COMMAND sls_tests)

# Live sockets stay outside the pure unit harness.
add_executable(sls_pusher_tests test_main.cpp test_pusher_connect.cpp)
target_link_libraries(sls_pusher_tests PRIVATE sls_core doctest srt)
add_test(NAME pusher_connect COMMAND sls_pusher_tests)
set_tests_properties(pusher_connect PROPERTIES TIMEOUT 30)

# SRT caller driven by the e2e scripts: readers sharing one UDP port, and
# handshake floods. Test tooling only; never installed.
add_executable(sls_e2e_reader e2e/sls_e2e_reader.cpp)
target_link_libraries(sls_e2e_reader PRIVATE srt Threads::Threads)
