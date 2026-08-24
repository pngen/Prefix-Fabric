find_package(Threads REQUIRED)
add_executable(prefix-fabric-benchmark benchmark.cpp)
target_link_libraries(prefix-fabric-benchmark PRIVATE prefix_fabric)
pf_set_warnings(prefix-fabric-benchmark)
