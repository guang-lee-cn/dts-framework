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

# 跨进程冒烟（手动双进程，后续可加 ctest 包装）
add_executable(bucket_smoke
  test/bucket_smoke.cpp)
target_link_libraries(bucket_smoke PRIVATE bucket)

# 端到端 RTPS 测试：桶传输替代 SHM 跑完整 FastDDS 收发（pub/sub 双进程）
add_executable(bucket_rtps_test
  test/bucket_rtps_test.cpp)
target_link_libraries(bucket_rtps_test PRIVATE bucket)
