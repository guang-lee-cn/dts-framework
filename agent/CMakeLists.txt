# dts-agent 独立库（日志走 dts::log 门面，SPA 联编用）
add_library(dts_agent STATIC
  src/dts_agent_api.cpp
  src/pingpong.cpp)
target_include_directories(dts_agent PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(dts_agent PUBLIC dts_log Threads::Threads)
