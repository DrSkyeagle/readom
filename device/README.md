# 设备树

AOSP 的约定：`device/<厂商>/<板子>/`。这里只有一块板子——`amazon/pw3/`，Kindle Paperwhite 3。
它和 `../patches/` 是分工的两头：**凡是能写成"这台机器自己的东西"的，都放这里；非改 AOSP
本体不可的，才去那边写成补丁**。移植到别的机型，照着再写一份兄弟目录，补丁系列大多能沿用
（[../docs/design/porting-to-other-models.md](../docs/design/porting-to-other-models.md)）。

## 从哪儿读起

| 文件 | 是什么 |
|---|---|
| `amazon/pw3/vendorsetup.sh` | `lunch` 里那两个组合：`pw3-user`（发行）和 `pw3-userdebug` |
| `amazon/pw3/AndroidProducts.mk` | 只有一行，指向 `pw3.mk` |
| `amazon/pw3/pw3.mk` | **产品定义**：从 AOSP 最小的 `core_base.mk` 起步，往上加这个移植要的包、属性和覆盖层 |
| `amazon/pw3/BoardConfig.mk` | 板子：armv7-a-neon／Cortex-A9、分区、SELinux、内核 |
| `amazon/pw3/system.prop` | 进 `build.prop` 的属性，每一条上面写着为什么 |
| `amazon/pw3/manifest.xml` | 这台机器声明提供的 HIDL 接口 |

其余子目录按它们负责的那一块分：显示（`epdc`、`gralloc`、`composer`）、电源与休眠
（`power`、`sleep`、`health`、`fuelgauge`、`lights`）、启动与自检（`preinit`、`initcheck`、
`report`、`rootdir`、`verify`）、界面与输入（`overlay`、`apps`、`launcher-icons`、`fonts`、
`emoji`、`input`）、安全策略（`sepolicy`、`sepolicy-private`、`seccomp`）。每一块的设计记录在
[../docs/design/](../docs/design/)，这里不重复。

**两种 `tests` 别混**：各模块自己的 `tests/`（`epdc/tests`、`composer/tests`、`health/tests`
这些）是**宿主机**上跑的，登记在 `tools/run-tests.py` 里，每次 `run-tests.py` 都跑；而
`amazon/pw3/tests/` 下那几个（`paneltest`、`displaybench`、`glesbench`、`formatapp`、
`reactor`）是**要装到机器上才跑得起来的设备模块**，手动 `mma` 编、手动跑，**不在任何产品的
`PRODUCT_PACKAGES` 里，所以不进发行镜像**。
`paneltest` 这个名字害过一次：刷新策略改成每 20 页全刷之后，`epdc/tests/panel_test.cpp` 里
还断言着 30，而那一项当时只由设备模块带着跑、平时根本没人跑，套件一路是绿的
（`tools/tests/test_every_test_runs.py` 开头记着这件事）。

## 两件容易踩的

- **`overlay/` 下的 framework 资源只改值、不加名。** 加一个新名字会动 `boot-*.art`／`.oat`／
  `.vdex`，使用者 `/data` 里所有应用的机器码随之作废，升级后第一次开机要整轮重编译。
  覆盖层条目正上方的注释照抄 AOSP 原文——`aapt2` 会把它抄进 `R.java`。两条规矩的完整说法和
  构建后怎么逐字节核对，在 [../CONTRIBUTING.md](../CONTRIBUTING.md)。
- **这里不放任何设备专属数据。** 屏幕波形、Wi-Fi 校准、MAC 都是每台机器自己的，一律每次启动
  现场从原厂系统读（`../boot/devicedata/`）。这是整个项目能开源的前提，发行前由
  `tools/check-private-data.py` 把关。
