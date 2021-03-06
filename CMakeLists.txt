cmake_minimum_required(VERSION 3.0)
project(rdma_sockets)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wall -Wextra -Wnon-virtual-dtor -Wold-style-cast -fshow-column -pipe -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -flto -DNDEBUG")

set(SOURCE_FILES
        rdma/CompletionQueuePair.cpp
        rdma/MemoryRegion.cpp
        rdma/Network.cpp
        rdma/QueuePair.cpp
        rdma/ReceiveQueue.cpp
        rdma/WorkRequest.cpp
        tcpWrapper.cpp
        RDMAMessageBuffer.cpp
        )
set(OVERRIDES_FILES
        fileDescriptorOverrides/overrides.cpp
        fileDescriptorOverrides/realFunctions.cpp)

include_directories(..)

add_executable(minimal minimal.cpp ${SOURCE_FILES})
target_link_libraries(minimal ibverbs)

add_executable(tcpPingPong tcpPingPong.cpp tcpWrapper.cpp)

add_executable(forkingPingPong forkingPingPong.cpp tcpWrapper.cpp)

add_executable(rdmaPingPong rdmaPingPong.cpp ${SOURCE_FILES})
target_link_libraries(rdmaPingPong ibverbs)

add_executable(rdmaInlineComparison rdmaInlineComparison.cpp ${SOURCE_FILES})
target_link_libraries(rdmaInlineComparison ibverbs)

add_library(preloadRDMA SHARED ${SOURCE_FILES} ${OVERRIDES_FILES})
target_link_libraries(preloadRDMA ibverbs)
