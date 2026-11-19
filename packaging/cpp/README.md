# KopiDB C++ SDK

This package contains a prebuilt database library for **macOS arm64**. You compile your own C++ application; KopiDB itself does not need to be rebuilt.

本包提供 **macOS arm64** 的预编译数据库。使用者只编译自己的 C++ 程序，不需要重新编译 KopiDB。

## 📦 Requirements and contents / 使用要求与文件

Use a C++20-capable compiler and either Xmake or CMake. Check [BUILD-INFO.txt](BUILD-INFO.txt) for this artifact's build environment and the library's minimum macOS version.

需要支持 C++20 的编译器，以及 Xmake 或 CMake，任选一种。具体构建环境和最低系统版本见 [BUILD-INFO.txt](BUILD-INFO.txt)。

```text
kopidb/
├── include/kopidb/       # Headers / 头文件
├── lib/libkopidb.so      # Prebuilt library / 已编译的库
├── xmake.lua            # Xmake integration / 接入配置
├── KopiDBConfig.cmake    # CMake integration / 接入配置
├── BUILD-INFO.txt
├── LICENSE
└── licenses/            # Dependency licenses / 依赖许可
```

## 💻 Your application / 你的程序

Rename the extracted folder to `kopidb` and place it at `third_party/kopidb` inside your project. Save the following as your own `main.cpp`:

将解压目录命名为 `kopidb`，放到自己项目的 `third_party/kopidb`。以下保存为自己的 `main.cpp`：

```cpp
#include <lsm/engine.h>
#include <iostream>

int main() {
  kopidb::LSM db("my-data");
  db.put("name", "Alice");
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';
  }
}
```

## 🛠️ Xmake

Add this to **your project's** `xmake.lua`. Do not edit the SDK's copy.

在**自己项目的** `xmake.lua` 中配置；无需修改 SDK 内的文件。

```lua
includes("third_party/kopidb")

target("my_app")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kopidb")
```

```bash
xmake
xmake run my_app
```

This integration sets dependent targets to C++20. / 此接入配置将依赖 KopiDB 的目标设置为 C++20。

## 🔧 CMake

Alternatively, use this **project-level** `CMakeLists.txt`:

也可以在**自己的项目**中使用以下 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

find_package(KopiDB CONFIG REQUIRED
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kopidb"
    NO_DEFAULT_PATH)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE KopiDB::KopiDB)
```

```bash
cmake -S . -B build
cmake --build build
./build/my_app
```

## ⚙️ Runtime and configuration / 运行与配置

- Keep the SDK's shared library available at runtime. Reconfigure and rebuild your application after moving the SDK.
- The example stores data in `my-data/` under its working directory. No configuration file is required; default-configuration messages on the first run are expected. WiscKey is disabled by default.
- For custom settings, put [config.toml](https://github.com/QinWenYan1/KopiDB/blob/main/config.toml) in your application's working directory.

- 运行时仍需要 SDK 中的共享库。移动 SDK 后，请重新配置并构建程序。
- 示例数据位于运行目录的 `my-data/`。默认使用不需要配置文件，首次运行可能打印默认配置提示；WiscKey 默认关闭。
- 自定义参数时，将 [config.toml](https://github.com/QinWenYan1/KopiDB/blob/main/config.toml) 放到程序的工作目录。

For transactions, iteration and usage constraints, see the [English manual](https://github.com/QinWenYan1/KopiDB/blob/main/README.md) or [中文手册](https://github.com/QinWenYan1/KopiDB/blob/main/README.zh-CN.md). `SERIALIZABLE` remains experimental and does not provide complete serializable isolation.

事务、遍历及使用约定见上述手册。`SERIALIZABLE` 尚未提供完整可串行化保证。
