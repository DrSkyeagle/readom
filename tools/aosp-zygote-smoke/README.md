# Zygote 预加载与等待连接检查

在无网络的512 MB、单核 Cortex-A9 QEMU 中使用实际系统镜像、ramdisk、SELinux策略和 Zygote。保留真实 `init.zygote32.rc` 的配置，只去掉 `--start-system-server`，以单独检查预加载阶段；没有替换 Zygote 实现或跳过字体、图形库及系统调用过滤器的初始化。

测试使用有大小上限的 RAM 数据目录和只读 system。简化的测试 rc 包含产品主 rc 原本已有的 `setrlimit 13 40 40`，避免因遗漏正常启动条件而引入额外权限要求。服务配置文件的内容必须与导出镜像的文件清单匹配。

成功条件是实际 Zygote 日志报告 `Accepting command socket connections`，进程继续运行两秒，SELinux 保持 enforcing、没有检测到崩溃、虚拟机正常关机且原镜像散列不变。结果不覆盖 APK 派生、SystemServer、桌面、PW3 硬件或阅读性能。

```sh
python3 /mnt/port/tools/aosp-zygote-smoke/run.py \
  --system-image /mnt/build-share/aosp-images-fonts/system.raw.img \
  --output /mnt/build-share/aosp-zygote-smoke-new
```

## 只能在 userdebug 镜像上跑（2026-10-22）

这个测试的做法是：init 把测试载荷拷进 `/data`，再以 shell 域起一个服务去跑它。**发行（`user`）镜像不接受这件事**，而且不是"换个目录"能解决的——AOSP 自己的三条 `neverallow` 把每条路都封了：

- `/data/local/tmp`（`shell_data_file`）：shell 读得、也执行得，但 `init.te` 的 `neverallow init shell_data_file:dir { write add_name remove_name }` 不许 init 往里写。开发期间实测到的就是 `init: Unable to open '/data/local/tmp/art-smoke.jar': Permission denied`。
- `/data/nativetest`（`nativetest_data_file`）：init 写得，但 `shell.te` 只在 `userdebug_or_eng` 里给 shell 读执行权，而 `domain.te` 的 `neverallow { domain userdebug_or_eng(-shell) } nativetest_data_file:file no_x_file_perms` 在 `user` 上对**所有域**禁了执行。开发期间实测：`avc: denied { search } ... nativetest_data_file ... permissive=0`。
- 让服务留在 init 域（这样就能读 `/data/nativetest`）：`init.te` 的 `neverallow init { file_type fs_type }:file execute_no_trans` 不许 init 不换域地 exec。
- `androidboot.selinux=permissive` 在 `user` 构建里被 init 忽略（`ALLOW_PERMISSIVE_SELINUX` 只在 userdebug/eng 打开）。

要让它在发行镜像上跑，只能往镜像里加策略规则——那是用系统的代价换一个测试，不做。所以 `tools/run-tests.py` 在导出是 `user` 时把这三项列为"不在这里跑"，并说明原因；发行时另外用同一棵源码树构一次 userdebug、导出、再跑这三项。这不是对发行镜像的直接证据：两种变体的差别是调试工具、`ro.debuggable` 和 `userdebug_or_eng` 那部分策略，而 45 个 ART 启动文件两者逐字节相同（`docs/design/release-build.md`），所以"这棵树的运行时与服务起得来"这个结论是可以搬过去的，"发行镜像的策略放不放行"则由上机验证。
