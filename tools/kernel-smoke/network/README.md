# 独立 ARM 网络内核契约 smoke

本目录提供父流程 **VExpress QEMU / `-pw3-core-smoke` 内核**使用的独立 userspace smoke，不用于 Kindle。它验证 namespace 隔离、8 项 xtables revision、NFLOG socket、qtaguid 只读 stat 和两个 sysctl。

**怎么跑**：不单独跑，由 `tools/build-kernel-smoke.sh` 编、`tools/run-kernel-smoke.sh` 在 QEMU 里跑，后者从输出里取 `network-smoke SUMMARY: checks=N failed=M` 记进回执。发行清单第 3a 步要求这一套全过。

一个踩过的坑留在这里：`create-dummy-0` 曾因测试报文用了较新的 nested type flag，被 3.0.35 的 rtnetlink 预解析器拒成 EINVAL；改法是把顶层 `IFLA_LINKINFO` 编码成裸类型 18，共用 builder 上有回归检查钉着。**本套件不覆盖**的：UID 路由、IPv6 表选择、收到的 RA、Wi-Fi、以及 Android `netd`。

程序文件：

- `network-smoke.c`：Linux syscall/netlink 测试入口。
- `netlink-wire.c`、`netlink-wire.h`：固定大小的 UAPI 消息构造与边界检查，没有依赖较新内核内部头文件。
- `isolation-guard.h`：与 Linux 3.0 ABI 匹配的接口清单守卫，无 namespace inode/符号链接推断。
- `wire-selftest.c`：可在主机或 ARM 执行的报文布局/边界和接口隔离策略检查；不执行 Linux 网络 syscalls。
- `build-arm.sh`：仅编译上面两个静态程序，不构建内核、不改父构建树。
- `required.config`：本 smoke 额外所需配置。

## 隔离与运行限制

运行必须显式传 `--run-isolated`，并同时满足 root、uname release 含 `-pw3-core-smoke`、ARM CPU 和 `/proc/cpuinfo` 中 VExpress 平台标识。还要求 `CONFIG_MODULES=n`（用 `/proc/modules` 不存在检查）：xtables revision 查询和 dummy kind 查找在旧内核上可能尝试 request_module；禁用模块避免这些查询在 namespace 之外成功加载全局模块。

父进程首先通过新建的 netlink socket 做只读 `RTM_GETLINK`，要求初始网络含 `lo` 与 builtin DUMMY 创建的 `dummy0`，且没有 `smk0/smk1`。随后成功 `clone(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD)`；子进程在任何修改前确认 PID=1、`mnt` symlink 与父不同，并用子进程当前 netns 中新开的 socket 再做只读 link dump。子集合必须是 `lo`，或 `lo + DOWN sit0`；后者来自本版 `CONFIG_IPV6_SIT` 的 per-net 初始化。父 `dummy0`、测试名字或其它接口均不得出现。通过后才改 mount propagation、挂 tmpfs/proc 和修改 link/address/route/rule/sysctl。测试只使用自建的两个 dummy interface，不启用备用 `sit0`、物理 NIC、无线、veth 或 TUN，不使用 DNS或外网，不创建持久文件。

0300 补丁只为 `mnt` 实现 `.inum` 与 symlink；net/uts/ipc 仍是 3.0 regular-file ABI。**本修订不对 net 文件调用 readlink，也不把不同 PID 的 proc inode 当成 namespace 身份。** 网络隔离依据为成功的 `CLONE_NEWNET` 加上父/子实际接口差异；子退出后，父再次 dump，按 name/index/type 检查接口成员与原始基线相同。这个结束检查准确表述为“父接口成员未改变”，不虚称获得了旧内核未提供的 netns ID。

UID socket 测试在子 namespace 内再 fork，改变该子进程 UID/GID，调用 UDP `connect()` 和 `getsockname()` 检查所选源地址；没有 send application payload。它与仅发送带 `RTA_UID` 的 route-get 请求是两项分开的证据。子 namespace 有 60 秒 userspace watchdog，父 QEMU 运行器仍需整体超时，因为 userspace watchdog不能保证打断内核死锁。

qtaguid 有一个明确例外说明：导入的旧驱动把 `/proc/net/xt_qtaguid` 注册到 init_net。程序只在 clone 之前对 ctrl 节点做只读 `stat`，然后在新 namespace 查询真正注册的 `owner` revision 1；不写 ctrl、不安装 accounting rule、不读写全局 quota 值。不存在一个另名 `qtaguid` 的 xtables match 可供查询。

## 当前覆盖的契约

| 检查 | 检查能证明什么 | 不证明什么 |
|---|---|---|
| `socket(AF_NETLINK, NETLINK_NETFILTER)` | 底层 NFNETLINK socket 协议可创建 | NFLOG group subscribe、日志事件或完整 netd 启动 |
| 双栈 owner rev1、quota2 rev3、IDLETIMER target rev1、NFLOG target rev0 查询 | 指定 family/revision 已注册 | 计数准确性、quota 告警、idle 通知时序、iptables 规则成功安装 |
| qtaguid ctrl 节点只读 stat + owner rev1 | qtaguid provider 的接口出现且 owner ABI 注册 | tagging、stats、Android 网络策略已正确执行 |
| 双栈 UID rule add/dump | `FRA_UID_RANGE=20` 的 start/end 与扩展表号实际 round-trip | 仅 add 成功本身不足以证明 UID 被用于匹配 |
| reversed/invalid UID/短长 payload reject | 错误范围/ABI 没有被静默当作 unrestricted rule | 所有 netlink 错误路径已覆盖 |
| mismatched range/table delete 拒绝；exact delete 后 dump 缺席 | delete 同时使用 UID range 和 `FRA_TABLE`，不忽略高于 255 的表号 | 并发规则修改或重启恢复 |
| root-only rule、两段 app UID 的 RTA_UID route lookup，范围外不可达 | UID 路由选择按范围和表生效 | 新 `kuid_t`/user-namespace 语义、完整 VPN |
| 实际 UID=10000/10010 的 UDP connect + 源地址，A→B→A | socket UID 数据流与不同 UID 路由选择；能发现部分 IPv4 cache key 混用 | 真实数据传输、TCP 生命周期、PMTU、VPN、Wi-Fi 或 TLS |
| 无 UID selector 的 rule dump 不出现 UID attr | unrestricted rule 编码保留该语义 | 任意自定义 policy rule 的全部语义 |
| `accept_ra_rt_table` default 继承、负 offset、正 table、0/main、改值不迁移旧 prefix | 通过 explicit IPv6 address 调用共享 `addrconf_prefix_route()`，验证表号选择及实际路由表内容 | **没有接收 RA packet**；不验证 PIO/RIO/default RA 更新、撤销、过期或多接口 RA 竞态 |

故意采用 source-grounded errno：短 UID range payload 在 PW3 `lib/nlattr.c` 最小长度检查处返回 `ERANGE`；其它非法范围由候选逻辑返回 `EINVAL`。旧 IPv6 GETROUTE 在无路由时可成功返回 `RTN_UNREACHABLE`，所以范围外检查同时识别该 route type 与 `ENETUNREACH` ACK，不能只检查 errno。旧 xtables revision getsockopt 可以返回非负的最高 revision（例如 1 或 3），所以成功判断为 `>= 0`，不能误写成 `== 0`。

## 父流程接入

父流程现有脚本只读参考路径：

`<repository>/tools/build-kernel-smoke.sh`

它当前在配置末尾写了 `# CONFIG_NETDEVICES is not set`。集成本 smoke 时应在该 override **之后** 合并 `required.config`，或等价修改父流程自己的配置生成逻辑，启用 `CONFIG_NETDEVICES=y`、`CONFIG_DUMMY=y`。还须合入已审阅的 5 个网络补丁以及 `kernel-candidate/android81-netd-sta.fragment`。不需要开启任何物理网络驱动；继续保持 `CONFIG_MODULES=n`。

本子任务没有修改父脚本。构建可在现有 `lima-pw3` 内，从本目录副本运行：

```sh
./build-arm.sh
```

默认只调用 `/home/builder/toolchains/musl-arm/bin/musl-gcc`，产物写本目录 `build/`。可用 `NETWORK_SMOKE_CC` 和 `NETWORK_SMOKE_OUT` 指定父流程已经准备好的只读编译器及隔离输出目录。采用 `-std=gnu99 -static -Os -Wall -Wextra -Werror`。程序不使用 binder/ashmem 头，因此无需父脚本额外的 `-include linux/types.h`。

父 cpio list 增加静态 `network-smoke` 和可选 `network-wire-selftest`。父 init 的 `run()` 目前不给参数；需要对本程序使用等价的：

```c
execl("/network-smoke", "/network-smoke", "--run-isolated", (char *)0);
```

并将退出码计入 suite failures。程序返回 0 才表示本次 kernel-contract 检查全部通过；64 是用法错误，77 是平台/模块 guard 拒绝，124 是子进程 watchdog。不要删除 guard 后在 Kindle 或开发机主系统执行。

主机可复核的检查命令仅涉及 wire codec：

```sh
clang -std=c99 -Wall -Wextra -Werror wire-selftest.c netlink-wire.c -o validation/wire-selftest-host
./validation/wire-selftest-host
bash -n build-arm.sh
```

`validation/host-checks.json` 记录当前修订的检查范围；`validation/legacy-netns-revision.json` 分开保存初版真实失败与当前修订的未验证状态。当前 Linux translation unit 仍需父流程重新用实际 musl-arm 编译器检查并运行。复制构建输入时需要包含新增的 `isolation-guard.h`；编译命令本身不需增加 translation unit。

## 来源与后续必需验证

`source-manifest.json` 锁定所核查本地 Amazon 3.0.35 头/实现、UID/RA 候选和父 smoke 构建入口。测试 UAPI 数字来自这些实际文件及 Android 8.1 UID ABI；没有根据现代 Linux 的结构随意猜测旧内核接口。

后续验收应保存最终 kernel.config、补丁 SHA、ARM ELF 信息、二进制 SHA、完整 QEMU stdout/退出码与 host/QEMU 平台标识。即使全部通过，准确结论也应是“这些 syscall 和隔离内核契约在 VExpress 测试内核上通过”，不能升级为 PW3 硬件、Android netd、Wi-Fi、微信读书或真实联网已通过。
