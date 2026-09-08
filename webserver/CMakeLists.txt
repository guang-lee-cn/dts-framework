# web-server 进程：dts 上报 → Kafka 沉淀 → 浏览器观测
# 纯 detmw 对端（不链 bootstrap）+ librdkafka 薄壳 + 自研 HTTP/SSE（零第三方 HTTP 库）
set(RDK_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/librdkafka)
if(NOT EXISTS ${RDK_ROOT}/usr/include/librdkafka/rdkafka.h)
  message(FATAL_ERROR
    "librdkafka 未解包：先执行 thirdparty/librdkafka/fetch.sh（apt download 免 root 解包到本地）")
endif()

add_executable(web_server
  src/main.cpp
  src/kafka_client.cpp
  src/http_sse.cpp)
target_include_directories(web_server PRIVATE
  src
  ${CMAKE_SOURCE_DIR}/infrastructure/include
  ${CMAKE_SOURCE_DIR}/contexts/data/domain/include)   # data_model.h：上报帧结构（纯头）
# 第三方头（librdkafka）：SYSTEM 隔离，头内告警不进 -Werror 视野
target_include_directories(web_server SYSTEM PRIVATE ${RDK_ROOT}/usr/include)
target_link_libraries(web_server PRIVATE
  detmw infrastructure dts_log Threads::Threads
  ${RDK_ROOT}/usr/lib/x86_64-linux-gnu/librdkafka.so)
# 运行期在 thirdparty 内找 librdkafka.so（开发机就地构建，无需系统安装）
set_target_properties(web_server PROPERTIES
  BUILD_RPATH "${RDK_ROOT}/usr/lib/x86_64-linux-gnu")

# 观测页拷到可执行目录（运行参数默认 ./web/index.html，工作目录 = 目标目录）
add_custom_command(TARGET web_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:web_server>/web)
