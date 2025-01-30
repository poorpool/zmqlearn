cmake_minimum_required(VERSION 3.11)
project(zmqlearn VERSION 0.1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g -ggdb -mavx -fno-omit-frame-pointer -fno-inline-functions -fdiagnostics-color=always")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -mavx -fdiagnostics-color=always")

# 查找 MPI
find_package(MPI REQUIRED)
# include_directories(${MPI_CXX_INCLUDE_DIRS})

# 生成可执行文件
add_executable(zmqlearn zmqlearn.cc)

# 链接 MPI 和 ZeroMQ
target_link_libraries(zmqlearn PRIVATE ${MPI_CXX_LIBRARIES} zmq)
