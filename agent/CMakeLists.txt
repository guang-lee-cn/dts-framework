# dts-agent 独立库（自包含，不依赖其它层，SPA 联编用）
add_library(dts_agent STATIC
  src/dts_agent_api.cpp
  src/pingpong.cpp)
target_include_directories(dts_agent PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(dts_agent PUBLIC Threads::Threads)
