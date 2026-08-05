# 确定性调度线程库（detsched，thirdparty 组件，独立编译）
# 分段规约配置驱动：sched_defs.json -> platform_sched_defs.h（构建期生成）

set(SCHED_DEFS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/sched_defs.json)
set(SCHED_DEFS_H ${CMAKE_CURRENT_SOURCE_DIR}/include/platform_sched_defs.h)

add_custom_command(
  OUTPUT ${SCHED_DEFS_H}
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_sched_defs.py
          ${SCHED_DEFS_JSON} ${SCHED_DEFS_H}
  DEPENDS ${SCHED_DEFS_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_sched_defs.py
  COMMENT "generating platform_sched_defs.h"
  VERBATIM)

add_custom_target(sched_defs_gen DEPENDS ${SCHED_DEFS_H})

add_library(detsched STATIC
  src/thread_api.cpp
  src/thread_factory.cpp)
add_dependencies(detsched sched_defs_gen)
target_include_directories(detsched PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(detsched PUBLIC dts_log Threads::Threads)

# 骨架验证 demo（可选）
add_executable(detsched_demo examples/demo.cpp)
target_link_libraries(detsched_demo PRIVATE detsched)
