# detmw —— 统一进程间通信中间件（v2：全 C++，Communicator + TransportInterface 隔离底层）
add_library(detmw STATIC
  src/detmw.cpp
  src/detmw_fastdds.cpp
  third_party/cJSON.c)
target_include_directories(detmw PUBLIC include)
target_include_directories(detmw PRIVATE third_party)
set_target_properties(detmw PROPERTIES C_STANDARD 11 CXX_STANDARD 17)

# Fast-DDS（系统 /usr/local 统计版；detmw 修复后统计版下正常，无需禁统计）
set(FASTDDS_PREFIX /usr/local)
target_include_directories(detmw PRIVATE ${FASTDDS_PREFIX}/include)
find_package(Threads REQUIRED)
target_link_libraries(detmw PUBLIC
  ${FASTDDS_PREFIX}/lib/libfastdds.so /usr/local/lib/libfastcdr.so
  dts_log Threads::Threads)

# 2 进程 demo：pub/sub 各跑一个实例（配置驱动 + 静态发现，产物 build/generated/detmw-demo）
set(DETMW_DEMO_CFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples/demo)
set(DETMW_DEMO_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw-demo)
add_custom_command(
  OUTPUT ${DETMW_DEMO_GEN_DIR}/demo-pub/data_routes.h ${DETMW_DEMO_GEN_DIR}/demo-pub/demo-pub.json
         ${DETMW_DEMO_GEN_DIR}/demo-pub/staticdiscovery.xml
         ${DETMW_DEMO_GEN_DIR}/demo-sub/data_routes.h ${DETMW_DEMO_GEN_DIR}/demo-sub/demo-sub.json
         ${DETMW_DEMO_GEN_DIR}/demo-sub/staticdiscovery.xml
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_detmw.py ${DETMW_DEMO_CFG_DIR} ${DETMW_DEMO_GEN_DIR}
  DEPENDS ${DETMW_DEMO_CFG_DIR}/pub.json ${DETMW_DEMO_CFG_DIR}/sub.json
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_detmw.py
  COMMENT "generating detmw demo configs"
  VERBATIM)
add_custom_target(detmw_demo_gen
  DEPENDS ${DETMW_DEMO_GEN_DIR}/demo-pub/demo-pub.json ${DETMW_DEMO_GEN_DIR}/demo-sub/demo-sub.json)

# 用法：detmw_demo pub build/generated/detmw-demo/demo-pub/demo-pub.json
#       detmw_demo sub build/generated/detmw-demo/demo-sub/demo-sub.json
add_executable(detmw_demo examples/demo.cpp)
target_link_libraries(detmw_demo PRIVATE detmw)
add_dependencies(detmw_demo detmw_demo_gen)
