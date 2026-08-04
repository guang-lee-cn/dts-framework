# task 限界上下文（线程模块）：可独立编译、自由集成
add_library(task STATIC
  interface/src/dts_task_entry.cpp
  application/src/task_msg_handler.cpp
  domain/src/task_manager.cpp
  domain/src/task_factory.cpp
  domain/src/task_strategy.cpp)
target_include_directories(task PUBLIC
  interface/include application/include domain/include)
target_link_libraries(task PUBLIC infrastructure)
