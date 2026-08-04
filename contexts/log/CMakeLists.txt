# log 限界上下文（线程模块）：可独立编译、自由集成（仅采集，占位）
add_library(log STATIC
  interface/src/dts_log_entry.cpp
  application/src/log_msg_handler.cpp)
target_include_directories(log PUBLIC
  interface/include application/include)
target_link_libraries(log PUBLIC infrastructure)
