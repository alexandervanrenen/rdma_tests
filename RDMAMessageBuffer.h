#ifndef RDMA_HASH_MAP_RDMAMESSAGEBUFFER_H
#define RDMA_HASH_MAP_RDMAMESSAGEBUFFER_H

#include <atomic>
#include "rdma/Network.hpp"
#include "rdma/CompletionQueuePair.hpp"
#include "rdma/QueuePair.hpp"
#include "rdma/MemoryRegion.hpp"

struct RDMANetworking {
    rdma::Network network;
    rdma::CompletionQueuePair completionQueue;
    rdma::QueuePair queuePair;

    RDMANetworking(int sock);
};

class RDMAMessageBuffer {
public:

    void send(const uint8_t *data, size_t length);

    std::vector<uint8_t> receive();

    // Construct a message buffer of the given size, exchanging RDMA networking information over the given socket
    RDMAMessageBuffer(size_t size, int sock);

    bool hasData();

private:
    const size_t size;
    const size_t bitmask;
    RDMANetworking net;
    std::unique_ptr<volatile uint8_t[]> receiveBuffer;
    std::atomic<size_t> readPos{0};
    std::unique_ptr<uint8_t[]> sendBuffer;
    size_t sendPos = 0;
    volatile size_t currentRemoteReceive = 0;
    rdma::MemoryRegion localSend;
    rdma::MemoryRegion localReceive;
    rdma::MemoryRegion localReadPos;
    rdma::MemoryRegion localCurrentRemoteReceive;
    rdma::RemoteMemoryRegion remoteReceive;
    rdma::RemoteMemoryRegion remoteReadPos;

    void writeToSendBuffer(const uint8_t *data, size_t sizeToWrite);

    void readFromReceiveBuffer(size_t readPos, uint8_t *whereTo, size_t sizeToRead);

    void zeroReceiveBuffer(size_t beginReceiveCount, size_t sizeToZero);
};

#endif //RDMA_HASH_MAP_RDMAMESSAGEBUFFER_H
