# 日志门面（dts::log，底层 spdlog；业务代码不直接调 spdlog）
add_library(dts_log STATIC
  src/log.cpp
  src/ts_rotating_sink.cpp)
target_include_directories(dts_log PUBLIC include)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPDLOG REQUIRED spdlog)
target_include_directories(dts_log PUBLIC ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(dts_log PUBLIC ${SPDLOG_LIBRARIES} Threads::Threads)

# 全局基础设施层：公共契约、mailbox、线程运行时（ThreadCtx/ThreadRun）
add_library(infrastructure STATIC
  src/dts_mw.cpp
  src/dts_thread.cpp)
target_include_directories(infrastructure PUBLIC include)
target_link_libraries(infrastructure PUBLIC detmw dts_log Threads::Threads)
