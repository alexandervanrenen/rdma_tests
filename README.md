# A project to tunnel TCP sockets over Infiniband / RDMA

In this repo, I use libibverbs and the C++ RDMA wrapper [roediger](https://github.com/roediger) and [alexandervanrenen](https://github.com/alexandervanrenen) 
wrote to create a `LD_PRELAOD` library for for TCP sockets (like [TSSX](https://github.com/goldsborough/tssx) did for 
domain sockets).

## Microbenchmarks

Reproducible with
```bash
tcpPingPong
rdmaPingPong
```

| Where      | What | RoundTrips / second | avg. latency in µs |
| ---------- | ---- | ------------------: | -----------------: |
| localhost  | TCP  |              81,781 |              12.23 |
| localhost  | RDMA |             472,415 |               2.12 |
| network    | TCP  |              39,541 |              25.29 |
| network    | RDMA |             381,520 |               2.62 |

## Remarks
* Keeping track of the sent / received messages with a separate AtomicFetchAndAddWorkRequest also slows the RTT by ~50%. Keeping the message in a single WriteRequest seems reasonable.
* RDMA guarantees, that memory is written in order. However, only bytes are written atomically. When reading bigger words, they might be written partially.
* `IBV_SEND_INLINE` is significantly faster for messages < 192 Bytes.

## Calling `fork()`
`fork()`-ing libibverbs should be avoided. However, the [man pages](https://linux.die.net/man/3/ibv_fork_init) suggest, that forking can be done when calling `ibv_fork_init()` before forking, or simply setting `IBV_FORK_SAFE=1`.  
However, trying to get this to work with postgres results in a segfault in the server process.

There is a (quite hacky) solution in place to allow correct operation with forking programs, by setting `RDMA_FORKGEN=1`. This works by only opening the RDMA connection, after 1 call to `fork()` and avoids later calls to it. E.g.:
```bash
RDMA_FORKGEN=1 USE_RDMA=127.0.0.1 LD_PRELOAD=$HOME/rdma_tests/bin/preloadRDMA.so ./forkingPingPong server 1234
RDMA_FORKGEN=0 USE_RDMA=127.0.0.1 LD_PRELOAD=$HOME/rdma_tests/bin/preloadRDMA.so ./forkingPingPong client 1234 127.0.0.1
```

You need to know in which generation your program stops to fork and set the environment variable accordingly.

## Executing postgres with the preload library

```bash
# Server
RDMA_FORKGEN=1 USE_RDMA=10.0.0.11 LD_PRELOAD=$HOME/rdma_tests/bin/preloadRDMA.so ./bin/postgres -D ../tmp/ -p 4567
# Client
RDMA_FORKGEN=0 USE_RDMA=10.0.0.16 LD_PRELOAD=$HOME/rdma_tests/bin/preloadRDMA.so ./bin/psql -h scyper16 -p 4567 -d postgres
```

Results in a working psql environment, which we can benchmark for a more realistic test:
```bash
$ wc -l pgbench.log
39705 pgbench.log
# Benchmark over TCP
$ time cat pgbench.log | ./bin/psql -h scyper16 -p 4567 -d postgres > /dev/null
real	0m12.138s
user	0m1.072s
sys	0m1.156s
# Benchmark over RDMA
real	0m7.685s
user	0m7.664s
sys	0m0.040s
$ time cat pgbench.log | RDMA_FORKGEN=0 USE_RDMA=10.0.0.16 LD_PRELOAD=$HOME/rdma_tests/bin/preloadRDMA.so ./bin/psql -h scyper16 -p 4567 -d postgres > /dev/null
```

One can already see, that the `sys` time is almost gone, since we don't use any syscalls. However, the ~50% performances increases are not quite in par with the microbenchmark speedup, yet.

## Building
The project can be built with CMake on any platform libibverbs is supported on (Only tested on Linux though) and a reasonably modern compiler (C++14).

```bash
mkdir bin
cd bin
cmake -DCMAKE_BUILD_TYPE=Release .. # Can also be set to Debug
make -j
```
