add_executable(of_bench of_bench.cpp ../examples/ex_support.cpp)
target_link_libraries(of_bench PRIVATE OpticalFabric)
target_include_directories(of_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples)
optical_fabric_apply_warnings(of_bench)
