<!-- 中文、英文都可以 / Either language is fine -->

## 改了什么 / What changed



## 为什么 / Why

<!-- 依据是什么：实测、日志、规范、上游的做法。推断和实测分开写。 -->
<!-- The evidence: measurements, logs, specifications, what upstream does. Keep inference and measurement apart. -->



## 验证到哪一步 / How far it was verified

- [ ] `python3 tools/run-tests.py` 在本机通过 / passes on my machine
- [ ] 改到只有构建虚拟机里才跑得了的部分时，在那里跑过全套 / where it touches what only runs in the build VM, the full suite ran there
- [ ] 在设备上验过（写下机型、版本号、看到了什么）/ checked on a device (model, build number, what you saw)

## 检查 / Checklist

- [ ] 改了一种语言的文档，另一种也改了 / a document changed in one language is changed in the other
- [ ] 补丁系列只追加，已有的补丁没有改 / the patch series was only appended to
- [ ] framework 资源只改值、不加新名字 / framework resources: values changed, no new names
- [ ] 没有设备专属数据：序列号、MAC、Wi‑Fi 名称、波形、校准 / no device-specific data: serial numbers, MACs, Wi-Fi names, waveforms, calibration
