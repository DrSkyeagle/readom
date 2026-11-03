# Android 密钥服务运行检查

在隔离的单核、512 MB Cortex-A9 QEMU 中启动实际构建的 Binder/HwBinder 服务管理器、软件 Keymaster、Keystore、Gatekeeper 和日志服务。系统镜像只读，数据仅在有大小上限的 RAM 文件系统中，SELinux 保持 enforcing。

Java 测试客户端以 shell 身份运行。它调用实际 `ActivityThread.systemMain()` 初始化应用上下文，再注册 AndroidKeyStore provider；它没有创建替代密钥服务或伪造加密结果。七项检查覆盖新目录中不存在测试别名、生成密钥、API 不导出原始密钥、通过别名取回、AES-GCM 加解密、拒绝被修改的密文和删除密钥。测试别名和输入内容固定，不访问用户账户或网络。

这只验证上述软件密钥服务链路。它不证明硬件密钥隔离、持久存储保护、锁屏身份认证、普通 APK 的完整生命周期或微信读书登录成功。Gatekeeper 仅作为真实服务启动，本测试不调用其认证 API。

```sh
python3 /mnt/port/tools/aosp-service-smoke/run.py \
  --system-image /mnt/build-share/aosp-images-bionic-string-fix/system.raw.img \
  --output /mnt/build-share/aosp-service-smoke-new
```

每次输出必须使用新目录。运行器校验导出镜像及相应 ramdisk 的散列，保存输入与控制台日志。成功要求七项检查通过、正常关机、无崩溃、SELinux 强制模式保持开启及系统镜像未改变；没有通过标记的退出不算成功。

## 只能在 userdebug 镜像上跑（2026-10-22）

这个测试的做法是：init 把测试载荷拷进 `/data`，再以 shell 域起一个服务去跑它。**发行（`user`）镜像不接受这件事**，而且不是"换个目录"能解决的——AOSP 自己的三条 `neverallow` 把每条路都封了：

- `/data/local/tmp`（`shell_data_file`）：shell 读得、也执行得，但 `init.te` 的 `neverallow init shell_data_file:dir { write add_name remove_name }` 不许 init 往里写。开发期间实测到的就是 `init: Unable to open '/data/local/tmp/art-smoke.jar': Permission denied`。
- `/data/nativetest`（`nativetest_data_file`）：init 写得，但 `shell.te` 只在 `userdebug_or_eng` 里给 shell 读执行权，而 `domain.te` 的 `neverallow { domain userdebug_or_eng(-shell) } nativetest_data_file:file no_x_file_perms` 在 `user` 上对**所有域**禁了执行。开发期间实测：`avc: denied { search } ... nativetest_data_file ... permissive=0`。
- 让服务留在 init 域（这样就能读 `/data/nativetest`）：`init.te` 的 `neverallow init { file_type fs_type }:file execute_no_trans` 不许 init 不换域地 exec。
- `androidboot.selinux=permissive` 在 `user` 构建里被 init 忽略（`ALLOW_PERMISSIVE_SELINUX` 只在 userdebug/eng 打开）。

要让它在发行镜像上跑，只能往镜像里加策略规则——那是用系统的代价换一个测试，不做。所以 `tools/run-tests.py` 在导出是 `user` 时把这三项列为"不在这里跑"，并说明原因；发行时另外用同一棵源码树构一次 userdebug、导出、再跑这三项。这不是对发行镜像的直接证据：两种变体的差别是调试工具、`ro.debuggable` 和 `userdebug_or_eng` 那部分策略，而 45 个 ART 启动文件两者逐字节相同（`docs/design/release-build.md`），所以"这棵树的运行时与服务起得来"这个结论是可以搬过去的，"发行镜像的策略放不放行"则由上机验证。
