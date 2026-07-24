# CXLfork 论文复现专用 CXLP/2 与 Docker ghost

本文描述 `cxlfork-paper-system-reproduction` 分支新增的控制路径。它与旧
CXLP/1/Cohort 路径并存，不替换旧 `cxlporter` 和 `cxlporter-control`。

## 组件与权限边界

1. `cxlporter-paper` 运行在目标 guest（paper VM1）的普通 `ubuntu`
   用户下，监听 TCP 7100。它只负责解析 CXLP/2、转发请求和记录控制路径
   时间，不打开 `/dev/cxlfork`。
2. `cxlfork-ghost-paper` 是真实 Docker 容器。容器使用离线
   `FROM scratch` 镜像、`--network none`，只有该容器通过 Docker
   `--device` 获得 `/dev/cxlfork`。
3. 容器 PID1 是 `cxlfork-ghost-agent`。它常驻并监听 bind mount 中的
   Unix `SOCK_SEQPACKET` socket。ACTIVATE 到来时，它查询共享 CXL catalog，
   fork 一个子进程，并 exec `cxlfork-capsule-runtime`。
4. capsule runtime 是 kernel K0 selftest 源码构建出的同一个静态、
   non-PIE ELF。VM0 用它 prepare/register，VM1 容器用同一 ELF restore，
   因而 continuation 的固定代码地址一致。

这是一种受限但真实的 ghost-container 路径：Docker 容器和容器内 fork
都真实发生；它不是 `docker exec` 或每请求创建容器。它仍不是任意进程的
透明 remote fork，恢复对象是已封存的协作式 C 函数 capsule。

## 控制协议

host 到 VM1 使用一行文本协议：

```text
CXLP/2 PING REQUEST_ID
CXLP/2 POOL_STATUS REQUEST_ID
CXLP/2 ACTIVATE REQUEST_ID OWNER FUNCTION VERSION
```

当前 K0 identity 是 `paper capsule-k0 v1`。VM1 daemon 到 Docker ghost
使用定长二进制结构，包含 magic、version、operation、request ID 和
catalog identity；未携带用户指针。

响应是一行 `CXLP/2 OK|ERROR key=value...`，包含：

- daemon 收到请求、Unix connect/round-trip 和 daemon total；
- agent 收到请求、catalog lookup、fork-to-exit 和 agent total；
- kernel V4 restore time/restore ID；
- key、slot generation、catalog generation；
- child status、verify 和十六进制编码的原始 runtime 证明。

`kernel_restore_ns` 已包含 V4 core、capsule validation 和 activation
commit；它不能再与这些子项重复相加。`fork_to_exit_ns` 包含进程创建、
exec、V4 restore、trampoline、continuation invocation 17、验证和退出。

## 构建与运行

先启动并 provision paper VM，然后在主仓库根目录运行：

```bash
./cxlporter/scripts/paper-system/30-deploy-paper-ghost.sh
./cxlporter/scripts/paper-system/32-smoke-paper-cxlp2.sh 56001
```

第一条命令离线构建静态 ghost/runtime 和 scratch Docker 镜像，启动容器
及 `cxlporter-paper.service`。第二条命令在 VM0 prepare/register，一个
checkpoint 经 host-forwarded CXLP/2 路径在 VM1 连续激活两次，最后
evict/release。

单独 prepare 可运行：

```bash
./cxlporter/scripts/paper-system/31-prepare-paper-capsule.sh KEY
```

手动请求示例：

```bash
./cxlporter/cxlporter-paper client 172.17.0.1 2501 PING 1
./cxlporter/cxlporter-paper client 172.17.0.1 2501 POOL_STATUS 2
./cxlporter/cxlporter-paper client 172.17.0.1 2501 \
  ACTIVATE 3 paper capsule-k0 v1
```

## 当前有意保留的边界

- ghost pool 当前是一个常驻容器，容器内每请求 fork；不实现生产级动态
  扩缩容、排队策略和故障重建。
- CXLP/2 当前只覆盖 ping、pool status 和 activate；catalog
  prepare/register/evict 由实验管理脚本显式执行。
- runtime 仍是 K0 协作式 continuation，不恢复任意线程、socket、
  namespace 或完整 OpenWhisk action 进程。
- `output_hex` 是 correctness 证据，正式高频 benchmark 可选择关闭或
  从计时区间外写盘，避免日志 I/O 污染关键路径。
