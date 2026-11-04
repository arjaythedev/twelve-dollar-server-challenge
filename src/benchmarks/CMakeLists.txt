tds_add_executable(group_commit_probe EXCLUDE_FROM_ALL group_commit.cpp)
target_link_libraries(group_commit_probe PRIVATE sqlite Threads::Threads)

tds_add_executable(wal_vfs_probe EXCLUDE_FROM_ALL wal_vfs.cpp)
target_include_directories(wal_vfs_probe PRIVATE "${PROJECT_SOURCE_DIR}/server")
target_link_libraries(wal_vfs_probe PRIVATE sqlite)

tds_add_executable(btree_post_probe EXCLUDE_FROM_ALL btree_post.cpp)
target_link_libraries(btree_post_probe PRIVATE api sqlite)

# The control replaces only Store's object file; production has no runtime backend switch
tds_add_executable(btree_post_sql_probe EXCLUDE_FROM_ALL btree_post.cpp
    "${PROJECT_SOURCE_DIR}/server/storage/database.cpp")
target_compile_definitions(btree_post_sql_probe PRIVATE TDS_POST_SQL_CONTROL=1
    TDS_TELEMETRY_LEVEL=${telemetry_level})
target_link_libraries(btree_post_sql_probe PRIVATE api sqlite)

tds_add_executable(response_reuse_probe EXCLUDE_FROM_ALL response_reuse.cpp)
target_link_libraries(response_reuse_probe PRIVATE api transport sqlite)
target_compile_definitions(response_reuse_probe PRIVATE TDS_RESPONSE_REUSE=1)

tds_add_executable(retry_receive_probe EXCLUDE_FROM_ALL retry_receive.cpp)
target_link_libraries(retry_receive_probe PRIVATE transport uring)

if(TDS_TELEMETRY STREQUAL "profile")
    tds_add_executable(telemetry_probe EXCLUDE_FROM_ALL telemetry.cpp)
    target_link_libraries(telemetry_probe PRIVATE transport api sqlite)

    tds_add_executable(history_probe EXCLUDE_FROM_ALL history.cpp)
    target_link_libraries(history_probe PRIVATE transport api sqlite)

    tds_add_executable(wal_advice_probe EXCLUDE_FROM_ALL wal_advice.cpp)
    target_link_libraries(wal_advice_probe PRIVATE api sqlite)
    target_compile_definitions(wal_advice_probe PRIVATE TDS_TELEMETRY_LEVEL=2)
    target_link_options(wal_advice_probe PRIVATE -Wl,--wrap=posix_fadvise64)
endif()
