#!/usr/bin/env python3
"""Collect auditable per-activation OpenWhisk state-action measurements."""

import argparse
import base64
import concurrent.futures
import csv
import json
import math
import pathlib
import statistics
import sys
import threading
import time
import urllib.parse
import http.client


EXPECTED_CHECKSUM = "05ae28486824d617"
EXPECTED_STATE_BYTES = 4 * 1024 * 1024

FIELDS = [
    "schema_version", "system", "vcpus", "rep", "mode", "scenario",
    "sample_index", "client_start_ns", "client_total_ns", "http_status",
    "success", "error", "activation_id", "wait_ns", "container_create_ns",
    "init_ns", "run_ns", "workload_ns", "container_id", "worker_pid",
    "worker_cpu", "state_bytes", "state_checksum", "init_count",
    "invocation", "request_id", "key", "slot_generation",
    "catalog_generation", "restore_id", "openwhisk_adapter_total_ns",
    "daemon_total_ns", "unix_connect_ns", "unix_roundtrip_ns",
    "daemon_to_agent_ns", "catalog_lookup_ns", "agent_total_ns",
    "fork_to_exit_ns", "kernel_restore_ns", "api_queue_schedule_ns",
    "handoff_ns", "worker_provision_ns", "state_activation_ns",
    "common_workload_ns", "completion_response_ns", "exclusive_sum_ns",
    "exclusive_delta_ns", "exclusive_valid", "mechanism_valid",
]

CXL_FIELDS = [
    "request_id", "key", "slot_generation", "catalog_generation",
    "restore_id", "openwhisk_adapter_total_ns", "daemon_total_ns",
    "unix_connect_ns", "unix_roundtrip_ns", "daemon_to_agent_ns",
    "catalog_lookup_ns", "agent_total_ns", "fork_to_exit_ns",
    "kernel_restore_ns",
]


def text(value, default="NA"):
    if value is None:
        return default
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def integer(value, default=None):
    if value in (None, "", "NA"):
        return default
    return int(value)


def annotations_map(activation):
    result = {}
    for item in activation.get("annotations", []):
        if isinstance(item, dict) and "key" in item:
            result[str(item["key"])] = item.get("value")
    return result


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100.0
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def invoke(args):
    parsed = urllib.parse.urlparse(args.apihost)
    connection_class = (
        http.client.HTTPSConnection
        if parsed.scheme == "https"
        else http.client.HTTPConnection
    )
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    action = urllib.parse.quote(args.action, safe="")
    path = f"/api/v1/namespaces/_/actions/{action}?blocking=true"
    auth = base64.b64encode(args.auth.encode("utf-8")).decode("ascii")
    connection = connection_class(parsed.hostname, port, timeout=args.timeout)
    try:
        connection.request(
            "POST",
            path,
            b"{}",
            {
                "Authorization": f"Basic {auth}",
                "Content-Type": "application/json",
            },
        )
        response = connection.getresponse()
        payload = response.read()
    finally:
        connection.close()
    decoded = json.loads(payload)
    return response.status, decoded


def base_row(args, index, started):
    row = {name: "NA" for name in FIELDS}
    row.update(
        {
            "schema_version": "1",
            "system": args.system,
            "vcpus": str(args.vcpus),
            "rep": str(args.rep),
            "mode": args.mode,
            "scenario": args.scenario,
            "sample_index": str(index),
            "client_start_ns": str(started),
            "success": "0",
            "exclusive_valid": "0",
            "mechanism_valid": "0",
        }
    )
    return row


def normal_mechanism_valid(args, row, result, annotations):
    common = (
        result.get("verify") is True
        and row["state_checksum"] == EXPECTED_CHECKSUM
        and integer(row["state_bytes"]) == EXPECTED_STATE_BYTES
        and integer(row["init_count"]) == 1
        and integer(row["worker_cpu"], -1) in args.target_cpu_set
    )
    if args.mode == "standard-cold":
        return (
            common
            and integer(row["invocation"]) == 1
            and annotations.get("benchForceCold") is True
            and row["container_create_ns"] != "NA"
            and row["init_ns"] != "NA"
        )
    if args.mode == "standard-warm":
        return (
            common
            and integer(row["invocation"], 0) >= 2
            and row["container_create_ns"] == "NA"
            and row["init_ns"] == "NA"
        )
    return False


def cxl_mechanism_valid(args, row, result):
    proof = result.get("runtime_proof", "")
    return (
        result.get("verify") in ("1", 1, True)
        and result.get("openwhisk_source_integrated") is True
        and row["state_checksum"] == EXPECTED_CHECKSUM
        and integer(row["state_bytes"]) == EXPECTED_STATE_BYTES
        and integer(row["init_count"]) == 1
        and integer(row["invocation"]) == 1
        and integer(row["restore_id"], 0) > 0
        and integer(row["slot_generation"], 0) > 0
        and integer(row["catalog_generation"], 0) > 0
        and integer(row["worker_pid"], 0) > 0
        and integer(row["worker_cpu"], -1) in args.target_cpu_set
        and "event=shared-leaf-proof" in proof
        and "event=target-continued" in proof
        and "markers=ok verify=ok" in proof
    )


def derive_breakdown(args, row):
    total = integer(row["client_total_ns"])
    workload = integer(row["workload_ns"])
    wait_ns = integer(row["wait_ns"], 0)
    if total is None or workload is None:
        return

    if args.mode.startswith("standard"):
        create_ns = integer(row["container_create_ns"], 0)
        init_ns = integer(row["init_ns"], 0)
        run_ns = integer(row["run_ns"])
        if run_ns is None:
            return
        api = max(0, wait_ns - create_ns)
        handoff = max(0, run_ns - workload)
        provision = create_ns
        state_activation = init_ns
    else:
        adapter = integer(row["openwhisk_adapter_total_ns"])
        fork_total = integer(row["fork_to_exit_ns"])
        kernel = integer(row["kernel_restore_ns"])
        if None in (adapter, fork_total, kernel):
            return
        api = wait_ns
        handoff = adapter - fork_total
        provision = fork_total - kernel - workload
        state_activation = kernel

    partial = api + handoff + provision + state_activation + workload
    completion = total - partial
    categories = [api, handoff, provision, state_activation, workload, completion]
    row.update(
        {
            "api_queue_schedule_ns": str(api),
            "handoff_ns": str(handoff),
            "worker_provision_ns": str(provision),
            "state_activation_ns": str(state_activation),
            "common_workload_ns": str(workload),
            "completion_response_ns": str(completion),
            "exclusive_sum_ns": str(sum(categories)),
            "exclusive_delta_ns": str(total - sum(categories)),
            "exclusive_valid": "1" if min(categories) >= 0 and sum(categories) == total else "0",
        }
    )


def one_sample(args, index):
    started = time.perf_counter_ns()
    row = base_row(args, index, started)
    raw = {"sample_index": index}
    try:
        status, activation = invoke(args)
        row["client_total_ns"] = str(time.perf_counter_ns() - started)
        row["http_status"] = str(status)
        raw["activation"] = activation
        if status != 200 or not isinstance(activation, dict):
            raise RuntimeError(f"OpenWhisk HTTP status={status}")
        if activation.get("response", {}).get("success") is not True:
            raise RuntimeError("OpenWhisk activation response is not successful")
        row["activation_id"] = text(activation.get("activationId"))
        annotations = annotations_map(activation)
        wait_ms = integer(annotations.get("waitTime"), 0)
        row["wait_ns"] = str(wait_ms * 1000000)
        result = activation.get("response", {}).get("result", {})
        if not isinstance(result, dict):
            raise RuntimeError("activation has no result object")

        row["workload_ns"] = text(result.get("workload_ns"))
        row["state_bytes"] = text(result.get("state_bytes"))
        row["state_checksum"] = text(result.get("state_checksum")).lower()
        row["init_count"] = text(result.get("init_count", result.get("source_initializations")))
        row["invocation"] = text(result.get("invocation"))
        row["worker_pid"] = text(result.get("worker_pid"))
        row["worker_cpu"] = text(result.get("worker_cpu"))
        row["container_id"] = text(result.get("container_id"))

        if args.mode.startswith("standard"):
            row["container_create_ns"] = text(annotations.get("benchContainerCreateNs"))
            row["init_ns"] = text(annotations.get("benchInitNs"))
            row["run_ns"] = text(annotations.get("benchRunNs"))
            valid = normal_mechanism_valid(args, row, result, annotations)
        else:
            for name in CXL_FIELDS:
                row[name] = text(result.get(name))
            valid = cxl_mechanism_valid(args, row, result)

        row["mechanism_valid"] = "1" if valid else "0"
        derive_breakdown(args, row)
        if valid and row["exclusive_valid"] == "1":
            row["success"] = "1"
            row["error"] = "NA"
        else:
            row["error"] = "mechanism-or-breakdown-validation"
    except Exception as error:
        if row["client_total_ns"] == "NA":
            row["client_total_ns"] = str(time.perf_counter_ns() - started)
        row["error"] = f"{type(error).__name__}:{error}"
        raw["exception"] = repr(error)
    raw["row"] = row
    return row, raw


def run_warmup(args):
    for index in range(args.warmup):
        row, _ = one_sample(args, -(index + 1))
        if row["success"] != "1":
            raise RuntimeError(f"warmup failed: {row['error']}")


def run_samples(args):
    if args.scenario == "sequential":
        return [one_sample(args, index) for index in range(args.requests)]

    deadline = time.perf_counter() + args.duration
    lock = threading.Lock()
    next_index = 0

    def worker():
        nonlocal next_index
        samples = []
        while time.perf_counter() < deadline:
            with lock:
                index = next_index
                next_index += 1
            samples.append(one_sample(args, index))
        return samples

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        for samples in executor.map(lambda _: worker(), range(args.concurrency)):
            results.extend(samples)
    results.sort(key=lambda item: int(item[0]["sample_index"]))
    return results


def validate_set(args, rows):
    successes = [row for row in rows if row["success"] == "1"]
    if len(successes) != len(rows):
        return False, f"{len(rows) - len(successes)} samples failed"
    if args.mode == "standard-cold":
        containers = [row["container_id"] for row in successes]
        if "NA" in containers or len(containers) != len(set(containers)):
            return False, "standard cold container IDs are not unique"
    if args.mode == "standard-warm":
        containers = {row["container_id"] for row in successes}
        if "NA" in containers or len(containers) != 1:
            return False, "standard warm samples did not reuse one container"
    if args.mode == "cxlfork":
        restore_ids = [row["restore_id"] for row in successes]
        if "NA" in restore_ids or len(restore_ids) != len(set(restore_ids)):
            return False, "CXLfork restore IDs are not unique"
    return True, "ok"


def write_outputs(args, samples, elapsed_ns):
    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    rows = [sample[0] for sample in samples]
    with (output / "samples.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    with (output / "samples.jsonl").open("w", encoding="utf-8") as handle:
        for _, raw in samples:
            handle.write(json.dumps(raw, sort_keys=True, separators=(",", ":")) + "\n")

    valid, reason = validate_set(args, rows)
    latencies = [integer(row["client_total_ns"]) for row in rows if row["success"] == "1"]
    summary = {
        "schema_version": 1,
        "system": args.system,
        "vcpus": args.vcpus,
        "rep": args.rep,
        "mode": args.mode,
        "scenario": args.scenario,
        "concurrency": args.concurrency,
        "samples": len(rows),
        "successes": len(latencies),
        "set_valid": valid,
        "set_validation": reason,
        "elapsed_ns": elapsed_ns,
        "achieved_rps": len(latencies) / (elapsed_ns / 1e9) if elapsed_ns else 0,
        "mean_ns": statistics.fmean(latencies) if latencies else None,
        "p50_ns": percentile(latencies, 50),
        "p95_ns": percentile(latencies, 95),
        "p99_ns": percentile(latencies, 99) if len(latencies) >= 500 else "NA",
        "stddev_ns": statistics.pstdev(latencies) if len(latencies) > 1 else 0,
        "expected_checksum": EXPECTED_CHECKSUM,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, sort_keys=True), flush=True)
    return valid


def parse_cpu_set(value):
    cpus = set()
    for part in value.split(","):
        if "-" in part:
            first, last = map(int, part.split("-", 1))
            cpus.update(range(first, last + 1))
        else:
            cpus.add(int(part))
    if not cpus:
        raise argparse.ArgumentTypeError("target CPU set is empty")
    return cpus


def summarize_main(arguments):
    parser = argparse.ArgumentParser(prog="state_action_benchmark.py summarize")
    parser.add_argument("root")
    parser.add_argument("--output")
    args = parser.parse_args(arguments)
    root = pathlib.Path(args.root)
    output = pathlib.Path(args.output) if args.output else root / "summary"
    output.mkdir(parents=True, exist_ok=True)
    groups = {}
    run_summaries = {}
    invalid = 0
    for path in root.rglob("samples.csv"):
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                key = (row["system"], int(row["vcpus"]), row["mode"], row["scenario"])
                groups.setdefault(key, []).append(row)
                if row["success"] != "1":
                    invalid += 1
    for path in root.rglob("summary.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        key = (data["system"], int(data["vcpus"]), data["mode"], data["scenario"])
        run_summaries.setdefault(key, []).append(data)

    summary_fields = [
        "system", "vcpus", "mode", "scenario", "reps", "samples",
        "successes", "mean_ns", "p50_ns", "p95_ns", "p99_ns",
        "stddev_ns", "achieved_rps_mean", "expected_checksum",
    ]
    breakdown_fields = [
        "system", "vcpus", "mode", "scenario", "samples",
        "api_queue_schedule_ns", "handoff_ns", "worker_provision_ns",
        "state_activation_ns", "common_workload_ns",
        "completion_response_ns", "exclusive_sum_ns",
    ]
    summaries = []
    breakdowns = []
    for key, rows in sorted(groups.items(), key=lambda item: (item[0][1], item[0][0], item[0][2], item[0][3])):
        successful = [row for row in rows if row["success"] == "1"]
        latency = [integer(row["client_total_ns"]) for row in successful]
        reps = {row["rep"] for row in rows}
        summaries.append(
            {
                "system": key[0], "vcpus": key[1], "mode": key[2],
                "scenario": key[3], "reps": len(reps), "samples": len(rows),
                "successes": len(successful),
                "mean_ns": statistics.fmean(latency) if latency else "NA",
                "p50_ns": percentile(latency, 50) if latency else "NA",
                "p95_ns": percentile(latency, 95) if latency else "NA",
                "p99_ns": percentile(latency, 99) if len(latency) >= 500 else "NA",
                "stddev_ns": statistics.pstdev(latency) if len(latency) > 1 else 0,
                "achieved_rps_mean": statistics.fmean(
                    item["achieved_rps"] for item in run_summaries.get(key, [])
                ) if run_summaries.get(key) else "NA",
                "expected_checksum": EXPECTED_CHECKSUM,
            }
        )
        category_names = breakdown_fields[5:-1]
        category_means = {
            name: statistics.fmean(integer(row[name]) for row in successful)
            if successful else "NA"
            for name in category_names
        }
        breakdowns.append(
            {
                "system": key[0], "vcpus": key[1], "mode": key[2],
                "scenario": key[3], "samples": len(successful),
                **category_means,
                "exclusive_sum_ns": sum(category_means.values()) if successful else "NA",
            }
        )

    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=summary_fields)
        writer.writeheader()
        writer.writerows(summaries)
    with (output / "breakdown.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=breakdown_fields)
        writer.writeheader()
        writer.writerows(breakdowns)
    manifest = {
        "groups": len(groups),
        "raw_csv_files": len(list(root.rglob("samples.csv"))),
        "invalid_samples": invalid,
        "expected_checksum": EXPECTED_CHECKSUM,
    }
    (output / "summary-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, sort_keys=True))
    return 0 if groups and invalid == 0 else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("standard-cold", "standard-warm", "cxlfork"), required=True)
    parser.add_argument("--scenario", choices=("sequential", "throughput"), required=True)
    parser.add_argument("--system", required=True)
    parser.add_argument("--vcpus", type=int, required=True)
    parser.add_argument("--rep", type=int, required=True)
    parser.add_argument("--action", required=True)
    parser.add_argument("--apihost", default="http://127.0.0.1:3233")
    parser.add_argument("--auth", required=True)
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--target-cpus", type=parse_cpu_set, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    args.target_cpu_set = args.target_cpus

    run_warmup(args)
    started = time.perf_counter_ns()
    samples = run_samples(args)
    elapsed = time.perf_counter_ns() - started
    return 0 if write_outputs(args, samples, elapsed) else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "summarize":
        raise SystemExit(summarize_main(sys.argv[2:]))
    raise SystemExit(main())
