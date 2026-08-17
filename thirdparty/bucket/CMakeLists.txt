# 自研桶传输：共享内存环（传输核心）+ TransportInterface 外壳
set(FASTDDS_PREFIX /usr/local)

add_library(bucket STATIC
  src/BucketSegment.cpp
  src/BucketTransport.cpp
  src/BucketTransportDescriptor.cpp)
target_include_directories(bucket PUBLIC include)
target_include_directories(bucket PRIVATE ${FASTDDS_PREFIX}/include)
find_package(Threads REQUIRED)
target_link_libraries(bucket PUBLIC Threads::Threads)
target_link_libraries(bucket PUBLIC
  ${FASTDDS_PREFIX}/lib/libfastdds.so ${FASTDDS_PREFIX}/lib/libfastcdr.so)

# 跨进程冒烟：1 reader + 2 writers（多写者互斥/ring 回绕/seq 全集）
add_executable(bucket_smoke
  test/bucket_smoke.cpp)
target_link_libraries(bucket_smoke PRIVATE bucket)
add_test(NAME bucket_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_bucket_smoke.sh
          $<TARGET_FILE:bucket_smoke>)
# 无 POSIX shm 的环境（CI 沙箱/容器）返回 77 → ctest 标 Skipped（不失败）
set_tests_properties(bucket_smoke PROPERTIES SKIP_RETURN_CODE 77)

# 端到端 RTPS 测试：桶传输替代 SHM 跑完整 FastDDS 收发（pub/sub 双进程）
add_executable(bucket_rtps_test
  test/bucket_rtps_test.cpp)
target_link_libraries(bucket_rtps_test PRIVATE bucket)
add_test(NAME bucket_rtps
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_bucket_rtps.sh
          $<TARGET_FILE:bucket_rtps_test> 200 32768)
set_tests_properties(bucket_rtps PROPERTIES SKIP_RETURN_CODE 77)
