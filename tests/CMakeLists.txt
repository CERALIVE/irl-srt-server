add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${PROJECT_SOURCE_DIR}/lib/doctest)

add_executable(sls_tests
    test_main.cpp
    test_listener_profile_map.cpp
    test_sls_sid.cpp
    test_recycle_array.cpp
    test_auth_reject_cache.cpp
    test_bitrate_limit.cpp
    test_audio_gap_filler.cpp
    test_map_data.cpp
    test_conn_rate_limiter.cpp
    test_push_url.cpp
    test_ts_parser.cpp
    test_ip_acl.cpp
    test_srt_profiles.cpp
    test_conf_parse.cpp
)
target_link_libraries(sls_tests PRIVATE sls_core doctest srt)
# test_conf_parse.cpp loads committed conf fixtures (src/sls.conf,
# tests/e2e/sls-loopback.conf, tests/fixtures/conf-multiport.conf) through the
# real loader; hand it the source-tree root so it can resolve those paths
# regardless of the build/run directory.
target_compile_definitions(sls_tests PRIVATE SLS_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND sls_tests)

# Named subset gate so the listener-profile mapping checks can run in isolation
# (ctest -R listener_profile). doctest's --source-file filter selects only the
# cases defined in that file; `unit` still runs everything.
add_test(NAME listener_profile COMMAND sls_tests --source-file=*test_listener_profile_map.cpp)
