# 质量维护目录：集成测试 + 跨进程测试链路 + 静态检查

# 单进程上电测试：Run(cfg) + mock 对端（stub），验证启动/优雅退出
# run.cpp 含进程生成路由头，用 cpf 生成目录编译
add_executable(dts_integration_test
  test/integration_test.cpp
  test/scheduler_mock.cpp
  test/spa_mock.cpp
  test/kafka_mock.cpp
  ${CMAKE_SOURCE_DIR}/bootstrap/src/run.cpp)
target_include_directories(dts_integration_test PRIVATE
  test
  ${CMAKE_SOURCE_DIR}/contexts/data/domain/include   # spa_mock 用 data_ids.h（raw 布局）
  ${DETMW_GEN_DIR}/cpf-dts)
target_compile_definitions(dts_integration_test PRIVATE
  DTS_TEST_CFG="${DETMW_GEN_DIR}/cpf-dts/cpf-dts.json")
target_link_libraries(dts_integration_test PRIVATE
  bootstrap task data log infrastructure dts_agent detsched Threads::Threads)
add_dependencies(dts_integration_test detmw_gen)
add_test(NAME integration COMMAND dts_integration_test)

# ---- 跨进程测试链路：nfoam + spa + dts_test（test 提供配置，走测试链路）----
set(DTS_TEST_CFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test/config/dts)
set(DTS_TEST_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw-test)

set(DTS_TEST_GEN_OUTPUTS
  ${DTS_TEST_GEN_DIR}/dts-test/task_routes.h
  ${DTS_TEST_GEN_DIR}/dts-test/data_routes.h
  ${DTS_TEST_GEN_DIR}/dts-test/log_routes.h
  ${DTS_TEST_GEN_DIR}/dts-test/control_routes.h
  ${DTS_TEST_GEN_DIR}/dts-test/dts-test.json
  ${DTS_TEST_GEN_DIR}/dts-test/staticdiscovery.xml
  ${DTS_TEST_GEN_DIR}/nfoam/task_routes.h
  ${DTS_TEST_GEN_DIR}/nfoam/data_routes.h
  ${DTS_TEST_GEN_DIR}/nfoam/nfoam.json
  ${DTS_TEST_GEN_DIR}/nfoam/staticdiscovery.xml
  ${DTS_TEST_GEN_DIR}/spa/data_routes.h
  ${DTS_TEST_GEN_DIR}/spa/spa.json
  ${DTS_TEST_GEN_DIR}/spa/staticdiscovery.xml)
add_custom_command(
  OUTPUT ${DTS_TEST_GEN_OUTPUTS}
  COMMAND python3 ${DETMW_GEN_SCRIPT} ${DTS_TEST_CFG_DIR} ${DTS_TEST_GEN_DIR}
  DEPENDS ${DTS_TEST_CFG_DIR}/dts.json ${DTS_TEST_CFG_DIR}/nfoam.json ${DTS_TEST_CFG_DIR}/spa.json
          ${DETMW_GEN_SCRIPT}
  COMMENT "generating detmw test configs"
  VERBATIM)
add_custom_target(detmw_test_gen
  DEPENDS ${DTS_TEST_GEN_OUTPUTS})

# 测试 dts 进程：bootstrap 源码 + 测试路由头（同产品 cpf_dts 模式）
add_executable(dts_test
  ${DTS_STARTUP_SOURCES})
target_include_directories(dts_test PRIVATE ${DTS_TEST_GEN_DIR}/dts-test)
target_link_libraries(dts_test PRIVATE
  bootstrap task data log infrastructure dts_agent detsched detmw Threads::Threads)
add_dependencies(dts_test detmw_test_gen)

# 对端测试进程：nfoam（task 往返）、spa（数据通路）
add_executable(nfoam_test test/nfoam_test.cpp)
target_include_directories(nfoam_test PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(nfoam_test PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(nfoam_test detmw_test_gen)

add_executable(spa_test test/spa_test.cpp)
target_include_directories(spa_test PRIVATE
  ${CMAKE_SOURCE_DIR}/infrastructure/include
  ${CMAKE_SOURCE_DIR}/contexts/data/domain/include)   # data_ids.h（raw 布局与 data 共享）
target_link_libraries(spa_test PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(spa_test detmw_test_gen)

add_test(NAME cross_process
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_cross_test.sh
          $<TARGET_FILE:dts_test> $<TARGET_FILE:nfoam_test> $<TARGET_FILE:spa_test>
          ${DTS_TEST_GEN_DIR})
# 受限环境（无 /dev/shm、UDP 大包不可达）下行：nfoam 配置载荷调小（32K S 级由 s_level_perf 覆盖）
set_tests_properties(cross_process PROPERTIES ENVIRONMENT "DTS_CFG_LEN=1024")

# ---- pubsub 能力压测：perf-gen 连发 32k -> dts(task->data->上报) -> perf-sub 量测 ----
set(PERF_CFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test/config/perf)
set(PERF_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw-perf)

set(PERF_GEN_OUTPUTS
  ${PERF_GEN_DIR}/perf-dts/task_routes.h
  ${PERF_GEN_DIR}/perf-dts/data_routes.h
  ${PERF_GEN_DIR}/perf-dts/log_routes.h
  ${PERF_GEN_DIR}/perf-dts/perf-dts.json
  ${PERF_GEN_DIR}/perf-dts/staticdiscovery.xml
  ${PERF_GEN_DIR}/perf-gen/perf-gen.json
  ${PERF_GEN_DIR}/perf-gen/staticdiscovery.xml
  ${PERF_GEN_DIR}/perf-sub/perf-sub.json
  ${PERF_GEN_DIR}/perf-sub/staticdiscovery.xml)
add_custom_command(
  OUTPUT ${PERF_GEN_OUTPUTS}
  COMMAND python3 ${DETMW_GEN_SCRIPT} ${PERF_CFG_DIR} ${PERF_GEN_DIR}
  DEPENDS ${PERF_CFG_DIR}/perf-dts.json ${PERF_CFG_DIR}/perf-gen.json ${PERF_CFG_DIR}/perf-sub.json
          ${DETMW_GEN_SCRIPT}
  COMMENT "generating detmw perf configs"
  VERBATIM)
add_custom_target(detmw_perf_gen
  DEPENDS ${PERF_GEN_DIR}/perf-dts/perf-dts.json ${PERF_GEN_DIR}/perf-gen/perf-gen.json
          ${PERF_GEN_DIR}/perf-sub/perf-sub.json)

add_executable(perf_gen test/perf_gen.cpp)
target_include_directories(perf_gen PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(perf_gen PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(perf_gen detmw_perf_gen)

add_executable(perf_sub test/perf_sub.cpp)
target_include_directories(perf_sub PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(perf_sub PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(perf_sub detmw_perf_gen)

# task 双向收发基准（A）：nfoam 发 32K JSON 配置 -> task 简析+响应 -> nfoam 量测往返
add_executable(bench_nfoam test/bench_nfoam.cpp)
target_include_directories(bench_nfoam PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(bench_nfoam PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(bench_nfoam detmw_perf_gen)

# 注：原生线程 API 拦截脚本由 detsched 库提供（thirdparty/detsched/tools/check_no_native_thread.sh），
#     由各业务工程自行集成到自身 CI，线程库不代集成。

# ---- DDS 调优链路：tune_dts + spa_mock_proc（握手+上报rawData）+ webserver_mock（收上报落CSV）----
set(TUNE_CFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test/config/tune)
set(TUNE_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw-tune)

set(TUNE_GEN_OUTPUTS
  ${TUNE_GEN_DIR}/tune-dts/task_routes.h
  ${TUNE_GEN_DIR}/tune-dts/data_routes.h
  ${TUNE_GEN_DIR}/tune-dts/log_routes.h
  ${TUNE_GEN_DIR}/tune-dts/control_routes.h
  ${TUNE_GEN_DIR}/tune-dts/tune-dts.json
  ${TUNE_GEN_DIR}/tune-dts/staticdiscovery.xml
  ${TUNE_GEN_DIR}/tune-spa/tune-spa.json
  ${TUNE_GEN_DIR}/tune-spa/staticdiscovery.xml
  ${TUNE_GEN_DIR}/tune-web/tune-web.json
  ${TUNE_GEN_DIR}/tune-web/staticdiscovery.xml
  ${TUNE_GEN_DIR}/tune-web2/tune-web2.json
  ${TUNE_GEN_DIR}/tune-web2/staticdiscovery.xml)
add_custom_command(
  OUTPUT ${TUNE_GEN_OUTPUTS}
  COMMAND python3 ${DETMW_GEN_SCRIPT} ${TUNE_CFG_DIR} ${TUNE_GEN_DIR}
  DEPENDS ${TUNE_CFG_DIR}/tune-dts.json ${TUNE_CFG_DIR}/tune-spa.json ${TUNE_CFG_DIR}/tune-web.json
          ${DETMW_GEN_SCRIPT}
  COMMENT "generating detmw tune configs"
  VERBATIM)
add_custom_target(detmw_tune_gen
  DEPENDS ${TUNE_GEN_DIR}/tune-dts/tune-dts.json
          ${TUNE_GEN_DIR}/tune-spa/tune-spa.json
          ${TUNE_GEN_DIR}/tune-web/tune-web.json)

# 中段 dts 进程（复用 bootstrap 启动源码，同 dts_test 模式）
add_executable(tune_dts ${DTS_STARTUP_SOURCES})
target_include_directories(tune_dts PRIVATE ${TUNE_GEN_DIR}/tune-dts)
target_link_libraries(tune_dts PRIVATE
  bootstrap task data log infrastructure dts_agent detsched detmw Threads::Threads)
add_dependencies(tune_dts detmw_tune_gen)

# mock 进程（纯 detmw pub/sub，不链 bootstrap，不消费 routes.h）
add_executable(spa_mock_proc test/spa_mock_proc.cpp)
target_include_directories(spa_mock_proc PRIVATE
  ${CMAKE_SOURCE_DIR}/infrastructure/include
  ${CMAKE_SOURCE_DIR}/contexts/data/domain/include)
target_link_libraries(spa_mock_proc PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(spa_mock_proc detmw_tune_gen)

add_executable(webserver_mock test/webserver_mock.cpp)
target_include_directories(webserver_mock PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(webserver_mock PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(webserver_mock detmw_tune_gen)

# S 级回归（真实数据链路）：1条/s × 32K raw，spa 握手+上报 -> dts(data 切分+合并上报)
# -> web 计数，断言端到端零丢包（替代旧 perf 链 task 模式：msg2 已改进程内直通，外部不可观测；
# 无 SHM 的受限环境可 DTS_RAW_LEN 调小验证链路）
add_test(NAME s_level_perf
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_slevel.sh
          $<TARGET_FILE:tune_dts> $<TARGET_FILE:spa_mock_proc> $<TARGET_FILE:webserver_mock>
          ${TUNE_GEN_DIR})

# ---- 单元自测（无外部依赖，纯逻辑断言；ctest 快速回归）----
add_executable(unit_msg_table unit/msg_table_test.cpp)
target_include_directories(unit_msg_table PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(unit_msg_table PRIVATE Threads::Threads)
add_test(NAME unit_msg_table COMMAND unit_msg_table)

add_executable(unit_data_domain unit/data_domain_test.cpp)
target_link_libraries(unit_data_domain PRIVATE data infrastructure dts_log Threads::Threads)
add_test(NAME unit_data_domain COMMAND unit_data_domain)

add_executable(unit_timer unit/timer_test.cpp)
target_link_libraries(unit_timer PRIVATE infrastructure dts_log Threads::Threads)
add_test(NAME unit_timer COMMAND unit_timer)

# 维护节拍准点（回归：持续负载下 TIMER 不得饿死——相对期限 bug 曾致 TTL 停摆）
add_executable(unit_thread_tick unit/thread_tick_test.cpp)
target_link_libraries(unit_thread_tick PRIVATE infrastructure dts_log Threads::Threads)
add_test(NAME unit_thread_tick COMMAND unit_thread_tick)

# 定长 plain 类型（raw 零拷贝通道类型基础）：序列化 roundtrip + plain/bounded 语义
add_executable(unit_fixed_type unit/fixed_type_test.cpp)
target_include_directories(unit_fixed_type PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/detmw/src)
target_link_libraries(unit_fixed_type PRIVATE detmw Threads::Threads)
add_test(NAME unit_fixed_type COMMAND unit_fixed_type)

# ---- plain 定长通道冒烟（双进程，FixedBytesType 收发全量断言）----
set(PLAIN_CFG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test/config/plain)
set(PLAIN_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw-plain)

set(PLAIN_GEN_OUTPUTS
  ${PLAIN_GEN_DIR}/plain-dts/data_routes.h
  ${PLAIN_GEN_DIR}/plain-dts/control_routes.h
  ${PLAIN_GEN_DIR}/plain-dts/plain-dts.json
  ${PLAIN_GEN_DIR}/plain-dts/staticdiscovery.xml
  ${PLAIN_GEN_DIR}/plain-pub/plain-pub.json
  ${PLAIN_GEN_DIR}/plain-pub/staticdiscovery.xml)
add_custom_command(
  OUTPUT ${PLAIN_GEN_OUTPUTS}
  COMMAND python3 ${DETMW_GEN_SCRIPT} ${PLAIN_CFG_DIR} ${PLAIN_GEN_DIR}
  DEPENDS ${PLAIN_CFG_DIR}/plain-dts.json ${PLAIN_CFG_DIR}/plain-pub.json ${DETMW_GEN_SCRIPT}
  COMMENT "generating detmw plain configs"
  VERBATIM)
add_custom_target(detmw_plain_gen
  DEPENDS ${PLAIN_GEN_DIR}/plain-dts/plain-dts.json ${PLAIN_GEN_DIR}/plain-pub/plain-pub.json)

add_executable(plain_dts ${DTS_STARTUP_SOURCES})
target_include_directories(plain_dts PRIVATE ${PLAIN_GEN_DIR}/plain-dts)
target_link_libraries(plain_dts PRIVATE
  bootstrap task data log infrastructure dts_agent detsched detmw Threads::Threads)
add_dependencies(plain_dts detmw_plain_gen)

add_executable(plain_pub test/plain_pub.cpp)
target_include_directories(plain_pub PRIVATE ${CMAKE_SOURCE_DIR}/infrastructure/include)
target_link_libraries(plain_pub PRIVATE detmw infrastructure Threads::Threads)
add_dependencies(plain_pub detmw_plain_gen)

add_test(NAME plain_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_plain.sh
          $<TARGET_FILE:plain_dts> $<TARGET_FILE:plain_pub> ${PLAIN_GEN_DIR})


# ---- web-server 功能链路：dts 上报 -> kafka 沉淀 -> SSE 观测（环境自适应）----
add_test(NAME web_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_web.sh
          $<TARGET_FILE:tune_dts> $<TARGET_FILE:spa_mock_proc>
          $<TARGET_FILE:web_server> ${TUNE_GEN_DIR})
set_tests_properties(web_smoke PROPERTIES SKIP_RETURN_CODE "77")
