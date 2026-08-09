# data 限界上下文（数据工厂子系统）：interface 接入 → application 编排 → domain 业务（零技术依赖）
add_library(data STATIC
  interface/src/dts_data_entry.cpp
  application/src/data_msg_handler.cpp
  domain/src/data_mem_manager.cpp
  domain/src/extractor.cpp
  domain/src/extractor_registry.cpp
  domain/src/data_factory_v2.cpp
  domain/src/tdmap_v2.cpp
  domain/src/extractors.cpp)
target_include_directories(data PUBLIC
  interface/include application/include domain/include)
target_link_libraries(data PUBLIC infrastructure)
