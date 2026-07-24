# OpenWhisk 源码级 CXLP/2 集成

本路径固定在 Apache OpenWhisk 提交
`7ef091c12714b40d5dd93ff1488b086f0cebf3f8`。补丁位于
`openwhisk/patches/0001-invoker-add-source-integrated-CXLP2-container-path.patch`。
独立源码仓库的开发分支为 `cxlfork-paper-system-reproduction`。

带有下列 annotation 的 executable action 不会进入普通 action runtime
预热容器，而是由 invoker 创建 `CxlporterContainer`：

```text
cxlfork=true
cxlfork-owner=paper
cxlfork-function=capsule-k0
cxlfork-version=v1
```

`CxlporterContainer.run()` 使用 activation ID 派生 CXLP/2 request ID，向
`172.17.0.1:2501` 发出 `ACTIVATE`。该 hostfwd 到达 VM1 的
`cxlporter-paper.service`，再经 Unix `SOCK_SEQPACKET` 到 Docker ghost；
ghost fork/exec K0 capsule runtime 并调用内核 `RESUME_V4`。返回值中保留
OpenWhisk、adapter、daemon、agent、fork/exit 和 kernel restore 各层计时，
以及共享 leaf/data PFN 和 continuation proof。

普通、未标注的 action 仍走原有 Docker container 路径。配置默认
`enabled=false`，只有 Paper profile 显式设置 `CXLPORTER_ENABLED=true`
时才启用 factory 分支。

## 人工运行

```bash
cd /home/xtl/cxlfork/cxlfork-repro/cxlporter
openwhisk/scripts/50-build-paper-source.sh
openwhisk/scripts/51-start-paper-source.sh
scripts/paper-system/31-prepare-paper-capsule.sh 2472402
START=0 openwhisk/scripts/53-deploy-paper-source-action.sh
openwhisk/scripts/54-smoke-paper-source.sh
```

默认 smoke 复用已注册的 key `2472401`，以避免把 catalog collision
保护误当成可覆盖更新。全新环境应先执行 `31-prepare-paper-capsule.sh`；
仅当该 identity 为空时，也可以用 `PREPARE=1 KEY=<new-key>` 让 smoke
自行准备。切换已有 identity 的 key 必须先显式 evict 旧 catalog 并释放
旧 slot。

停止：

```bash
openwhisk/scripts/52-stop-paper-source.sh
```

源码构建要求 JDK 11 和 Node/npm。focused ScalaTest 需要排除 OpenWhisk
tests 模块中与本测试无关的 `buildArtifacts`，因为它会构建所有语言的
测试 action，并额外依赖宿主 `zip` 命令：

```bash
./gradlew --no-daemon :tests:test \
  -x :tests:buildArtifacts \
  --tests org.apache.openwhisk.core.containerpool.cxlfork.CxlporterContainerTests
```

`54-smoke-paper-source.sh` 同时验证：

- 两次真实 source-integrated restore；
- 缺失 catalog identity 返回明确失败；
- 未标注 action 仍走普通 Docker runtime；
- annotated action 没有进入 `DockerContainer.run`；
- 两台 Paper VM 的 dmesg 无 BUG/WARNING/Oops/CXLfork error。

原始 JSON、factory markers、Docker 快照、完整 dmesg 和验收日志写入
主仓库 `results/paper-system/<timestamp>-openwhisk-source/`。
