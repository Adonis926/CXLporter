#!/usr/bin/env python3
"""Per-request CXLporter/OpenWhisk control-path benchmark."""

import argparse
import base64
import concurrent.futures
import csv
import http.client
import json
import os
import pathlib
import socket
import statistics
import time
import urllib.parse
import uuid


RESULT_FIELDS = [
    "schema_version", "run_id", "request_id", "timestamp_ns", "path",
    "activation_id", "function", "policy", "mode", "validation", "bytes",
    "pool_size", "concurrency", "status", "verify", "error_stage",
    "client_e2e_ns", "ow_wait_ns", "ow_init_ns", "ow_duration_ns",
    "action_total_ns", "action_connect_ns", "action_first_byte_ns",
    "action_bridge_rtt_ns", "daemon_total_ns", "queue_wait_ns",
    "worker_wakeup_ns", "worker_total_ns", "restore_ioctl_ns", "workload_ns",
    "kernel_total_ns", "lock_wait_ns", "lifecycle_ns", "validation_ns",
    "vma_create_ns", "upper_pt_ns",
    "shared_leaf_ns", "fallback_ns", "dirty_prefault_ns", "kernel_other_ns",
    "global_restore_ns", "rollback_ns",
    "supported_mask", "valid_mask", "path_mask", "key", "generation",
    "restore_id", "pages", "page_checksum", "result_checksum", "faults",
    "fault_dispatch_ns", "leaf_materialize_ns", "data_cow_ns", "cxl_copy_ns",
    "shared_leaf_pages", "fallback_pages", "local_copy_pages", "leaf_copy_pages",
    "dirty_prefault_pages", "validated_ptes",
]

SUMMARY_METRICS = [
    "client_e2e_ns", "ow_wait_ns", "ow_init_ns", "ow_duration_ns",
    "action_total_ns", "action_bridge_rtt_ns", "daemon_total_ns",
    "queue_wait_ns", "worker_wakeup_ns", "worker_total_ns",
    "restore_ioctl_ns", "workload_ns", "kernel_total_ns", "lock_wait_ns",
    "lifecycle_ns", "validation_ns",
    "vma_create_ns", "upper_pt_ns", "shared_leaf_ns", "fallback_ns",
    "dirty_prefault_ns", "global_restore_ns", "rollback_ns",
    "kernel_other_ns", "fault_dispatch_ns",
    "leaf_materialize_ns", "data_cow_ns", "cxl_copy_ns",
]


def parse_line(line):
    parts = line.strip().split()
    if len(parts) < 2 or parts[:2] != ["CXLP/1", "RESULT"]:
        raise ValueError(f"bad CXLP response: {line[:160]}")
    result = {}
    for part in parts[2:]:
        key, sep, value = part.partition("=")
        if not sep:
            raise ValueError(f"bad CXLP field: {part}")
        result[key] = value
    return result


def direct_call(args, request_id):
    line = (f"CXLP/1 INVOKE id={request_id} function={args.function} "
            f"policy={args.policy} mode={args.mode} validation={args.validation}")
    if args.hold_ms:
        line += f" hold_ms={args.hold_ms}"
    begin = time.perf_counter_ns()
    with socket.create_connection((args.control_host, args.control_port),
                                  timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall((line + "\n").encode())
        data = bytearray()
        while b"\n" not in data:
            chunk = sock.recv(8192)
            if not chunk:
                raise RuntimeError("CXLporter closed before response")
            data.extend(chunk)
    end = time.perf_counter_ns()
    return parse_line(bytes(data).split(b"\n", 1)[0].decode()), {}, end - begin


def load_wskprops(path):
    props = {}
    for line in pathlib.Path(path).expanduser().read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            props[key] = value
    return props


def annotation_ns(activation, name):
    for item in activation.get("annotations", []):
        if item.get("key") == name:
            return str(int(item.get("value", 0)) * 1_000_000)
    return "NA"


def openwhisk_call(args, request_id):
    body = {
        "operation": "INVOKE",
        "request_id": request_id,
        "function": args.function,
        "policy": args.policy,
        "mode": args.mode,
        "validation": args.validation,
    }
    if args.hold_ms:
        body["hold_ms"] = args.hold_ms
    parsed = urllib.parse.urlparse(args.apihost)
    connection_cls = (http.client.HTTPSConnection if parsed.scheme == "https"
                      else http.client.HTTPConnection)
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    auth = base64.b64encode(args.auth.encode()).decode()
    path = (f"/api/v1/namespaces/_/actions/{args.action}"
            "?blocking=true")
    begin = time.perf_counter_ns()
    conn = connection_cls(parsed.hostname, port, timeout=args.timeout)
    conn.request("POST", path, json.dumps(body), {
        "Authorization": f"Basic {auth}",
        "Content-Type": "application/json",
    })
    response = conn.getresponse()
    payload = response.read()
    conn.close()
    end = time.perf_counter_ns()
    if response.status != 200:
        raise RuntimeError(f"OpenWhisk HTTP {response.status}: {payload[:200]!r}")
    activation = json.loads(payload)
    result = activation.get("response", {}).get("result", {})
    meta = {
        "activation_id": activation.get("activationId", "NA"),
        "ow_wait_ns": annotation_ns(activation, "waitTime"),
        "ow_init_ns": annotation_ns(activation, "initTime"),
        "ow_duration_ns": str(int(activation.get("duration", 0)) * 1_000_000),
    }
    return {str(k): str(v) for k, v in result.items()}, meta, end - begin


def one_sample(args, run_id, index):
    request_id = f"{run_id}-{index}-{uuid.uuid4().hex[:8]}"
    base = {field: "NA" for field in RESULT_FIELDS}
    base.update({
        "schema_version": "2", "run_id": run_id,
        "request_id": request_id, "timestamp_ns": str(time.time_ns()),
        "path": args.path, "function": args.function, "policy": args.policy,
        "mode": args.mode, "validation": args.validation,
        "bytes": str(args.bytes), "pool_size": str(args.pool_size),
        "concurrency": str(args.concurrency),
    })
    try:
        if args.path == "direct":
            result, meta, elapsed = direct_call(args, request_id)
        else:
            result, meta, elapsed = openwhisk_call(args, request_id)
        aliases = {
            "id": "request_id",
        }
        for key, value in result.items():
            target = aliases.get(key, key)
            if target in base:
                base[target] = value
        base.update(meta)
        base["client_e2e_ns"] = str(elapsed)
    except Exception as exc:  # Preserve failures as samples rather than losing a run.
        base["status"] = "client_error"
        base["verify"] = "0"
        base["error_stage"] = f"client:{type(exc).__name__}"
    return base


def percentile(values, fraction):
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def write_summary(rows, path):
    group_fields = ["path", "function", "bytes", "policy", "mode",
                    "validation", "pool_size", "concurrency"]
    groups = {}
    for row in rows:
        key = tuple(row[name] for name in group_fields)
        groups.setdefault(key, []).append(row)
    fields = group_fields + ["metric", "samples", "failures", "mean", "p50",
                             "p95", "p99", "stddev", "min", "max"]
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for key, grouped in sorted(groups.items()):
            failures = sum(r["status"] != "0" or r["verify"] != "1"
                           for r in grouped)
            for metric in SUMMARY_METRICS:
                values = []
                for row in grouped:
                    value = row.get(metric, "NA")
                    if value not in ("", "NA") and row["status"] == "0" and row["verify"] == "1":
                        values.append(int(value))
                if not values:
                    continue
                output = dict(zip(group_fields, key))
                output.update({
                    "metric": metric, "samples": len(values), "failures": failures,
                    "mean": f"{statistics.mean(values):.3f}",
                    "p50": f"{percentile(values, .50):.3f}",
                    "p95": f"{percentile(values, .95):.3f}",
                    "p99": (f"{percentile(values, .99):.3f}"
                            if len(values) >= 1000 else "NA"),
                    "stddev": (f"{statistics.stdev(values):.3f}"
                               if len(values) > 1 else "0"),
                    "min": min(values), "max": max(values),
                })
                writer.writerow(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", choices=("direct", "openwhisk"), required=True)
    parser.add_argument("--function", default="touch")
    parser.add_argument("--policy", default="mow")
    parser.add_argument("--mode", choices=("invoke", "restore", "cold"), default="invoke")
    parser.add_argument("--validation", choices=("fast", "full"), default="fast")
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--pool-size", type=int, default=4)
    parser.add_argument("--hold-ms", type=int, default=0)
    parser.add_argument("--control-host", default="172.17.0.1")
    parser.add_argument("--control-port", type=int, default=2301)
    parser.add_argument("--apihost")
    parser.add_argument("--auth")
    parser.add_argument("--wskprops", default="~/.wskprops")
    parser.add_argument("--action", default="cxlporter_proxy")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--run-id", default=f"control-{int(time.time())}")
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary")
    args = parser.parse_args()
    if args.path == "openwhisk" and (not args.apihost or not args.auth):
        props = load_wskprops(args.wskprops)
        args.apihost = args.apihost or props.get("APIHOST")
        args.auth = args.auth or props.get("AUTH")
    if args.samples < 1 or args.warmup < 0 or args.concurrency < 1:
        parser.error("samples/concurrency must be positive and warmup non-negative")

    for index in range(args.warmup):
        one_sample(args, args.run_id + "-warmup", index)
    rows = []
    if args.concurrency == 1:
        rows = [one_sample(args, args.run_id, i) for i in range(args.samples)]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
            futures = [pool.submit(one_sample, args, args.run_id, i)
                       for i in range(args.samples)]
            rows = [future.result() for future in futures]
        rows.sort(key=lambda row: row["request_id"])

    pathlib.Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=RESULT_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    summary = args.summary or os.path.splitext(args.output)[0] + "-summary.csv"
    write_summary(rows, summary)
    failures = sum(r["status"] != "0" or r["verify"] != "1" for r in rows)
    print(f"control_benchmark path={args.path} samples={len(rows)} failures={failures} "
          f"output={args.output} summary={summary}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
