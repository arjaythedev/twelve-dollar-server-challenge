tds_add_executable(transport_tests transport.cpp)
target_link_libraries(transport_tests PRIVATE transport api sqlite yyjson OpenSSL::Crypto)
target_link_options(transport_tests PRIVATE -Wl,--wrap=sqlite3_step)

tds_add_executable(io_lifetime_tests io_lifetimes.cpp)
target_link_libraries(io_lifetime_tests PRIVATE transport uring llhttp_static yyjson)

tds_add_executable(checkpoint_tests checkpoint.cpp)
target_link_libraries(checkpoint_tests PRIVATE api sqlite)
target_compile_definitions(checkpoint_tests PRIVATE TDS_TELEMETRY_LEVEL=${telemetry_level})
target_link_options(checkpoint_tests PRIVATE
    -Wl,--wrap=sqlite3_open_v2 -Wl,--wrap=sqlite3_close -Wl,--wrap=sqlite3_wal_checkpoint_v2
    -Wl,--wrap=posix_fadvise64)

tds_add_executable(group_commit_tests group_commit.cpp)
target_link_libraries(group_commit_tests PRIVATE api sqlite)
target_link_options(group_commit_tests PRIVATE
    -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_prepare_v3 -Wl,--wrap=tds_btree_feed_next
    -Wl,--wrap=tds_btree_post_exists)

tds_add_executable(post_writer_tests post_writer.cpp)
target_link_libraries(post_writer_tests PRIVATE api)

tds_add_executable(application_tests application.cpp)
target_link_libraries(application_tests PRIVATE api transport OpenSSL::Crypto sqlite yyjson)

tds_add_executable(btree_tests btree.cpp)
target_include_directories(btree_tests PRIVATE "${PROJECT_SOURCE_DIR}/server")
target_link_libraries(btree_tests PRIVATE sqlite)

tds_add_executable(sync_tests sync.cpp "${PROJECT_SOURCE_DIR}/server/sync_timing.cpp"
    "${PROJECT_SOURCE_DIR}/server/diagnostics/database.cpp")
target_include_directories(sync_tests PRIVATE "${PROJECT_SOURCE_DIR}/server")
target_compile_definitions(sync_tests PRIVATE TDS_TELEMETRY_LEVEL=2)
target_link_libraries(sync_tests PRIVATE sqlite telemetry_output Threads::Threads)
target_link_options(sync_tests PRIVATE ${sync_wrap_options})

tds_add_executable(telemetry_tests telemetry.cpp)
target_link_libraries(telemetry_tests PRIVATE telemetry_output yyjson)

tds_add_executable(load_tests load.cpp)
target_link_libraries(load_tests PRIVATE load_core)
target_link_options(load_tests PRIVATE -Wl,--wrap=epoll_ctl -Wl,--wrap=send)
tds_add_executable(latency_tests latencies.cpp)
target_link_libraries(latency_tests PRIVATE load_core)

tds_add_executable(load_activation_tests load_activation.cpp)
target_link_libraries(load_activation_tests PRIVATE load_core)
target_link_options(load_activation_tests PRIVATE
    -Wl,--wrap=epoll_wait -Wl,--wrap=_ZNSt6chrono3_V212steady_clock3nowEv)

add_test(NAME io_uring_operations COMMAND server --self-test)
add_test(NAME transport_lifetimes COMMAND transport_tests)
add_test(NAME io_lifetime_contract COMMAND io_lifetime_tests)
add_test(NAME checkpoint_lifetimes COMMAND checkpoint_tests)
add_test(NAME group_commit_contract COMMAND group_commit_tests)
add_test(NAME post_writer_bytes COMMAND post_writer_tests)
add_test(NAME application_contract COMMAND application_tests)
add_test(NAME btree_feed COMMAND btree_tests)
add_test(NAME sync_timing_contract COMMAND sync_tests)
add_test(NAME telemetry_contract COMMAND telemetry_tests)
add_test(NAME load_generator_conformance COMMAND load_tests)
add_test(NAME load_generator_latencies COMMAND latency_tests)
add_test(NAME load_generator_activation COMMAND load_activation_tests)

set_tests_properties(io_uring_operations transport_lifetimes io_lifetime_contract
    PROPERTIES LABELS "native;transport;integration")
set_tests_properties(checkpoint_lifetimes group_commit_contract application_contract btree_feed
    PROPERTIES LABELS "native;database;integration")
set_tests_properties(post_writer_bytes PROPERTIES LABELS "native;unit")
set_tests_properties(sync_timing_contract PROPERTIES LABELS "native;database;unit")
set_tests_properties(telemetry_contract PROPERTIES LABELS "native;unit")
set_tests_properties(load_generator_conformance PROPERTIES LABELS "native;generator;integration")
set_tests_properties(load_generator_latencies PROPERTIES LABELS "native;generator;unit")
set_tests_properties(load_generator_activation PROPERTIES LABELS "native;generator;unit")
set_tests_properties(checkpoint_lifetimes group_commit_contract io_lifetime_contract
    PROPERTIES TIMEOUT 15)

get_property(native_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${native_tests} PROPERTIES ENVIRONMENT "TDS_TELEMETRY_PATH=/dev/null")
