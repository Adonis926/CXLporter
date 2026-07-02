# CXLporter

This is a compact user-space CXLporter-style harness for the CXLfork
prototype. It is not a full OpenWhisk deployment. It reproduces the paper's
CXLporter control-plane roles closely enough to run comparable benchmarks:

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
