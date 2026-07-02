# CXLporter Prototype Design

The prototype maps the CXLfork paper's CXLporter operations to the current
two-VM lab environment.

| Paper operation | Prototype implementation |
| --- | --- |
| Function checkpointing | `cxlporter checkpoint` maps and initializes a function working set, then calls `CXLFORK_FORK_PREPARE`. |
| Object store of checkpoints | `scripts/30-benchmark-cxlporter.sh` records a CSV manifest containing function, key, size, pages, and checkpoint cost. The backing object is the CXLfork key/slot in shared CXL memory. |
| Ghost container pool | `cxlporter invoke` creates a pre-forked pool of helper processes. Each helper waits on a pipe, receives a restore job, calls `CXLFORK_FORK_RESUME`, executes the function workload, reports latency, and is replaced. |
| Tiering policy control | The invoke path passes `mow`, `moa`, or `hybrid` to the kernel via CXLfork restore flags. |
| Keep-alive window | The benchmark can inject `keepalive_us` between dispatches while keeping the ghost pool alive. |

The benchmark is intentionally smaller than OpenWhisk. It isolates the
CXLfork/CXLporter mechanisms from HTTP gateways, queues, and Docker image
management so local fork and CXLfork can be compared with the same function
code and working set.
