# CXLporter

This is a compact user-space CXLporter-style harness for the CXLfork
prototype. It also contains an Apache OpenWhisk standalone benchmark path for
running the same serverless-style workloads through OpenWhisk's Docker action
runtime. The CXLporter harness reproduces the paper's control-plane roles
closely enough to run comparable benchmarks:

- Function checkpointing on the checkpoint node.
- A checkpoint object-store manifest keyed by function/key.
- A pool of ghost containers implemented as pre-forked helper processes.
- Per-invocation CXLfork tiering policy selection (`mow`, `moa`, `hybrid`).
- Keep-alive delay injection for ghost-pool experiments.

The kernel CXLfork mechanism remains in `/dev/cxlfork`; this project only
orchestrates it from user space.

## Build

```sh
make
```

## Commands

```sh
./cxlporter checkpoint <key> <function> <bytes>
./cxlporter invoke <key> <function> <bytes> <policy> <pool> <requests> <warmup> [keepalive_us] [hot_stride_pages]
./cxlporter local <function> <bytes> <requests> <warmup>
```

Functions: `touch`, `json`, `pyaes`, `html`, `float`.

Policies: `mow`, `moa`, `hybrid`.

`checkpoint` is intended for VM0. `invoke` is intended for VM1. `local` can run
on either VM and uses ordinary `fork()`.

## Benchmark

From the workspace root on the host:

```sh
cd /home/xtl/cxlfork-ws/cxlporter
FUNCTIONS="json:4194304 pyaes:4194304 html:8388608 touch:67108864" \
POLICIES="mow moa hybrid" \
POOL_SIZES="1 4" \
REQUESTS=5 \
WARMUP=1 \
./scripts/30-benchmark-cxlporter.sh
```

The script writes two CSV files under `results/`: an invocation benchmark and a
checkpoint-store manifest.

## OpenWhisk Standalone

The OpenWhisk path lives under `openwhisk/` and uses the upstream
`openwhisk/standalone:nightly` image plus the upstream `wsk` CLI. The benchmark
default is `nodejs:20`, because the standalone image prewarms Node.js action
containers; `RUNTIME=python:3.10` is also supported when the corresponding
runtime image is available locally.

```sh
cd /home/xtl/cxlfork-ws/cxlporter/openwhisk
./scripts/start-standalone.sh
RUNTIME=nodejs:20 REQUESTS=5 WARMUP=1 ./scripts/40-benchmark-openwhisk.sh
REQUESTS=5 WARMUP=1 ./scripts/41-benchmark-local-node.sh
```

`40-benchmark-openwhisk.sh` measures the full OpenWhisk blocking invoke path
(`wsk action update`, Docker action runtime, activation metadata). The local
script runs the same action directly in the OpenWhisk Node.js runtime container,
without the OpenWhisk control plane, to provide a single-machine runtime
baseline.

## Publishing

This project is intended to live as a private GitHub submodule:

```sh
cd /home/xtl/cxlfork-ws/cxlporter
gh repo create qmyyxtl/cxlfork-cxlporter --private --source=. --remote=origin --push

cd /home/xtl/cxlfork-ws
git push origin remote-fork
```

If the repository already exists:

```sh
cd /home/xtl/cxlfork-ws/cxlporter
git remote set-url origin https://github.com/qmyyxtl/cxlfork-cxlporter.git
git push -u origin main

cd /home/xtl/cxlfork-ws
git push origin remote-fork
```
