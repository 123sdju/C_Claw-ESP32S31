# CClaw ESP32S31

这是 CClaw 的 ESP32S31 产品仓库，负责板级硬件、ESP-IDF 组件、产品服务、
传输层、产品工具、OTA、摄像头、LCD、USB、Lua 和设备配置。

通用 Agent Runtime 不在本仓库复制维护，而是通过
`third_party/cclaw-sdk` 固定依赖 `C_Claw-SDK`。产品仓库可以使用 SDK 的公共
`CClaw::runtime` target 和 `cc/` 公共头文件，但不应引用 SDK 的 `src/` 内部实现。

## 仓库结构

- `main/`：ESP32S31 应用、板级适配、服务、传输和产品工具。
- `cmake/esp32s31-profile.cmake`：产品拥有的 SDK feature profile。
- `third_party/cclaw-sdk/`：固定到 SDK tag/commit 的 Git submodule。
- `dependencies.lock`：ESP-IDF 组件依赖锁定。
- `sdkconfig.defaults`、`partitions.csv`：产品构建和分区配置。

## 初始化 SDK

```bash
git submodule update --init --recursive
idf.py set-target esp32s31
idf.py build
```

不要依赖 SDK 的浮动 `main` 分支；SDK 更新应先发布版本，再更新本仓库的
submodule 指针。

当前目录是从现有 CClaw 工程整理出的产品初始快照；本目录的 ESP-IDF 构建和硬件运行尚未执行验证。

## License

MIT License，详见根目录 `LICENSE`。ESP-IDF 和其他第三方组件仍以其各自的许可证为准。
