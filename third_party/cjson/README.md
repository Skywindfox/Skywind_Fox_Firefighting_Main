# cJSON 1.7.18

这是 [cJSON](https://github.com/DaveGamble/cJSON) 的**源码副本**，随仓库一起提交，
不依赖网络，也不要求板上/构建机另装 `libcjson-dev`。

| 项 | 值 |
|---|---|
| 版本 | 1.7.18 |
| 来源 | Debian `cjson` 源码包（`cjson_1.7.18.orig.tar.gz`，与上游 v1.7.18 对应） |
| 许可证 | MIT，见同目录 [LICENSE](LICENSE) |
| 用到的文件 | `cJSON.c` / `cJSON.h` |
| 集成方式 | `CMakeLists.txt` 里的 `cjson` 静态库；`firecontrol` 私有链接它 |

## 为什么不用系统的 libcjson

- 开发机（Fedora）只有运行库、没有头文件；板上 Debian 12 也需要额外装 `libcjson-dev`。
- 交叉编译时还得在 `armhf-toolchain` 镜像里再装 armhf 版，构建环境会多一个版本变量。
- 源码只有两个文件，MIT 许可，直接随仓库走最省事，构建结果可复现。

## 升级方式

替换 `cJSON.c` / `cJSON.h` / `LICENSE` 三个文件，并在本文件里改版本号。
cJSON 的公开 API 在 1.x 内保持稳定；升级后跑一遍 `ctest`（`test_json_status`）即可。
