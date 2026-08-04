# data 限界上下文（线程模块）：可独立编译、自由集成（含 1s 定时上报）
add_library(data STATIC
  interface/src/dts_data_entry.cpp
  application/src/data_msg_handler.cpp
  domain/src/tdmap.cpp
  domain/src/report_cache.cpp
  domain/src/data_construct.cpp
  domain/src/data_factory.cpp
  domain/src/processor/cell_prb_processor.cpp
  domain/src/processor/ue_bler_processor.cpp)
target_include_directories(data PUBLIC
  interface/include application/include domain/include)
target_link_libraries(data PUBLIC infrastructure)
