#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <rdma_tests/rdma/CompletionQueuePair.hpp>
#include <rdma_tests/rdma/QueuePair.hpp>
#include <rdma_tests/rdma/MemoryRegion.hpp>
#include <rdma_tests/rdma/WorkRequest.hpp>
#include <infiniband/verbs.h>
#include "rdma/Network.hpp"

using namespace std;
using namespace rdma;

int tcp_socket() {
    auto sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw runtime_error{"Could not open socket"};
    }
    return sock;
}

void tcp_connect(int sock, sockaddr_in &addr) {
    if (connect(sock, (sockaddr *) &addr, sizeof addr) < 0) {
        throw runtime_error{"error connect'ing"};
    }
}

void tcp_write(int sock, void *buffer, size_t size) {
    if (write(sock, buffer, size) < 0) {
        throw runtime_error{"error write'ing"};
    }
}

void tcp_read(int sock, void *buffer, size_t size) {
    if (read(sock, buffer, size) < 0) {
        throw runtime_error{"error read'ing"};
    }
}

void tcp_bind(int sock, sockaddr_in &addr) {
    if (bind(sock, (sockaddr *) &addr, sizeof addr) < 0) {
        throw runtime_error{"error bind'ing"};
    }
}

void exchangeQPNAndConnect(int sock, Network &network, QueuePair &queuePair);

void receiveAndSetupRmr(int sock, RemoteMemoryRegion &remoteMemoryRegion, RemoteMemoryRegion &remoteWritePos,
                        RemoteMemoryRegion &remoteReadPos);

void sendRmrInfo(int sock, MemoryRegion &sharedMemoryRegion, MemoryRegion &sharedWritePos, MemoryRegion &sharedReadPos);

int tcp_accept(int sock, sockaddr_in &inAddr) {
    socklen_t inAddrLen = sizeof inAddr;
    auto acced = accept(sock, (sockaddr *) &inAddr, &inAddrLen);
    if (acced < 0) {
        throw runtime_error{"error accept'ing"};
    }
    return acced;
}

static const size_t MESSAGES = 1024 * 1024;

int main(int argc, char **argv) {
    if (argc < 3 || (argv[1][0] == 'c' && argc < 4)) {
        cout << "Usage: " << argv[0] << " <client / server> <Port> [IP (if client)]" << endl;
        return -1;
    }
    const auto isClient = argv[1][0] == 'c';
    const auto port = ::atoi(argv[2]);

    auto sock = tcp_socket();

    const size_t BUFFER_SIZE = 64 * 4; // TODO: buffersize, that forces wraparound
    const char DATA[] = "123456789012345678901234567890123456789012345678901234567890123";
    static_assert(64 == sizeof(DATA), "DATA needs the right size ");

    // RDMA networking. The queues are needed on both sides
    Network network;
    CompletionQueuePair completionQueue(network);
    QueuePair queuePair(network, completionQueue);

    if (isClient) {
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, argv[3], &addr.sin_addr);

        tcp_connect(sock, addr);
        exchangeQPNAndConnect(sock, network, queuePair);

        const size_t completeBufferSize = BUFFER_SIZE;
        uint64_t writePos = 0;
        volatile uint64_t readPos = 0;
        uint8_t localBuffer[completeBufferSize]{};
        uint64_t lastWrite = 0;

        MemoryRegion sharedBuffer(localBuffer, completeBufferSize, network.getProtectionDomain(),
                                  MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteBuffer;
        MemoryRegion sharedWritePos(&lastWrite, sizeof(lastWrite), network.getProtectionDomain(),
                                    MemoryRegion::Permission::All);
        MemoryRegion sharedReadPos((void *) &readPos, sizeof(readPos), network.getProtectionDomain(),
                                   MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteWritePos;
        RemoteMemoryRegion remoteReadPos;
        receiveAndSetupRmr(sock, remoteBuffer, remoteWritePos, remoteReadPos);

        const auto startTime = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES; ++i) {
            // Send DATA
            const size_t sizeToWrite = sizeof(DATA);
            size_t safeToWrite = completeBufferSize - (writePos - readPos);
            while (safeToWrite < sizeToWrite) { // Only synchronize with sever when necessary
                ReadWorkRequest readWritePos;
                readWritePos.setLocalAddress(sharedReadPos);
                readWritePos.setRemoteAddress(remoteReadPos);
                readWritePos.setCompletion(true);
                queuePair.postWorkRequest(readWritePos);
                completionQueue.waitForCompletion(); // Synchronization point
                safeToWrite = completeBufferSize - (writePos - readPos);
            }
            const size_t beginPos = writePos % completeBufferSize;
            const size_t endPos = (writePos + sizeToWrite - 1) % completeBufferSize;
            if (endPos <= beginPos) {
                uint8_t *begin1 = (uint8_t *) localBuffer;
                begin1 += beginPos;
                size_t bytesToWrite1 = completeBufferSize - beginPos;
                uint8_t *begin2 = (uint8_t *) localBuffer;
                size_t bytesToWrite2 = endPos;
                cout << "Write to    [" << beginPos << ", " << completeBufferSize << "]\n";
                cout << "split write [" << 0 << ", " << endPos << "]" << endl;

                MemoryRegion sendBuffer1(begin1, bytesToWrite1, network.getProtectionDomain(),
                                         MemoryRegion::Permission::All);
                RemoteMemoryRegion receiveBuffer1;
                receiveBuffer1.key = remoteBuffer.key;
                receiveBuffer1.address = remoteBuffer.address + beginPos;
                MemoryRegion sendBuffer2(begin2, bytesToWrite2, network.getProtectionDomain(),
                                         MemoryRegion::Permission::All);
                RemoteMemoryRegion receiveBuffer2;
                receiveBuffer2.key = remoteBuffer.key;
                receiveBuffer2.address = remoteBuffer.address/* + 0*/;
                WriteWorkRequest wr1;
                wr1.setLocalAddress(sendBuffer1);
                wr1.setRemoteAddress(receiveBuffer1);
                wr1.setCompletion(false);
                WriteWorkRequest wr2;
                wr2.setLocalAddress(sendBuffer2);
                wr2.setRemoteAddress(receiveBuffer2);
                wr2.setCompletion(false);

                AtomicFetchAndAddWorkRequest atomicAddRequest;
                atomicAddRequest.setLocalAddress(sharedWritePos);
                atomicAddRequest.setAddValue(sizeToWrite);
                atomicAddRequest.setRemoteAddress(remoteWritePos);
                atomicAddRequest.setCompletion(false);

                writePos += sizeToWrite;
                wr1.setNextWorkRequest(&wr2);
                wr2.setNextWorkRequest(&atomicAddRequest);

                queuePair.postWorkRequest(wr1); // only one post

                throw runtime_error{"Can't cope with wraparound yet"};
            } else { // Nice linear memory
                uint8_t *begin = (uint8_t *) localBuffer;
                begin += beginPos;
                //uint8_t* end = (uint8_t*) localBuffer;
                //end += endPos;
                memcpy(begin, DATA, sizeToWrite);
                for (size_t j = 0; j < sizeToWrite; ++j) {
                    cout << localBuffer[beginPos + j];
                }
                cout << endl;
                // TODO: Don't constantly allocate new MRs, since that's a context switch
                MemoryRegion sendBuffer(begin, sizeToWrite, network.getProtectionDomain(),
                                        MemoryRegion::Permission::All);
                RemoteMemoryRegion receiveBuffer;
                receiveBuffer.key = remoteBuffer.key;
                receiveBuffer.address = remoteBuffer.address + beginPos;

                WriteWorkRequest writeRequest;
                writeRequest.setLocalAddress(sendBuffer);
                writeRequest.setRemoteAddress(receiveBuffer);
                writeRequest.setCompletion(false);
                // Dont post yet, we can chain the WRs

                AtomicFetchAndAddWorkRequest atomicAddRequest;
                atomicAddRequest.setLocalAddress(sharedWritePos);
                atomicAddRequest.setAddValue(sizeToWrite);
                atomicAddRequest.setRemoteAddress(remoteWritePos);
                atomicAddRequest.setCompletion(false);

                // We probably don't need an explicit wraparound. The uint64_t will overflow, when we write more than
                // 16384 Petabyte == 16 Exabyte. That probably "ought to be enough for anybody" in the near future.
                // Assume we have 100Gb/s transfer, then we'll overflow in (16*1024*1024*1024/(100/8))s == 700 years
                writePos += sizeToWrite;

                writeRequest.setNextWorkRequest(&atomicAddRequest);

                queuePair.postWorkRequest(writeRequest); // Only one post
            }
        }

        while (writePos != readPos) {
            ReadWorkRequest readWritePos;
            readWritePos.setLocalAddress(sharedReadPos);
            readWritePos.setRemoteAddress(remoteReadPos);
            readWritePos.setCompletion(true);
            queuePair.postWorkRequest(readWritePos);
            completionQueue.waitForCompletion();
        }
        const auto endTime = chrono::steady_clock::now();
        const auto msTaken = chrono::duration<double, milli>(endTime - startTime).count();
        const auto totallyWritten = sizeof(DATA) * MESSAGES;
        cout << "wrote " << totallyWritten << " Bytes of data (" << MESSAGES << "x" << sizeof(DATA) << ")" << endl;
        cout << "with a buffer size of " << completeBufferSize << endl;
        const auto bytesPerms = ((double) totallyWritten) / msTaken;
        cout << "that's " << ((bytesPerms / 1024) / 1024) * 1000 << "MByte/s" << endl;
    } else {
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        tcp_bind(sock, addr);
        listen(sock, SOMAXCONN);
        sockaddr_in inAddr;

        auto acced = tcp_accept(sock, inAddr);
        exchangeQPNAndConnect(acced, network, queuePair);

        const size_t completeBufferSize = BUFFER_SIZE;
        uint8_t localBuffer[completeBufferSize]{};
        MemoryRegion sharedMR(localBuffer, BUFFER_SIZE, network.getProtectionDomain(), MemoryRegion::Permission::All);
        uint64_t readPosition = 0;
        volatile uint64_t sharedBufferWritePosition = 0;
        MemoryRegion sharedWritePos((void *) &sharedBufferWritePosition, sizeof(uint64_t),
                                    network.getProtectionDomain(), MemoryRegion::Permission::All);
        MemoryRegion sharedReadPos(&readPosition, sizeof(readPosition), network.getProtectionDomain(),
                                   MemoryRegion::Permission::All);
        sendRmrInfo(acced, sharedMR, sharedWritePos, sharedReadPos);

        for (size_t i = 0; i < MESSAGES; ++i) {
            const size_t sizeToRead = sizeof(DATA);
            size_t beginPos = readPosition % completeBufferSize;
            size_t endPos = (readPosition + sizeToRead - 1) % completeBufferSize;
            // Spin wait until we have some data
            while (readPosition == sharedBufferWritePosition) sched_yield();
            if (endPos < beginPos) {
                //cout << "Read from  [" << beginPos << ", " << completeBufferSize << "]\n";
                //cout << "split read [" << 0 << ", " << endPos << "]" << endl;
                //throw runtime_error{"Can't cope with wraparound yet"};
                readPosition += sizeToRead;
            } else { // Nice linear data
                //const size_t begin = readPosition % completeBufferSize;
                readPosition += sizeToRead;
            }
        }

        close(acced);
    }

    close(sock);
    return 0;
}

struct RmrInfo {
    uint32_t bufferKey;
    uintptr_t bufferAddress;
    static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "Only 64bit platforms supported");
    uint32_t writePosKey;
    uintptr_t writePosAddress;
    uint32_t readPosKey;
    uintptr_t readPosAddress;
};

void receiveAndSetupRmr(int sock, RemoteMemoryRegion &remoteMemoryRegion, RemoteMemoryRegion &remoteWritePos,
                        RemoteMemoryRegion &remoteReadPos) {
    RmrInfo rmrInfo;
    tcp_read(sock, &rmrInfo, sizeof(rmrInfo));
    rmrInfo.bufferKey = ntohl(rmrInfo.bufferKey);
    rmrInfo.writePosKey = ntohl(rmrInfo.writePosKey);
    rmrInfo.readPosKey = ntohl(rmrInfo.readPosKey);
    rmrInfo.bufferAddress = be64toh(rmrInfo.bufferAddress);
    rmrInfo.writePosAddress = be64toh(rmrInfo.writePosAddress);
    rmrInfo.readPosAddress = be64toh(rmrInfo.readPosAddress);
    remoteMemoryRegion.key = rmrInfo.bufferKey;
    remoteWritePos.key = rmrInfo.writePosKey;
    remoteReadPos.key = rmrInfo.readPosKey;
    remoteMemoryRegion.address = rmrInfo.bufferAddress;
    remoteWritePos.address = rmrInfo.writePosAddress;
    remoteReadPos.address = rmrInfo.readPosAddress;
}

void
sendRmrInfo(int sock, MemoryRegion &sharedMemoryRegion, MemoryRegion &sharedWritePos, MemoryRegion &sharedReadPos) {
    RmrInfo rmrInfo;
    rmrInfo.bufferKey = htonl(sharedMemoryRegion.key->rkey);
    rmrInfo.writePosKey = htonl(sharedWritePos.key->rkey);
    rmrInfo.readPosKey = htonl(sharedReadPos.key->rkey);
    rmrInfo.bufferAddress = htobe64((uint64_t) sharedMemoryRegion.address);
    rmrInfo.writePosAddress = htobe64((uint64_t) sharedWritePos.address);
    rmrInfo.readPosAddress = htobe64((uint64_t) sharedReadPos.address);
    tcp_write(sock, &rmrInfo, sizeof(rmrInfo));
}

void exchangeQPNAndConnect(int sock, Network &network, QueuePair &queuePair) {
    uint32_t qpn = queuePair.getQPN();
    uint32_t qPNbuffer = htonl(qpn);
    tcp_write(sock, &qPNbuffer, sizeof(qPNbuffer)); // Send own qpn to server
    tcp_read(sock, &qPNbuffer, sizeof(qPNbuffer)); // receive qpn
    qpn = ntohl(qPNbuffer);
    const Address address{network.getLID(), qpn};
    queuePair.connect(address);
    cout << "connected to qpn " << qpn << endl;
}

