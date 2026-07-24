# OpenWhisk 主路径性能测试

本测试不补造 CRIU-CXL 或 Mitosis-CXL 对照。它回答当前科研系统最重要的
问题：同一份已经证明 shared leaf、fd-lifetime 和 continuation 的
CXLfork capsule，从宿主直接发出 CXLP/2 请求要多久；把同一请求放进
OpenWhisk/controller/invoker 激活路径后又要多久；150 RPS burst 下尾部
排队出现在何处；内核 restore 在完整路径中占多大比例。

## 场景

| 场景 | 含义 |
|---|---|
| `direct-sequential` | 宿主到 VM1 CXLP/2 的闭环单请求下界 |
| `openwhisk-sequential` | HTTP → OpenWhisk → CXLP/2 → ghost → restore |
| `direct-150rps` | 直接路径，150 RPS open-loop burst |
| `openwhisk-150rps` | 完整 OpenWhisk 路径，150 RPS open-loop burst |

默认每个场景 1,000 个正式样本、20 个 warmup。只有正式成功样本达到
1,000 时才输出 P99；否则 P99 为 `NA`。同时输出 mean、P50、P95 和
标准差。

Paper action 显式使用 `logsize=0`，因为 CXLP/2 已在响应中返回 capsule
输出和 proof，OpenWhisk 宿主上不存在与逻辑 CXLfork container ID 对应的
Docker stdout；若保留普通 Docker log collector，它会把成功的逻辑容器
误判为失效。action 默认按 OpenWhisk 最小实验额度记账为 128 MiB，ghost
内部使用 16 个固定 worker 并发受理 restore；这不是 16 份 checkpoint，
它们共同查询同一个 catalog capsule，并为每次 activation 创建独立进程。
OpenWhisk 入口并发上限设为 128，高于负载发生器的 64 个并发连接，以免
150 RPS 实验被控制器的默认 30 请求限流人为截断。

## 计时边界

原始字段是嵌套区间，不能相加。例如 `daemon_total_ns` 包含
`agent_total_ns`，而 `agent_total_ns` 又包含 `fork_to_exit_ns` 和
`kernel_restore_ns`。采集器为每行额外计算如下 exclusive 分项：

- `outer_control_ns`：direct 的宿主 TCP 外围，或 OpenWhisk HTTP/controller
  到 adapter 之外的部分；
- `adapter_outside_daemon_ns`：OpenWhisk adapter/hostfwd 中不在 daemon
  内的部分；
- `daemon_other_ns`：不在 Unix connect/roundtrip 内的 daemon 工作；
- `unix_connect_ns` 和 `unix_transport_ns`；
- `catalog_lookup_ns`；
- `agent_other_ns`；
- `fork_runtime_other_ns`：fork/exec/runtime 中除 kernel restore 的部分；
- `kernel_restore_ns`。

这些 exclusive 项之和必须逐样本严格等于 `client_total_ns`，否则该样本
标为 measurement error。`daemon_to_agent_ns` 仅作为诊断时间戳，不加入
exclusive 求和。

## 运行

先完成 Paper VM、Docker ghost 和源码 OpenWhisk 部署，然后：

```bash
cd /home/xtl/cxlfork/cxlfork-repro/cxlporter
openwhisk/scripts/55-benchmark-paper-source.sh
```

快速调试：

```bash
SAMPLES=20 BURST_SAMPLES=30 WARMUP=2 \
  openwhisk/scripts/55-benchmark-paper-source.sh
```

快速调试不会报告 P99。正式论文采集保持默认 1,000 样本。

## 输出

结果目录为主仓库
`results/paper-system/paper-source-<timestamp>/`：

- 四个 CSV：逐样本紧凑字段；
- 四个 `*-raw.jsonl`：完整 CXLP/2/OpenWhisk 原始响应和 proof；
- `summary.csv` / `summary.json`：统计结果；
- `analysis.md`：自动生成的 mean/P50/P95/P99/标准差、排他 breakdown
  和 kernel/client 比例；
- `environment.txt` / `openwhisk-build-metadata.txt`：提交和工具链；
- `openwhisk-run.log`：只截取本轮 OpenWhisk 日志，禁止出现逻辑
  container 的 Docker log 误收集、warm container 失效和 HTTP 429；
- `postflight-audit.txt`：ghost worker 数、slot `readers=0` 和容器进程表；
- `dmesg-*.log`、Docker/factory 快照与 `SHA256SUMS`。

所有成功样本都要求同时满足：

- `status=0`、`verify=1`；
- `shared-leaf-proof`；
- `context-after-fd-close`；
- `target-continued` 且 markers 正确；
- exclusive breakdown 完整且不重复。

普通 OpenWhisk Node.js action 可以作为额外的 runtime 参考，但其函数
语义、预热状态和执行工作不同，因此不放进本主矩阵，也不与 CXLfork
柱子直接相减。当前 QEMU 文件 backend 结果用于软件控制路径和趋势，
不能解释为真实 CXL fabric latency/带宽。
