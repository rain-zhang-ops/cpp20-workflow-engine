# cpp20-workflow-engine

高性能 C++20 DAG 工作流引擎，面向 AI 推理编排与插件化任务调度，针对 Linux 服务器环境设计。

> 极低资源占用 · 零拷贝热重载 · Unix Domain Socket CLI 控制 · 插件 .so 热插拔

---

## 目录

- [系统架构](#系统架构)
- [核心特性](#核心特性)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [配置说明](#配置说明)
- [CLI 控制台使用](#cli-控制台使用)
- [插件开发指南](#插件开发指南)
- [集成指南](#集成指南)
- [技术栈](#技术栈)
- [许可证](#许可证)

---

## 系统架构

```
                         ┌──────────────────────┐
                         │     workflow-cli      │
                         │  (UDS 命令行客户端)    │
                         └──────────┬───────────┘
                                    │ Unix Domain Socket
                         ┌──────────▼───────────┐
                         │    ControlPlane       │
                         │  RELOAD / STOP /      │
                         │  STATUS 命令派发       │
                         └──────────┬───────────┘
                                    │
┌───────────────┐       ┌───────────▼───────────┐       ┌───────────────┐
│ ConfigWatcher │──────▶│    WorkflowEngine     │◀──────│ PluginManager │
│ inotify 热监听│       │  DAG 调度 + 并发执行   │       │ dlopen 热加载  │
└───────────────┘       └───────────┬───────────┘       └───────────────┘
                                    │
                         ┌──────────▼───────────┐
                         │     ThreadPool        │
                         │  counting_semaphore   │
                         │  N 个 Worker 线程      │
                         └──────────┬───────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
              [Node A]        [Node B]        [Node C]
              (插件 .so)      (插件 .so)      (插件 .so)
```

### 数据流

1. `main.cpp` 启动 → ThreadPool → PluginManager → WorkflowEngine → ConfigWatcher → ControlPlane
2. 引擎进入 **Daemon 模式**（`std::promise<void>().get_future().wait()`），主线程永久挂起
3. 外部通过 `workflow-cli` 发送命令（RELOAD / STATUS / STOP）驱动引擎
4. DAG 节点按依赖关系自动并行调度，原子计数器递减驱动下游节点

---

## 核心特性

| 模块 | 说明 | 关键技术 |
|------|------|---------|
| **ConfigWatcher** | 零 CPU 文件变更监听 | `inotify` + `eventfd` + `jthread` + `poll` |
| **ThreadPool** | 高效工作线程池 | `counting_semaphore` + `std::function` 任务队列 |
| **PluginManager** | 无锁插件热更新 | `atomic<shared_ptr>` + `dlopen/dlsym/dlclose` |
| **WorkflowEngine** | DAG 并发调度器 | 原子依赖计数 + `std::latch` 完成同步 + 级联取消 |
| **ControlPlane** | UDS 命令控制面 | `poll` 事件循环 + `SOCK_NONBLOCK` + RAII fd 管理 |
| **ExecutionContext** | 跨插件共享上下文 | `extern "C"` ABI 安全接口 + `std::any` 类型擦除 |
| **NodeExecutionWrapper** | 重试 + 错误隔离 | 可配置 `max_retries` + `NodeResult` variant |
| **AsyncLogger** | 异步无阻塞日志 | `LockFreeRingBuffer` + 后台 flush 线程 |

---

## 目录结构

```
cpp20-workflow-engine/
├── CMakeLists.txt              # 顶层构建配置
├── config/
│   └── workflow.json           # DAG 配置文件（节点、依赖、插件路径）
├── core/                       # 核心静态库 (workflow-core)
│   ├── ControlPlane.cpp/h      #   UDS 控制面
│   ├── GraphBuilder.cpp/h      #   JSON → Taskflow DAG 构建器
│   ├── PluginRegistry.cpp/h    #   节点类型注册表
│   └── CMakeLists.txt
├── include/                    # 公共头文件
│   ├── WorkflowNode.h          #   插件基类接口
│   ├── WorkflowEngine.h        #   引擎头文件
│   ├── ExecutionContext.h       #   共享上下文 + extern "C" API
│   ├── ThreadPool.h            #   线程池
│   ├── PluginManager.h         #   插���管理器
│   ├── ConfigWatcher.h         #   配置监听器
│   ├── NodeState.h             #   节点状态枚举
│   ├── NodeExecutionWrapper.h  #   执行包装器（重试逻辑）
│   ├── EngineMetrics.h         #   引擎指标（原子计数器）
│   ├── AsyncLogger.h           #   异步日志
│   ├── LockFreeRingBuffer.h    #   无锁环形缓冲区
│   └── ScopedTimer.h           #   RAII 计时器
├── plugins/                    # 插件源码（编译为 .so）
│   └── example_plugin.cpp      #   示例插件
├── src/                        # 主程序源码
│   ├── main.cpp                #   入口 + daemon 模式
│   ├── WorkflowEngine.cpp      #   DAG 调度实现
│   ├── ExecutionContext.cpp     #   上下文 + extern "C" 实现
│   ├── ConfigWatcher.cpp       #   inotify 监听实现
│   ├── PluginManager.cpp       #   dlopen 加载实现
│   └── ThreadPool.cpp          #   线程池实现
├── tools/                      # CLI 工具
│   ├── workflow-cli.cpp        #   命令行客户端
│   └── CMakeLists.txt
└── examples/                   # 示例程序
    ├── json_taskflow_demo.cpp
    └── CMakeLists.txt
```

---

## 快速开始

### 环境要求

- **操作系统**: Linux (Ubuntu 20.04+, CentOS 8+, WSL2)
- **编译器**: GCC 11+ 或 Clang 14+（需支持 C++20）
- **构建工具**: CMake 3.20+

### 编译

```bash
git clone https://github.com/rain-zhang-ops/cpp20-workflow-engine.git
cd cpp20-workflow-engine
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

编译产物（全部在 `build/` 目录下）：

| 文件 | 说明 |
|------|------|
| `workflow-engine` | 主引擎守护进程 |
| `workflow-cli` | CLI 控制客户端 |
| `libexample_plugin.so` | 示例插件 |
| `json-taskflow-demo` | Taskflow 演示程序 |

### 启动引擎

```bash
cd build

# 使用默认配置启动（daemon 模式，不会退出）
./workflow-engine

# 或指定配置文件路径
./workflow-engine /path/to/your/workflow.json
```

启动后输出：

```
=== cpp20-workflow-engine ===
Config: ../config/workflow.json

[main] ThreadPool created with 12 worker(s)
[WorkflowEngine] Loaded config: ../config/workflow.json  plugin=./libexample_plugin.so  nodes=4
[main] ConfigWatcher started for: ../config/workflow.json

[main] ControlPlane started (socket: /tmp/workflow.sock)

[main] Engine is running in daemon mode. Waiting for CLI commands via socket...
[ControlPlane] Listening on /tmp/workflow.sock
```

引擎以 **daemon 模式** 常驻运行，通过 CLI 或修改配置文件控制。

---

## 配置说明

配置文件为 JSON 格式，路径默认 `config/workflow.json`：

```json
{
  "plugin": "./libexample_plugin.so",
  "nodes": [
    {
      "id": "A",
      "dependencies": [],
      "max_retries": 2
    },
    {
      "id": "B",
      "dependencies": ["A"],
      "max_retries": 1
    },
    {
      "id": "C",
      "dependencies": ["A"],
      "max_retries": 0
    },
    {
      "id": "D",
      "dependencies": ["B", "C"],
      "max_retries": 0
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `plugin` | string | 插件 `.so` 文件路径（相对于 build 目录） |
| `nodes` | array | DAG 节点列表 |
| `nodes[].id` | string | 节点唯一标识 |
| `nodes[].dependencies` | string[] | 依赖的上游节点 ID 列表（空数组 = 根节点） |
| `nodes[].max_retries` | number | 失败重试次数（0 = 不重试） |

### DAG 依赖图示例

上述配置生成的 DAG：

```
    A
   / \
  B   C
   \ /
    D
```

- A 为根节点，无依赖，最先执行
- B 和 C 依赖 A，A 完成后并行执行
- D 依赖 B 和 C，两者都完成后执行

### 热重载

修改 `workflow.json` 并保存，ConfigWatcher 会自动检测变更并触发热重载。也可通过 CLI 手动触发：

```bash
./workflow-cli reload
```

---

## CLI 控制台使用

`workflow-cli` 通过 Unix Domain Socket (`/tmp/workflow.sock`) 与引擎通信。

### 基本命令

```bash
# 查看引擎运行状态
./workflow-cli status

# 输出示例：
# STATUS: workflow-engine running
#   success_count    : 4
#   failed_count     : 0
#   cancelled_count  : 0
#   hot_reload_count : 0

# 触发配置热重载
./workflow-cli reload

# 输出示例：
# OK: reloaded config from ../config/workflow.json

# 优雅停止引擎
./workflow-cli stop

# 输出示例：
# OK: stop signal sent — engine will exit shortly
```

### CLI 工作原理

1. `workflow-cli` 连接到 `/tmp/workflow.sock`
2. 发送命令字符串（如 `STATUS`）
3. ControlPlane 在引擎进程内接收、分发、执行
4. 结果通过 socket 回传给 CLI 并打印

---

## 插件开发指南

### 插件接口

所有插件必须实现 `WorkflowNode` 抽象基类，并导出 C 工厂函数：

```cpp
// 你的插件 .cpp 文件
#include "ExecutionContext.h"
#include "WorkflowNode.h"

class MyNode : public WorkflowNode {
public:
    void execute(ExecutionContext& ctx) override {
        // ⚠️ 跨 .so 边界必须使用 extern "C" 辅助函数
        void* ctx_ptr = static_cast<void*>(&ctx);

        // 写入上下文
        ctx_set_string(ctx_ptr, "result", "hello from my plugin");
        ctx_set_int64(ctx_ptr, "count", 42);
        ctx_set_double(ctx_ptr, "score", 0.95);

        // 读取上下文
        char buf[256] = {};
        ctx_get_string(ctx_ptr, "result", buf, sizeof(buf));

        int64_t count = 0;
        ctx_get_int64(ctx_ptr, "count", &count);
    }
};

// 必须导出的 C 工厂函数
extern "C" WorkflowNode* create_node() {
    return new MyNode();
}

extern "C" void destroy_node(WorkflowNode* node) {
    delete node;
}
```

### ABI 安全规则

> **重要**: 插件 `.so` 与主程序是独立编译单元。直接调用 ExecutionContext 的 C++ 成员函数（`set/get/has/remove`）是**不安全**的，因为 `std::any` 的 typeinfo 和分配器地址可能不同。
>
> **必须**使用 `extern "C"` 辅助函数（`ctx_set_string`、`ctx_get_int64` 等）。这些函数在主程序��实现，通过 `-rdynamic` 链接选项导出。

### 可用的 extern "C" 上下文 API

```c
// 写入
void ctx_set_string(void* ctx, const char* key, const char* value);
void ctx_set_int64 (void* ctx, const char* key, int64_t value);
void ctx_set_double(void* ctx, const char* key, double value);

// 读取（成功返回 0，失败返回 -1）
int ctx_get_string(void* ctx, const char* key, char* buf, size_t buf_size);
int ctx_get_int64 (void* ctx, const char* key, int64_t* out);
int ctx_get_double(void* ctx, const char* key, double* out);

// 检查 key 是否存在（存在返回 1，不存在返回 0）
int ctx_has_key(void* ctx, const char* key);

// 删除 key
void ctx_remove_key(void* ctx, const char* key);
```

### 编译插件

在 `CMakeLists.txt` 中添加：

```cmake
# 新插件
add_library(my_plugin SHARED plugins/my_plugin.cpp)
target_include_directories(my_plugin PRIVATE include)
set_target_properties(my_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
```

然后在 `workflow.json` 中指定：

```json
{
  "plugin": "./libmy_plugin.so",
  "nodes": [...]
}
```

---

## 集成指南

### 作为独立守护进程运行

最常见的使用方式——作为后台服务运行，通过 CLI 或 Socket 交互：

```bash
# 后台启动
nohup ./workflow-engine ../config/workflow.json > engine.log 2>&1 &

# 通过 CLI 控制
./workflow-cli status
./workflow-cli reload
./workflow-cli stop
```

### 编写 systemd 服务

```ini
# /etc/systemd/system/workflow-engine.service
[Unit]
Description=C++20 Workflow Engine
After=network.target

[Service]
Type=simple
ExecStart=/opt/workflow-engine/workflow-engine /opt/workflow-engine/config/workflow.json
WorkingDirectory=/opt/workflow-engine
Restart=always
RestartSec=5
User=engine
Group=engine

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now workflow-engine
sudo systemctl status workflow-engine
```

### 通过 Socket 编程集成

任何语言都可以通过 Unix Domain Socket 与引擎通信：

**Python 示例：**

```python
import socket

def send_command(cmd: str, socket_path: str = "/tmp/workflow.sock") -> str:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)
    sock.sendall(cmd.encode())
    response = sock.recv(4096).decode()
    sock.close()
    return response

# 使用
print(send_command("STATUS"))
print(send_command("RELOAD"))
```

**Bash 示例：**

```bash
echo "STATUS" | socat - UNIX-CONNECT:/tmp/workflow.sock
```

**Node.js 示例：**

```javascript
const net = require('net');

const client = net.createConnection('/tmp/workflow.sock', () => {
    client.write('STATUS');
});
client.on('data', (data) => {
    console.log(data.toString());
    client.end();
});
```

### 嵌入到现有 C++ 项目

如果需要将引擎嵌入到现有项目中，直接链接 `workflow-core` 静态库：

```cmake
# 你的项目 CMakeLists.txt
add_subdirectory(path/to/cpp20-workflow-engine)

target_link_libraries(your_app
    PRIVATE
        workflow-core
        dl
        pthread
)
```

```cpp
#include "ThreadPool.h"
#include "PluginManager.h"
#include "WorkflowEngine.h"

// 创建引擎
ThreadPool pool;
PluginManager plugin_mgr;
WorkflowEngine engine(pool, plugin_mgr);

// 加载配置并执行
engine.loadConfig("workflow.json");
engine.run();
engine.waitForCompletion();

// 查看指标
engine.getMetrics().print();
```

---

## 技术栈

| 类别 | 技术 |
|------|------|
| **语言标准** | C++20 (`-std=c++20`) |
| **构建** | CMake 3.20+ |
| **并发** | `std::jthread`, `std::stop_token`, `std::latch`, `counting_semaphore`, `std::atomic` |
| **系统 API** | `inotify`, `eventfd`, `poll`, `dlopen/dlsym`, Unix Domain Socket |
| **第三方** | [nlohmann/json](https://github.com/nlohmann/json) v3.11.3, [Taskflow](https://github.com/taskflow/taskflow) v3.7.0 (FetchContent 自动拉取) |
| **零运行时依赖** | 无需安装任何第三方库，构建时自动下载 |

---

## 许可证

MIT License