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

    const char DATA[] = "123456789012345678901234567890123456789012345678901234567890123";

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

        uint64_t readPos = 0;
        uint64_t lastWrite = 0;
        uint8_t localBuffer[sizeof(DATA)]{};

        MemoryRegion sharedBuffer(localBuffer, sizeof(DATA), network.getProtectionDomain(),
                                  MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteBuffer;
        MemoryRegion sharedWritePos(&lastWrite, sizeof(lastWrite), network.getProtectionDomain(),
                                    MemoryRegion::Permission::All);
        MemoryRegion sharedReadPos((void *) &readPos, sizeof(readPos), network.getProtectionDomain(),
                                   MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteWritePos;
        RemoteMemoryRegion remoteReadPos;
        receiveAndSetupRmr(sock, remoteBuffer, remoteWritePos, remoteReadPos);

        auto startTime = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES; ++i) {
            ReadWorkRequest readWritePos;
            readWritePos.setLocalAddress(sharedReadPos);
            readWritePos.setRemoteAddress(remoteReadPos);
            readWritePos.setCompletion(true);
            queuePair.postWorkRequest(readWritePos);
            completionQueue.waitForCompletion(); // Synchronize every time
        }
        auto endTime = chrono::steady_clock::now();
        auto msTaken = chrono::duration<double, milli>(endTime - startTime).count();
        cout << "Reading " << MESSAGES << " messages of size " << sizeof(readPos) << " took " << msTaken << "ms"
             << endl;

        startTime = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES - 1; ++i) {
            WriteWorkRequest writeRequest;
            writeRequest.setLocalAddress(sharedBuffer);
            writeRequest.setRemoteAddress(remoteBuffer);
            writeRequest.setCompletion(false);
            queuePair.postWorkRequest(writeRequest);
        }
        {
            WriteWorkRequest writeRequest;
            writeRequest.setLocalAddress(sharedBuffer);
            writeRequest.setRemoteAddress(remoteBuffer);
            writeRequest.setCompletion(true);
            queuePair.postWorkRequest(writeRequest);
            completionQueue.waitForCompletion();
        }
        endTime = chrono::steady_clock::now();
        msTaken = chrono::duration<double, milli>(endTime - startTime).count();
        cout << "Writing " << MESSAGES << " messages of size " << sizeof(DATA) << " took " << msTaken << "ms" << endl;

        startTime = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES - 1; ++i) {
            AtomicFetchAndAddWorkRequest atomicAddRequest;
            atomicAddRequest.setLocalAddress(sharedWritePos);
            atomicAddRequest.setAddValue(1);
            atomicAddRequest.setRemoteAddress(remoteWritePos);
            atomicAddRequest.setCompletion(false);
            queuePair.postWorkRequest(atomicAddRequest);
        }
        {
            AtomicFetchAndAddWorkRequest atomicAddRequest;
            atomicAddRequest.setLocalAddress(sharedWritePos);
            atomicAddRequest.setAddValue(1);
            atomicAddRequest.setRemoteAddress(remoteWritePos);
            atomicAddRequest.setCompletion(true);
            queuePair.postWorkRequest(atomicAddRequest);
            lastWrite++;
            completionQueue.waitForCompletion();
        }
        endTime = chrono::steady_clock::now();
        msTaken = chrono::duration<double, milli>(endTime - startTime).count();
        cout << "AtomicInc " << MESSAGES << " times took " << msTaken << "ms" << endl;

        startTime = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES - 1; ++i) { // TODO: test if this actually works
            AtomicCompareAndSwapWorkRequest atomicCasRequest;
            atomicCasRequest.setLocalAddress(sharedWritePos);
            atomicCasRequest.setCompareValue(lastWrite);
            atomicCasRequest.setSwapValue(--lastWrite);
            atomicCasRequest.setRemoteAddress(remoteWritePos);
            atomicCasRequest.setCompletion(false);
            queuePair.postWorkRequest(atomicCasRequest);
        }
        {
            AtomicCompareAndSwapWorkRequest atomicCasRequest;
            atomicCasRequest.setLocalAddress(sharedWritePos);
            atomicCasRequest.setCompareValue(lastWrite);
            atomicCasRequest.setSwapValue(--lastWrite);
            atomicCasRequest.setRemoteAddress(remoteWritePos);
            atomicCasRequest.setCompletion(true);
            queuePair.postWorkRequest(atomicCasRequest);
            completionQueue.waitForCompletion();
        }
        endTime = chrono::steady_clock::now();
        msTaken = chrono::duration<double, milli>(endTime - startTime).count();
        cout << "AtomicCas " << MESSAGES << " times took " << msTaken << "ms" << endl;
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

        const size_t completeBufferSize = sizeof(DATA);
        uint8_t localBuffer[completeBufferSize]{};
        MemoryRegion sharedMR(localBuffer, sizeof(DATA), network.getProtectionDomain(), MemoryRegion::Permission::All);
        uint64_t readPosition = 0;
        volatile uint64_t sharedBufferWritePosition = 0;
        MemoryRegion sharedWritePos((void *) &sharedBufferWritePosition, sizeof(uint64_t),
                                    network.getProtectionDomain(), MemoryRegion::Permission::All);
        MemoryRegion sharedReadPos(&readPosition, sizeof(readPosition), network.getProtectionDomain(),
                                   MemoryRegion::Permission::All);
        sendRmrInfo(acced, sharedMR, sharedWritePos, sharedReadPos);

        int ignored;
        cout << "Is client finished?" << endl;
        cin >> ignored;

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
