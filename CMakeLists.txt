cmake_minimum_required(VERSION 3.21)
project(lockless)

set(CMAKE_CXX_STANDARD 17)
link_libraries(pthread)

add_executable(lockless main.cpp lockfree_queue.h bbq.h)
