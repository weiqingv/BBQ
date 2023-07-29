cmake_minimum_required(VERSION 3.21)
project(lockless)

set(CMAKE_CXX_STANDARD 17)

set(BOOST_ROOT /usr/local/boost_1_82_0)

include_directories(BOOST_ROOT)
link_directories(lockless /usr/local/lib)

link_libraries(boost_thread)
link_libraries(pthread)

add_executable(lockless main.cpp benchmark/lockfree_queue.h bbq.h benchmark/benchmarks.cpp benchmark/MPMCQueue.h benchmark/concurrentqueue/blockingconcurrentqueue.h benchmark/concurrentqueue/concurrentqueue.h benchmark/concurrentqueue/lightweightsemaphore.h benchmark/queues/include/mpmc-bounded-queue.h)

