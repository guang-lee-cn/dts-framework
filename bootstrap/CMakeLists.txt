# 组合根（上电装配）：无编译产物，接口库仅提供 include；
# dts_startup.cpp 含进程特定生成路由头，由各可执行（cpf_dts/dpf_dts/测试）按进程编译。
add_library(bootstrap INTERFACE)
target_include_directories(bootstrap INTERFACE include)

# 进程上电公共源码（含 dts_startup.cpp），供各可执行复用
set(DTS_STARTUP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dts_startup.cpp
  PARENT_SCOPE)
