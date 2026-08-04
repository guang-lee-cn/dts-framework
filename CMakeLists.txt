cmake_minimum_required(VERSION 3.16)
project(dts_framework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 导出编译数据库，供 clangd/IDE 精确跳转
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror)

find_package(Threads REQUIRED)

# 工程构建组织
add_subdirectory(infrastructure)          # 全局基础设施层
add_subdirectory(contexts/task)           # 限界上下文：task
add_subdirectory(contexts/data)           # 限界上下文：data
add_subdirectory(contexts/log)            # 限界上下文：log
add_subdirectory(agent)                   # dts-agent 独立库
add_subdirectory(thirdparty/detsched)       # 确定性调度线程库
add_subdirectory(thirdparty/detmw)          # 统一进程间通信中间件
add_subdirectory(thirdparty/bucket)         # 自研桶传输（共享内存环 + TransportInterface 外壳）
add_subdirectory(bootstrap)               # 组合根（上电装配，无编译产物）

# detmw 配置组生成：cpf/dpf -> 线程路由头 + 进程生成配置 + 组静态发现 xml（构建期，产物进 build/）
set(DETMW_CFG_DIR ${CMAKE_SOURCE_DIR}/config/detmw/dts)
set(DETMW_GEN_DIR ${CMAKE_BINARY_DIR}/generated/detmw)
set(DETMW_GEN_SCRIPT ${CMAKE_SOURCE_DIR}/thirdparty/detmw/tools/gen_detmw.py)

set(DETMW_GEN_OUTPUTS
  ${DETMW_GEN_DIR}/cpf-dts/task_routes.h
  ${DETMW_GEN_DIR}/cpf-dts/data_routes.h
  ${DETMW_GEN_DIR}/cpf-dts/log_routes.h
  ${DETMW_GEN_DIR}/cpf-dts/cpf-dts.json
  ${DETMW_GEN_DIR}/cpf-dts/staticdiscovery.xml
  ${DETMW_GEN_DIR}/dpf-dts/task_routes.h
  ${DETMW_GEN_DIR}/dpf-dts/data_routes.h
  ${DETMW_GEN_DIR}/dpf-dts/log_routes.h
  ${DETMW_GEN_DIR}/dpf-dts/dpf-dts.json
  ${DETMW_GEN_DIR}/dpf-dts/staticdiscovery.xml)
add_custom_command(
  OUTPUT ${DETMW_GEN_OUTPUTS}
  COMMAND python3 ${DETMW_GEN_SCRIPT} ${DETMW_CFG_DIR} ${DETMW_GEN_DIR}
  DEPENDS ${DETMW_CFG_DIR}/cpf/cpf-dts.json ${DETMW_CFG_DIR}/dpf/dpf-dts.json ${DETMW_GEN_SCRIPT}
  COMMENT "generating detmw routes/json/xml"
  VERBATIM)
add_custom_target(detmw_gen
  DEPENDS ${DETMW_GEN_OUTPUTS})

# 两个 dts 进程可执行：各自 include 各自生成目录，各自编译 startup.cpp（含生成路由头）
set(DTS_PROCS cpf-dts dpf-dts)
foreach(proc IN LISTS DTS_PROCS)
  string(REPLACE "-" "_" exe "${proc}")   # cpf-dts -> cpf_dts
  add_executable(${exe}
    ${DTS_STARTUP_SOURCES})
  target_include_directories(${exe} PRIVATE ${DETMW_GEN_DIR}/${proc})
  target_link_libraries(${exe} PRIVATE
    bootstrap task data log infrastructure dts_agent detsched detmw Threads::Threads)
  add_dependencies(${exe} detmw_gen)
endforeach()

enable_testing()
add_subdirectory(quality)
