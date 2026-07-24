#!/usr/bin/env python3
"""Summarize paper source-path CSVs without double-counting nested intervals."""

import argparse
import csv
import json
import math
import pathlib
import statistics


METRICS = [
    "client_total_ns",
    "queue_delay_ns",
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
]

GROUP_FIELDS = ["scenario", "path", "offered_rps", "concurrency"]


def percentile(values, fraction):
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def numeric(row, metric):
    value = row.get(metric, "NA")
    if value in ("", "NA", None):
        return None
    return int(value)


def summarize(rows):
    groups = {}
    for row in rows:
        key = tuple(row[field] for field in GROUP_FIELDS)
        groups.setdefault(key, []).append(row)
    records = []
    for key, grouped in sorted(groups.items()):
        failures = sum(
            row["status"] != "0"
            or row["verify"] != "1"
            or row["exclusive_valid"] != "1"
            for row in grouped
        )
        for metric in METRICS:
            values = [
                value
                for row in grouped
                if row["status"] == "0"
                and row["verify"] == "1"
                and row["exclusive_valid"] == "1"
                and (value := numeric(row, metric)) is not None
            ]
            if not values:
                continue
            record = dict(zip(GROUP_FIELDS, key))
            record.update(
                {
                    "metric": metric,
                    "samples": len(values),
                    "failures": failures,
                    "mean": f"{statistics.mean(values):.3f}",
                    "p50": f"{percentile(values, 0.50):.3f}",
                    "p95": f"{percentile(values, 0.95):.3f}",
                    "p99": (
                        f"{percentile(values, 0.99):.3f}"
                        if len(values) >= 1000
                        else "NA"
                    ),
                    "stddev": (
                        f"{statistics.stdev(values):.3f}"
                        if len(values) > 1
                        else "0.000"
                    ),
                    "min": str(min(values)),
                    "max": str(max(values)),
                    "unit": "ns",
                }
            )
            records.append(record)
        successful = [
            row
            for row in grouped
            if row["status"] == "0"
            and row["verify"] == "1"
            and row["exclusive_valid"] == "1"
        ]
        if successful:
            start = min(int(row["start_ns"]) for row in successful)
            end = max(
                int(row["start_ns"]) + int(row["client_total_ns"])
                for row in successful
            )
            elapsed = max(1, end - start)
            throughput = len(successful) * 1_000_000_000 / elapsed
            output = dict(zip(GROUP_FIELDS, key))
            output.update(
                {
                    "metric": "achieved_throughput_rps",
                    "samples": len(successful),
                    "failures": failures,
                    "mean": f"{throughput:.6f}",
                    "p50": "NA",
                    "p95": "NA",
                    "p99": "NA",
                    "stddev": "NA",
                    "min": "NA",
                    "max": "NA",
                    "unit": "requests/s",
                }
            )
            records.append(output)
    return records


def write_analysis(records, path):
    indexed = {
        (record["scenario"], record["metric"]): record for record in records
    }
    scenarios = sorted({record["scenario"] for record in records})
    lines = [
        "# Paper OpenWhisk benchmark 自动摘要",
        "",
        "所有 mean/P50/P95/stddev 均按成功样本统计；只有样本数不少于",
        "1000 时报告 P99。原始 interval 是嵌套关系，百分比只使用脚本计算的",
        "exclusive 字段，不能把 daemon/agent/fork/kernel 原始区间直接相加。",
        "",
        "| scenario | n | achieved RPS | client mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | kernel mean (us) | kernel/client |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario in scenarios:
        client = indexed.get((scenario, "client_total_ns"))
        kernel = indexed.get((scenario, "kernel_restore_ns"))
        throughput = indexed.get((scenario, "achieved_throughput_rps"))
        if not client or not kernel or not throughput:
            continue
        client_mean = float(client["mean"])
        kernel_mean = float(kernel["mean"])
        p99 = (
            f"{float(client['p99']) / 1e6:.3f}"
            if client["p99"] != "NA"
            else "NA"
        )
        lines.append(
            "| {scenario} | {samples} | {throughput:.2f} | {mean:.3f} | {p50:.3f} | "
            "{p95:.3f} | {p99} | {kernel:.3f} | {ratio:.3%} |".format(
                scenario=scenario,
                samples=client["samples"],
                throughput=float(throughput["mean"]),
                mean=client_mean / 1e6,
                p50=float(client["p50"]) / 1e6,
                p95=float(client["p95"]) / 1e6,
                p99=p99,
                kernel=kernel_mean / 1e3,
                ratio=kernel_mean / client_mean,
            )
        )
    lines.extend(
        [
            "",
            "解释边界：`direct-*` 是从宿主发起 CXLP/2 的控制面下界；",
            "`openwhisk-*` 覆盖 HTTP/controller/invoker adapter、QEMU hostfwd、",
            "VM1 daemon、Docker ghost、fork/exec 和内核 restore。二者使用相同",
            "catalog capsule 与相同 runtime proof，因此差值可以用于研究",
            "OpenWhisk/跨 OS 控制路径，但不是 CRIU 或 Mitosis 对照。",
            "",
        ]
    )
    pathlib.Path(path).write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("--output", required=True)
    parser.add_argument("--json-output")
    parser.add_argument("--analysis")
    args = parser.parse_args()
    rows = []
    for name in args.inputs:
        with open(name, newline="", encoding="utf-8") as stream:
            rows.extend(csv.DictReader(stream))
    records = summarize(rows)
    fields = GROUP_FIELDS + [
        "metric",
        "samples",
        "failures",
        "mean",
        "p50",
        "p95",
        "p99",
        "stddev",
        "min",
        "max",
        "unit",
    ]
    with open(args.output, "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)
    if args.json_output:
        pathlib.Path(args.json_output).write_text(
            json.dumps(records, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if args.analysis:
        write_analysis(records, args.analysis)
    print(
        f"paper_source_summary samples={len(rows)} records={len(records)} "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()
