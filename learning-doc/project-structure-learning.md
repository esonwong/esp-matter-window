# ESP-IDF 项目结构与 Components 学习笔记

## 1. 什么是 Component? (What is a Component?)

在 ESP-IDF 中，**Component (组件)** 是代码复用和模块化的基本单元。可以说，整个 ESP-IDF 本身就是由各种 Components 组成的（如 `driver`, `freertos`, `log` 等）。

你的项目可以看作是：
*   **Main Component**: 包含 `app_main` 的业务逻辑代码。
*   **System Components**: ESP-IDF 提供的基础库。
*   **Custom Components**: 你自己编写的或者第三方的功能模块。

## 2. `components` 目录的作用

项目根目录下的 `components/` 文件夹由于**特殊性**：ESP-IDF 构建系统会自动搜索这个目录，并将其中的每个子目录识别为一个独立的 Component。

### 为什么要把代码放入 `components`?
1.  **模块化 (Modularity)**: 将功能独立的代码（如屏幕驱动、传感器驱动、算法库）与业务逻辑 (`main`) 解耦。
2.  **可复用性 (Reusability)**: 写好的 Component 可以直接复制到另一个项目中由 `components` 目录下使用。
3.  **封装 (Encapsulation)**: Component 可以通过 `CMakeLists.txt` 控制哪些头文件对外公开 (`REQUIRES`), 哪些仅内部可见 (`PRIV_REQUIRES`)。

## 3. 标准的 Component 结构

一个典型的 Component 目录结构如下：

```text
my_component/
├── CMakeLists.txt      # 构建脚本 (必须)
├── idf_component.yml   # 依赖管理器配置 (可选)
├── include/            # 对外头文件目录
│   └── my_component.h  # 公共 API
├── src/                # 私有源文件目录 (可选，也可以直接放在根目录)
│   ├── internal_logic.c
│   └── my_component.c
└── Kconfig             # Menuconfig 配置选项 (可选)
```

**CMakeLists.txt 示例**:
```cmake
idf_component_register(
    SRCS "my_component.c" "src/internal_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_log  # 公共依赖 (使用了这里的头文件的其他人也需要这些依赖)
    PRIV_REQUIRES freertos   # 私有依赖 (只在 .c 实现文件中用到)
)
```

## 4. 什么时候在 `main` 写，什么时候用 `components`?

| 场景 | 推荐位置 | 理由 |
| :--- | :--- | :--- |
| **特定的业务流程** | `main/` | 比如 `app_main`, 任务调度，各个模块的粘合代码。 |
| **只能在此项目中用的模块** | `main/` | 比如 `app_ui.c` (特定的UI布局), `led_ctrl.c` (特定的指示灯逻辑)。 |
| **通用外设驱动 (e.g. 屏幕、传感器)** | `components/` | 以后换个项目还可能用同一块外设，独立出来最好。 |
| **通用算法** | `components/` | PID 控制算法、滤波算法等。 |
| **第三方库** | `components/` | 如 `Adafruit-GFX`, `LVGL` 等。 |

## 5. 结合本项目：`main/CMakeLists.txt` 现状与可改进点

本项目所有源码都放在 `main/` 下（`main_app.cpp`、`window_ctrl.cpp`、`motor_ctrl.cpp`、`led_ctrl.cpp`、`matter_app.cpp`、`diag_log.cpp`），这符合上表的建议——它们都是与本窗控业务强绑定的模块，暂时没有拆 `components/` 的必要。

目前 `main/CMakeLists.txt` 用的是通配符收集源文件：

```cmake
file(GLOB SOURCES "*.c" "*.cpp")

idf_component_register(
    SRCS ${SOURCES}
    ...
)
```

**`file(GLOB)` 的问题**：CMake 只在“重新运行 configure”时才展开 GLOB。新增/删除 `.cpp` 文件后，如果只是增量 `idf.py build`，CMake 可能不会察觉文件列表变了，导致新文件没被编译（或删掉的文件仍被引用报错）。这也是 CMake 官方不推荐 GLOB 收集源文件的原因。

**更稳妥的写法**是显式列出源文件：

```cmake
idf_component_register(
    SRCS "main_app.cpp" "window_ctrl.cpp" "motor_ctrl.cpp"
         "led_ctrl.cpp" "matter_app.cpp" "diag_log.cpp"
    INCLUDE_DIRS "." "include"
    ...
)
```

代价是每加一个文件要手动登记一行，但换来的是构建行为完全确定。小项目里 GLOB 也能用（大不了 `idf.py fullclean`），但知道这个坑在哪很重要。

**将来什么时候拆 `components/`？** 如果某个模块（比如 `motor_ctrl` 的 H 桥驱动逻辑）要被第二个项目复用，再按第 3 节的方式把它提成独立 component 即可：建 `components/motor_ctrl/`、写自己的 `CMakeLists.txt`、头文件放 `include/`，然后在 `main` 的 `PRIV_REQUIRES` 里声明依赖。
