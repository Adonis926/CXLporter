#!/usr/bin/env python3
"""Measure the CXLP/2 data path directly and through OpenWhisk.

Every successful sample retains mechanism proof and computes an exclusive
latency decomposition. Raw protocol responses are written separately so the
CSV stays compact without losing auditability.
"""

import argparse
import base64
import concurrent.futures
import csv
import hashlib
import http.client
import json
import pathlib
import re
import socket
import sys
import time
import urllib.parse


FIELDS = [
    "schema_version",
    "run_id",
    "scenario",
    "sample_index",
    "path",
    "offered_rps",
    "concurrency",
    "scheduled_ns",
    "start_ns",
    "queue_delay_ns",
    "client_total_ns",
    "http_status",
    "status",
    "verify",
    "error_stage",
    "request_id",
    "activation_id",
    "key",
    "slot_generation",
    "catalog_generation",
    "restore_id",
    "openwhisk_adapter_total_ns",
    "daemon_total_ns",
    "unix_connect_ns",
    "unix_roundtrip_ns",
    "daemon_to_agent_ns",
    "catalog_lookup_ns",
    "fork_to_exit_ns",
    "kernel_restore_ns",
    "agent_total_ns",
    "outer_control_ns",
    "adapter_outside_daemon_ns",
    "daemon_other_ns",
    "unix_transport_ns",
    "agent_other_ns",
    "fork_runtime_other_ns",
    "exclusive_sum_ns",
    "exclusive_delta_ns",
    "exclusive_valid",
    "proof_sha256",
    "proof_shared_leaf",
    "proof_fd_lifetime",
    "proof_continued",
    "source_initializations",
    "invocation",
]

NUMERIC_RESPONSE_FIELDS = [
    "key",
    "slot_generation",
    "catalog_generation",
    "restore_id",
    "openwhisk_adapter_total_ns",
    "daemon_total_ns",
    "unix_connect_ns",
    "unix_roundtrip_ns",
    "daemon_to_agent_ns",
    "catalog_lookup_ns",
    "fork_to_exit_ns",
    "kernel_restore_ns",
    "agent_total_ns",
]


def text_value(value):
    if value is None:
        return "NA"
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def parse_response_line(line):
    words = line.strip().split()
    if len(words) < 2 or words[0] != "CXLP/2" or words[1] not in ("OK", "ERROR"):
        raise ValueError(f"invalid CXLP/2 response: {line[:160]}")
    fields = {}
    for word in words[2:]:
        key, separator, value = word.partition("=")
        if not separator or not key or key in fields:
            raise ValueError(f"invalid CXLP/2 field: {word[:160]}")
        fields[key] = value
    return fields


def read_line(sock, limit=32768):
    data = bytearray()
    while b"\n" not in data:
        chunk = sock.recv(8192)
        if not chunk:
            raise RuntimeError("CXLP/2 peer closed before newline")
        data.extend(chunk)
        if len(data) > limit:
            raise RuntimeError("CXLP/2 response exceeds limit")
    return bytes(data).split(b"\n", 1)[0].decode("utf-8")


def direct_call(args, request_id):
    line = (
        f"CXLP/2 ACTIVATE {request_id} "
        f"{args.owner} {args.function} {args.version}\n"
    )
    with socket.create_connection(
        (args.control_host, args.control_port), timeout=args.timeout
    ) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(line.encode("ascii"))
        response_line = read_line(sock)
    result = parse_response_line(response_line)
    raw = {"request": line.rstrip(), "response": response_line}
    return result, raw, "NA"


def openwhisk_call(args):
    parsed = urllib.parse.urlparse(args.apihost)
    connection_class = (
        http.client.HTTPSConnection
        if parsed.scheme == "https"
        else http.client.HTTPConnection
    )
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    auth = base64.b64encode(args.auth.encode("utf-8")).decode("ascii")
    action = urllib.parse.quote(args.action, safe="")
    path = (
        f"/api/v1/namespaces/_/actions/{action}"
        "?blocking=true&result=true"
    )
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
    if response.status != 200:
        raise RuntimeError(
            f"OpenWhisk HTTP {response.status}: {payload[:200]!r}"
        )
    if "error" in decoded and isinstance(decoded["error"], dict):
        result = decoded["error"]
    else:
        result = decoded
    raw = {"http_status": response.status, "response": decoded}
    return result, raw, str(response.status)


def decode_proof(result):
    proof = result.get("runtime_proof")
    if proof is None:
        encoded = result.get("output_hex", "")
        try:
            proof = bytes.fromhex(str(encoded)).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            proof = ""
    return str(proof)


def proof_number(proof, name):
    matches = re.findall(rf"(?:^|\s){re.escape(name)}=(\d+)", proof)
    return matches[-1] if matches else "NA"


def integer(row, name):
    value = row.get(name, "NA")
    if value in ("", "NA", None):
        raise ValueError(f"missing {name}")
    return int(value)


def derive_exclusive(row):
    client = integer(row, "client_total_ns")
    daemon = integer(row, "daemon_total_ns")
    unix_connect = integer(row, "unix_connect_ns")
    unix_roundtrip = integer(row, "unix_roundtrip_ns")
    agent = integer(row, "agent_total_ns")
    catalog = integer(row, "catalog_lookup_ns")
    fork_total = integer(row, "fork_to_exit_ns")
    kernel = integer(row, "kernel_restore_ns")

    if row["path"] == "openwhisk":
        adapter = integer(row, "openwhisk_adapter_total_ns")
        outer = client - adapter
        adapter_other = adapter - daemon
    else:
        outer = client - daemon
        adapter_other = 0

    daemon_other = daemon - unix_connect - unix_roundtrip
    unix_transport = unix_roundtrip - agent
    agent_other = agent - catalog - fork_total
    fork_other = fork_total - kernel
    exclusive = [
        outer,
        adapter_other,
        daemon_other,
        unix_connect,
        unix_transport,
        catalog,
        agent_other,
        fork_other,
        kernel,
    ]
    total = sum(exclusive)
    row.update(
        {
            "outer_control_ns": str(outer),
            "adapter_outside_daemon_ns": (
                str(adapter_other) if row["path"] == "openwhisk" else "NA"
            ),
            "daemon_other_ns": str(daemon_other),
            "unix_transport_ns": str(unix_transport),
            "agent_other_ns": str(agent_other),
            "fork_runtime_other_ns": str(fork_other),
            "exclusive_sum_ns": str(total),
            "exclusive_delta_ns": str(client - total),
            "exclusive_valid": (
                "1" if all(value >= 0 for value in exclusive) and total == client else "0"
            ),
        }
    )


def request_id(run_id, index):
    prefix = int(hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:8], 16)
    return str(((prefix << 32) | ((index + 1) & 0xFFFFFFFF)) & 0xFFFFFFFFFFFFFFFF)


def one_sample(args, index, scheduled_ns):
    row = {field: "NA" for field in FIELDS}
    start_ns = time.perf_counter_ns()
    row.update(
        {
            "schema_version": "1",
            "run_id": args.run_id,
            "scenario": args.scenario,
            "sample_index": str(index),
            "path": args.path,
            "offered_rps": str(args.offered_rps),
            "concurrency": str(args.concurrency),
            "scheduled_ns": str(scheduled_ns),
            "start_ns": str(start_ns),
            "queue_delay_ns": str(max(0, start_ns - scheduled_ns)),
            "status": "client_error",
            "verify": "0",
            "error_stage": "client",
        }
    )
    raw = {"sample_index": index, "path": args.path}
    try:
        if args.path == "direct":
            requested_id = request_id(args.run_id, index)
            result, protocol_raw, http_status = direct_call(args, requested_id)
            row["request_id"] = requested_id
        else:
            result, protocol_raw, http_status = openwhisk_call(args)
        end_ns = time.perf_counter_ns()
        row["client_total_ns"] = str(end_ns - start_ns)
        row["http_status"] = http_status
        raw.update(protocol_raw)

        for name in NUMERIC_RESPONSE_FIELDS:
            if name in result:
                row[name] = text_value(result[name])
        row["request_id"] = text_value(result.get("request_id", row["request_id"]))
        row["activation_id"] = text_value(
            result.get("openwhisk_activation_id", "NA")
        )
        row["status"] = text_value(result.get("status", "NA"))
        row["verify"] = text_value(result.get("verify", "NA"))
        row["error_stage"] = text_value(result.get("error_stage", "NA"))

        proof = decode_proof(result)
        row["proof_sha256"] = hashlib.sha256(proof.encode("utf-8")).hexdigest()
        row["proof_shared_leaf"] = (
            "1" if "event=shared-leaf-proof" in proof and "verify=ok" in proof else "0"
        )
        row["proof_fd_lifetime"] = (
            "1" if "event=context-after-fd-close" in proof else "0"
        )
        row["proof_continued"] = (
            "1"
            if "event=target-continued" in proof and "markers=ok verify=ok" in proof
            else "0"
        )
        row["source_initializations"] = proof_number(proof, "source_inits")
        row["invocation"] = proof_number(proof, "invocation")

        if (
            row["status"] != "0"
            or row["verify"] != "1"
            or row["proof_shared_leaf"] != "1"
            or row["proof_fd_lifetime"] != "1"
            or row["proof_continued"] != "1"
        ):
            row["error_stage"] = "mechanism-proof"
        else:
            derive_exclusive(row)
            if row["exclusive_valid"] != "1":
                row["status"] = "measurement_error"
                row["verify"] = "0"
                row["error_stage"] = "exclusive-breakdown"
    except Exception as error:  # Keep failures as raw samples.
        row["client_total_ns"] = str(time.perf_counter_ns() - start_ns)
        row["error_stage"] = f"client:{type(error).__name__}"
        raw["exception"] = repr(error)
    raw["row"] = row
    return row, raw


def sleep_until(deadline_ns):
    while True:
        remaining = deadline_ns - time.perf_counter_ns()
        if remaining <= 0:
            return
        time.sleep(min(remaining / 1_000_000_000, 0.01))


def execute(args, count, run_id_suffix=""):
    original_run_id = args.run_id
    if run_id_suffix:
        args.run_id = original_run_id + run_id_suffix
    rows = []
    raws = []
    epoch = time.perf_counter_ns() + 100_000_000
    interval_ns = (
        int(1_000_000_000 / args.offered_rps)
        if args.offered_rps > 0
        else 0
    )

    if args.concurrency == 1 and interval_ns == 0:
        for index in range(count):
            scheduled = time.perf_counter_ns()
            row, raw = one_sample(args, index, scheduled)
            rows.append(row)
            raws.append(raw)
    else:
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.concurrency
        ) as executor:
            futures = []
            for index in range(count):
                scheduled = (
                    epoch + index * interval_ns
                    if interval_ns
                    else time.perf_counter_ns()
                )
                if interval_ns:
                    sleep_until(scheduled)
                futures.append(
                    executor.submit(one_sample, args, index, scheduled)
                )
            for future in futures:
                row, raw = future.result()
                rows.append(row)
                raws.append(raw)
    args.run_id = original_run_id
    rows.sort(key=lambda item: int(item["sample_index"]))
    raws.sort(key=lambda item: item["sample_index"])
    return rows, raws


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", choices=("direct", "openwhisk"), required=True)
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--offered-rps", type=float, default=0)
    parser.add_argument("--run-id", default=f"paper-source-{int(time.time())}")
    parser.add_argument("--output", required=True)
    parser.add_argument("--raw-output")
    parser.add_argument("--control-host", default="172.17.0.1")
    parser.add_argument("--control-port", type=int, default=2501)
    parser.add_argument("--owner", default="paper")
    parser.add_argument("--function", default="capsule-k0")
    parser.add_argument("--version", default="v1")
    parser.add_argument("--apihost", default="http://127.0.0.1:3433")
    parser.add_argument(
        "--auth",
        default=(
            "23bc46b1-71f6-4ed5-8c54-816aa4f8c502:"
            "123zO3xZCLrMN6v2BKK1dXYFpXlPkccOFqm12CdAsMgRU4VrNZ9lyGVCGuMDGIwP"
        ),
    )
    parser.add_argument("--action", default="paper_cxlfork_source")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()

    if (
        args.samples < 1
        or args.warmup < 0
        or args.concurrency < 1
        or args.offered_rps < 0
    ):
        parser.error("invalid samples, warmup, concurrency, or offered-rps")
    if args.samples < 1000:
        print(
            "note: fewer than 1000 samples; summarizer will report P99 as NA",
            file=sys.stderr,
        )

    if args.warmup:
        execute(args, args.warmup, run_id_suffix="-warmup")
    rows, raws = execute(args, args.samples)
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_output = pathlib.Path(
        args.raw_output or str(output.with_suffix("")) + "-raw.jsonl"
    )
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    with raw_output.open("w", encoding="utf-8") as stream:
        for item in raws:
            stream.write(json.dumps(item, sort_keys=True) + "\n")

    failures = sum(
        row["status"] != "0"
        or row["verify"] != "1"
        or row["exclusive_valid"] != "1"
        for row in rows
    )
    print(
        f"paper_source_benchmark scenario={args.scenario} path={args.path} "
        f"samples={len(rows)} failures={failures} output={output} raw={raw_output}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
