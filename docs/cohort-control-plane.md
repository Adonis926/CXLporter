# Cohort CXLfork control-plane baseline

## Purpose and scope

This branch supplies a measurable control path above the restricted CXLfork
kernel data plane.  Its purpose is to separate the remaining cross-OS and
runtime control overhead from checkpoint data movement and kernel restore
work.  It is not a production OpenWhisk runtime and does not transparently
restore arbitrary Node.js containers.

The implemented request path is:

```text
benchmark client
  -> OpenWhisk standalone HTTP API
  -> warm Node.js proxy action in a Docker container
  -> TCP 172.17.0.1:2301
  -> QEMU hostfwd to VM1:7000
  -> CXLporter daemon queue
  -> pre-forked one-shot ghost worker
  -> CXLFORK_RESUME_V2
  -> synthetic function working set
  -> CXLFORK_GET_CALL_STATS_V2
  -> response through the same path
```

A direct client can start at the TCP endpoint and omit the OpenWhisk layers.
The difference between the two paths is the primary control-plane signal for
the Cohort experiments.

## Components

- `cxlporter-control checkpoint` creates a PMD-aligned anonymous working set
  in VM0 and publishes it with `CXLFORK_PREPARE_V2`.
- `cxlporter-control serve` runs in VM1.  It owns a bounded request queue, a
  fixed ghost pool, and a registry from function name to
  `(key, generation, bytes, checksums)`.
- `REGISTER` accepts an object only after `INSPECT_V2` proves its generation,
  size, and page checksum.  A stale or corrupt registration is rejected.
- Each worker serves one request and is then reaped and replaced.  This keeps
  process activation visible instead of hiding it in a permanent worker.
- The Node action in `openwhisk/actions/cxlporter_proxy.js` is only a TCP
  bridge.  It does not duplicate kernel restore or workload execution.
- The V2 UAPI header under the kernel submodule is authoritative.  The build
  copies it into the ignored `.build-uapi` directory; no second ABI definition
  is committed in CXLporter.

The wire protocol is line-oriented `CXLP/1`.  Supported operations are
`PING`, `REGISTER`, and `INVOKE`.  Invocation modes are `invoke`,
`restore-only`, and `cold`; policies are `readonly`, `mow`, `moa`, and
`hybrid`.

## Lifetime and failure semantics

The kernel restore context, slot reader reference, and per-call statistics are
owned independently from the device file.  A restored VMA keeps its context
alive until the last mapping closes.  The control daemon does not release a
slot while an active reader exists.

The acceptance test covers stale generation, unknown function, malformed
request, concurrent restore, active-reader `RELEASE_V2 -> EBUSY`, release after
the final reader, and clean VM dmesg.  Registration and invocation errors are
returned with a numeric status and an `error_stage`; they are not converted to
successful action results.

## Measurement model

Every CSV row is one request.  Schema v2 has 62 columns and preserves 64-bit
identifiers and checksums as strings through JavaScript.

The principal intervals are nested as follows:

```text
client_e2e_ns
  OpenWhisk wait + action scheduling/runtime + HTTP return
    action_total_ns
      action_connect_ns / action_first_byte_ns / action_bridge_rtt_ns
        daemon_total_ns
          queue_wait_ns
          worker_wakeup_ns
          worker_total_ns
            restore_ioctl_ns
              kernel_total_ns
            workload_ns
```

These parent and child intervals must not be added together.  OpenWhisk
`waitTime`, `initTime`, and `duration` annotations have millisecond resolution
and are reported independently from the nanosecond action instrumentation.

`kernel_total_ns` is partitioned by the V2 validity mask into lock wait,
lifecycle, validation, VMA creation, upper page-table allocation, shared-leaf
attach, fallback reconstruction, dirty prefetch, global restore, rollback, and
other time.  Fault dispatch, leaf materialization, data CoW, and CXL-to-local
copy are post-restore context counters.  A field is numeric only when both its
supported and valid bits are set; otherwise the protocol and CSV contain
`NA`.  The cold baseline likewise marks all restore-only fields `NA`.

The 1,000-sample micro and concurrency files report mean, P50, P95, P99, and
standard deviation.  The 100-sample scaling files deliberately report P99 as
`NA`.

## Running the experiments

From the main reproduction workspace:

```bash
scripts/start-cxlfork-vms.sh cxl
```

From the CXLporter submodule:

```bash
scripts/31-start-control-plane.sh
openwhisk/scripts/start-standalone.sh
openwhisk/scripts/42-deploy-cxlporter-action.sh
scripts/32-control-correctness.sh
scripts/33-benchmark-control-plane.sh
```

Standalone is started with an explicit invocation limit of 100,000/minute.
The script refuses to reuse a process with different throttle arguments.  This
avoids the upstream standalone default of 60/minute truncating a 1,000-sample
run.

## Truth boundaries

- The shared PTE leaf path and kernel page-table work are real within the
  restricted x86-64, 4 KiB, private-anonymous model.
- The current `touch` benchmark is read-only.  It proves shared-leaf attach,
  but does not exercise leaf materialization or data-page CoW; those fields are
  therefore `NA` and remain the responsibility of the kernel correctness
  suite.
- Global-file and cooperative-thread restore are disabled in the default data
  plane benchmark and do not block it.
- OpenWhisk is used as a real scheduler/action/container control path, but the
  action is a proxy to a synthetic C worker rather than a transparently
  restored OpenWhisk language runtime.
- The shared backend is a QEMU/KVM host file.  Results are valid for software
  control-path decomposition, not for absolute CXL device latency or bandwidth.

