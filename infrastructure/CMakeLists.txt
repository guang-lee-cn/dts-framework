# 全局基础设施层：公共契约、mailbox、线程运行时（ThreadCtx/ThreadRun）
add_library(infrastructure STATIC
  src/dts_mw.cpp
  src/dts_thread.cpp)
target_include_directories(infrastructure PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(infrastructure PUBLIC detmw Threads::Threads)
