# 工具

构建、检查、测试用的脚本。**先看这一个**：

```sh
python3 tools/run-tests.py --list     # 这棵树里能跑什么、跑不了什么、为什么跑不了
python3 tools/run-tests.py            # 跑能跑的
```

`run-tests.py` 是唯一那份名单：每一项测试要么登记在它里面，要么登记在
`tools/tests/test_every_test_runs.py` 的 `ELSEWHERE` 里并写明由什么跑它。
加了测试不登记，`every-test-runs` 这一项就红——那是唯一有人在看的时刻。

跑不了的项目**不会显示为通过**，会列成 `SKIP … needs <什么>`。这条分得很细，因为"检查被关掉
却显示通过"是这套工具最贵的失效方式：2026-10-31 一天里抓出六类（计数为 0 当通过、规则集被清空、
输入被截断仍算已检查、显式路径不存在被静默丢掉、`--filter` 选不中任何测试、`SKIP` 的条件拿被检查
对象本身来判）。新加检查时请自问一句：**它被关掉的时候会显示什么？**

## 按用途分

| 做什么 | 脚本 |
|---|---|
| 准备源码树、应用补丁系列 | `prepare-aosp-device.sh`、`verify-tree-matches-series.py` |
| 构建与导出系统镜像 | `run-aosp-build-job.sh`（**一律经它**）、`build-aosp-system.sh`、`export-aosp-images.py`、`inspect-system-image.py` |
| 构建内核 | `build-kernel.sh`（Readom 的那棵）、`build-loader-stage-kernel.sh`（QEMU 夹具的第一级）、`build-kernel-smoke.sh`、`make-test-kernel-config.py` |
| 取第三方预构建件 | `fetch-prebuilts.py`（按 `source-lock.json` 的哈希） |
| 发行 | `make-release-keys.sh`、`collect-copyleft-sources.py` |
| 设备上自检 | `verify-device.sh`（在设备上跑，十二节；要 root 的那几项在 `user` 镜像上会如实列成「要 root」而不是通过，脚本开头打印能不能提权、末尾给计数——数字别抄在这里，每加一项就变） |
| 出事了手动用的两把 | `read-crash-from-data-image.sh`（**adb 够不着的时候**：launcher 每次启动就崩，没人进得去开发者选项，而崩溃在 `/data/system/dropbox` 里——这个脚本把 `data.img` 拷出来在虚拟机里只读挂载，打印 DropBox、tombstone 和 ANR）、`kindle-readonly-probe.sh`（在 Kindle 原厂系统上跑，只读采集硬件信息，写 `/mnt/us/pw3-port-probe/`，移植新机型时用） |
| 发出去之前的闸门 | `check-private-data.py`（设备序列号、Wi-Fi 名称、密钥）、`check-image-references.py`、`tests/test_doc_links.py` |
| 宿主机测试 | `tests/`（28 个 Python 测试）、`kernel-smoke/`、`kernel-hardware/`、`aosp-*-smoke/`、`preinit-pack/`、`host-art/` |
| 画图、看数 | `panel-chart/` |

`git-hooks/pre-commit` 把隐私检查挂在提交之前；装法在 [CONTRIBUTING.md](../CONTRIBUTING.md)。

## 两件容易踩的

- **构建一律经 `run-aosp-build-job.sh`**。直接调 `build-aosp-system.sh` 会让导出用上别次构建的
  回执；在 chroot 里手敲 `make` 会把宿主工具链全部重编，约两小时。
- **别把压缩过的载荷喂给 `check-private-data.py`**。190 MB 的 zstd 块里随机字节会撞上邮箱正则，
  判法和该查哪里写在 [docs/design/release-build.md](../docs/design/release-build.md)。
