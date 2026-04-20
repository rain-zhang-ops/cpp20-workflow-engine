# cpp20-workflow-engine

高性能 C++20 DAG 工作流引擎，面向 AI 推理编排与插件化任务调度，针对 Linux 服务器环境设计。

> 极低资源占用 · 零拷贝热重载 · Unix Domain Socket CLI 控制 · 插件 .so 热插拔

---

## 目录

- [系统说明](#系统说明)
- [系统架构](#系统架构)
- [核心特性](#核心特性)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [配置说明](#配置说明)
- [CLI 控制台](#cli-控制台)
- [插件开发指南](#插件开发指南)
- [集成指南](#集成指南)
- [技术栈](#技术栈)
- [许可证](#许可证)

---

## 系统说明

### 这是什么？

cpp20-workflow-engine 是一个用纯 C++20 编写的 **DAG（有向无环图）工作流调度引擎**。它将一组有依赖关系的任务节点（插件 `.so`）按拓扑顺序自动并发执行，并提供运行时热重载、CLI 远程控制和全链路指标监控。

### 解决什么问题？

在 AI 推理、数据处理、DevOps 编排等场景中，往往需要：

1. **多步骤任务编排** — 模型预处理 → 推理 → 后处理 → 写入，步骤间有依赖
2. **高性能并发** — 无依赖的步骤必须并行，CPU 利用率要高
3. **运行时灵活性** — 不停机修改工作流、替换算法插件
4. **极低资源占用** — 无 JVM/Python 运行时开销，适合边缘设备和容器环境

本引擎用 **原子操作 + 无锁数据结构** 实现 DAG 调度，用 **inotify + dlopen** 实现零停机热更新，用 **Unix Domain Socket** 实现进程外控制，整体内存占用通常 < 10 MB。

### 典型使用场景

| 场景 | 如何使用 |
|------|---------|
| **AI 推理流水线** | 每个节点是一个推理步骤（预处理/模型调用/后处理），插件 .so 封装具体逻辑 |
| **数据 ETL** | 节点按 DAG 依赖并发执行 Extract → Transform → Load |
| **CI/CD 任务编排** | 定义构建/测试/部署步骤的依赖关系，引擎自动调度 |
| **IoT 边缘计算** | 极低资源占用，在嵌入式 Linux 设备上运行复杂工作流 |

### 工作流程概览

```
1. 编写 workflow.json 定义节点和依赖关系
2. 编写插件 .so（继承 WorkflowNode，实现 execute()）
3. 启动引擎：./workflow-engine workflow.json
4. 引擎自动：解析 JSON → 构建 DAG → 加载插件 → 并发调度执行
5. 运行时：CLI 查状态 / 热重载配置 / 替换插件 / 优雅停机
```

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
2. 引擎以 **daemon 模式** 常驻运行
3. 外部通过 `workflow-cli` 发送命令（RELOAD / STATUS / STOP）驱动引擎
4. DAG 节点按依赖关系自动并行调度，原子计数器递减驱动下游节点
5. 节点间通��� `ExecutionContext`（线程安全 KV 存储）共享数据

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
│   ├── PluginManager.h         #   插件管理器
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

# 使用默认配置启动（daemon 模式）
./workflow-engine

# 或指定配置文件路径
./workflow-engine /path/to/your/workflow.json
```

启动后输出：

```
=== cpp20-workflow-engine ===
Config: ../config/workflow.json

[main] ThreadPool created with 12 worker(s)
[WorkflowEngine] Loaded config  plugin=./libexample_plugin.so  nodes=4
[main] ConfigWatcher started for: ../config/workflow.json
[main] ControlPlane started (socket: /tmp/workflow.sock)
```

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
| `nodes[].id` | string | 节点��一标识 |
| `nodes[].dependencies` | string[] | 依赖的上游节点 ID 列表（空数组 = 根节点） |
| `nodes[].max_retries` | number | 失败重试次数（0 = 不重试） |

### DAG 依赖图示例

```
    A           ← 根节点，无依赖，最先执行
   / \
  B   C         ← 依赖 A，A 完成后并行执行
   \ /\n    D           ← 依赖 B 和 C，两者都完成后执行
```

### 热重载

```bash
# 方式一：编辑配置文件，保存即触发（inotify 自动检测）
vim config/workflow.json

# 方式二：CLI 手动触发
./workflow-cli reload
```

---

## CLI 控制台

`workflow-cli` 通过 Unix Domain Socket (`/tmp/workflow.sock`) 与引擎通信。

### 命令

```bash
# 查看引擎运行状态
./workflow-cli status
# STATUS: workflow-engine running
#   success_count    : 8
#   failed_count     : 0
#   cancelled_count  : 0
#   hot_reload_count : 1

# 触发配置热重载
./workflow-cli reload
# OK: reloaded config from ../config/workflow.json

# 优雅停止引擎
./workflow-cli stop
# OK: stop signal sent — engine will exit shortly
```

也可以不用 CLI 工具，直接通过 socket 通信：

```bash
echo "STATUS" | socat - UNIX-CONNECT:/tmp/workflow.sock
```

---

## 插件开发指南

### 插件接口

所有插件必须实现 `WorkflowNode` 抽象基类，并导出 C 工厂函数：

```cpp
// plugins/my_plugin.cpp
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

> **重要**: 插件 `.so` 与主程序是独立编译单元。直接调用 ExecutionContext 的 C++ 成员函数（`set/get/has/remove`）是**不安全**的，因为 `std::any` 的 typeinfo 和内存分配器地址可能跨 .so 边界不同。
>
> **必须**使用 `extern "C"` 辅助函数（`ctx_set_string`、`ctx_get_int64` 等）。这些函数在主程序中实现，通过 `-rdynamic` 链接选项导出。

### extern "C" 上下文 API

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
add_library(my_plugin SHARED plugins/my_plugin.cpp)
target_include_directories(my_plugin PRIVATE include)
set_target_properties(my_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
```

在 `workflow.json` 中指定：

```json
{
  "plugin": "./libmy_plugin.so",
  "nodes": [...] 
}
```

### 插件生命周期

```
dlopen("libmy_plugin.so")
    ↓
dlsym("create_node") → 获取工厂函数
    ↓
create_node() → 创建 WorkflowNode 实例
    ↓
execute(ctx) ← 线程池并发调用（可能多次）
    ↓
destroy_node(ptr) → 释放实例
    ↓
dlclose() → 卸载 .so
```

**热更新**: 修改 .so 源码 → 重新编译 → `./workflow-cli reload` → PluginManager 通过 `atomic_store(shared_ptr)` 无锁切换新实例，旧实例引用释放后自动卸载。

---

## 集成指南

### 方式一：作为独立守护进程

最常见的方式——后台运行，通过 CLI 或 socket 交互：

```bash
nohup ./workflow-engine ../config/workflow.json > engine.log 2>&1 &
./workflow-cli status
```

### 方式二：systemd 服务

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
```

### 方式三：Socket 编程集成

任何语言都可以通过 Unix Domain Socket 与引擎通信：

**Python:**

```python
import socket

def send_command(cmd: str, socket_path: str = "/tmp/workflow.sock") -> str:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)
    sock.sendall(cmd.encode())
    response = sock.recv(4096).decode()
    sock.close()
    return response

print(send_command("STATUS"))
print(send_command("RELOAD"))
```

**Bash:**

```bash
echo "STATUS" | socat - UNIX-CONNECT:/tmp/workflow.sock
```

**Node.js:**

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

### 方式四：嵌入到现有 C++ 项目

直接链接 `workflow-core` 静态库：

```cmake
add_subdirectory(path/to/cpp20-workflow-engine)
target_link_libraries(your_app PRIVATE workflow-core dl pthread)
target_link_options(your_app PRIVATE -rdynamic)
```

```cpp
#include "ThreadPool.h"
#include "PluginManager.h"
#include "WorkflowEngine.h"

ThreadPool pool;
PluginManager plugin_mgr;
WorkflowEngine engine(pool, plugin_mgr);

engine.loadConfig("workflow.json");
engine.run();
engine.waitForCompletion();
engine.getMetrics().print();
```

### 自定义命令扩展

通过 `ControlPlane::register_command()` 注册自定义命令：

```cpp
ControlPlane control_plane;

control_plane.register_command("HEALTH", []() -> std::string {
    return "OK: engine is healthy";
});

control_plane.register_command("METRICS", [&engine]() -> std::string {
    const auto& m = engine.getMetrics();
    return "success=" + std::to_string(m.success_count.load()) +
           " failed=" + std::to_string(m.failed_count.load());
});
```

注册后即可通过 CLI 或 socket 调用：

```bash
echo "HEALTH" | socat - UNIX-CONNECT:/tmp/workflow.sock
```

---

## 技术栈

| 类别 | 技术 |
|------|------|
| **语言标准** | C++20 ( `-std=c++20` ) |
| **构建** | CMake 3.20+ |
| **并发** | `std::jthread`, `std::stop_token`, `std::latch`, `counting_semaphore`, `std::atomic` |
| **系统 API** | `inotify`, `eventfd`, `poll`, `dlopen/dlsym`, Unix Domain Socket |
| **第三方** | [nlohmann/json](https://github.com/nlohmann/json) v3.11.3, [Taskflow](https://github.com/taskflow/taskflow) v3.7.0 (FetchContent 自动拉取) |
| **零运行时依赖** | 无需安装任何第三方库，构建时自动下载 |

---

## 许可证

MIT License
---

## 插件 5：LLM 文本分析插件 (LlmTextAnalyzerNode)

**文件**: `plugins/llm_text_analyzer_plugin.cpp` → `libllm_text_analyzer_plugin.so`

### 功能

读取多个 TXT 文件，将内容发送给大模型 API（支持 OpenAI 兼容接口 / 阿里云通义千问 / 本地 Ollama），执行用户自定义的分析任务（摘要、分类、情感分析、关键词提取等），将分析结果写入输出文件。

### 编译

```bash
sudo apt install libssl-dev
cd build && make llm_text_analyzer_plugin
```

### 主要输入参数

| Key | 必需 | 默认值 | 说明 |
|-----|------|--------|------|
| `llm_input_path` | ✅ | — | TXT 文件路径或目录 |
| `llm_api_key` | ✅ | — | API Key |
| `llm_output_dir` | ❌ | `./llm_output` | 输出目录 |
| `llm_api_endpoint` | ❌ | `https://api.openai.com/v1/chat/completions` | LLM API 端点 |
| `llm_model` | ❌ | `gpt-4o` | 模型名称 |
| `llm_provider` | ❌ | `openai` | `openai` / `dashscope` / `ollama` |
| `llm_task_type` | ❌ | `summary` | `summary` / `keywords` / `sentiment` / `classification` / `qa_extract` / `custom` |
| `llm_output_format` | ❌ | `markdown` | `markdown` / `json` / `txt` |
| `llm_parallel` | ❌ | `2` | 并行请求数 |
| `llm_merge_output` | ❌ | `false` | 是否将所有结果合并为一份总报告 |

### 使用示例

```cpp
ctx_set_string(ctx_ptr, "llm_input_path",  "/data/texts/");
ctx_set_string(ctx_ptr, "llm_api_key",     "sk-xxxxxxxx");
ctx_set_string(ctx_ptr, "llm_provider",    "openai");
ctx_set_string(ctx_ptr, "llm_task_type",   "summary");
ctx_set_string(ctx_ptr, "llm_merge_output","true");
```

输出结果在 `./llm_output/` 目录，合并报告为 `_merged_report.md`。

---

## 插件 6：LLM 驱动音频编辑插件 (LlmAudioEditorNode)

**文件**: `plugins/llm_audio_editor_plugin.cpp` → `libllm_audio_editor_plugin.so`

### 功能

读取 LLM 分析结果文件（插件 5 的输出），解析其中的 JSON 编辑指令，通过 FFmpeg 对 WAV 音频执行切割、拼接、静音插入、片段删除、变速、淡入淡出、音量调整、归一化等操作。

### 依赖

```bash
sudo apt install ffmpeg
cd build && make llm_audio_editor_plugin
```

### 支持的编辑指令类型

| 指令类型 | 说明 |
|----------|------|
| `cut` | 截取指定时间段 |
| `merge` | 拼接多个音频（支持 crossfade） |
| `silence` | 插入静音片段 |
| `delete` | 删除指定片段 |
| `speed` | 变速（支持链式 atempo） |
| `fade` | 淡入淡出 |
| `volume` | 音量调整 |
| `normalize` | 响度归一化 |

### 主要输入参数

| Key | 必需 | 默认值 | 说明 |
|-----|------|--------|------|
| `aed_input_path` | ✅ | — | LLM 分析结果文件路径或目录 |
| `aed_audio_dir` | ✅ | — | WAV 音频文件所在目录 |
| `aed_output_dir` | ❌ | `./audio_edited` | 输出目录 |
| `aed_dry_run` | ❌ | `false` | 试运行模式：只打印命令不执行 |
| `aed_parallel` | ❌ | `2` | 并行 ffmpeg 进程数（文件级别） |
| `aed_overwrite` | ❌ | `false` | 是否覆盖已有文件 |

### 指令文件格式

在 LLM 输出文件中嵌入以下 JSON 块（插件会自动识别 ` ```json ``` ` 代码块）：

```json
{
  "edit_commands": [
    {
      "type": "cut",
      "source": "interview.wav",
      "start": 10.5,
      "end": 45.2,
      "output": "segment_intro.wav",
      "description": "提取开场白"
    },
    {
      "type": "merge",
      "sources": ["segment_intro.wav", "segment_main.wav"],
      "output": "final_cut.wav",
      "transition": "crossfade",
      "crossfade_duration": 0.5,
      "description": "合并精华片段"
    }
  ]
}
```

---

## 完整 AI 流水线

六个插件串联构建完整的内容处理流水线：

```
知乎爬虫           百度网盘下载        视频转音频
ZhihuCrawler  ──▶  BaiduPanDownloader ──▶  VideoToWav
     │                                        │
     │                                        ▼
     │                               语音识别 WavToText
     │                                        │
     └────────────────────────────────────────┤
                                              ▼
                                   LLM 文本分析 LlmTextAnalyzer
                                              │
                                              ▼
                                   LLM 音频编辑 LlmAudioEditor
                                              │
                                              ▼
                                       编辑后 WAV 文件
```

使用 `config/full_ai_pipeline.json` 运行完整六节点流水线：

```bash
./workflow-engine ../config/full_ai_pipeline.json
```

### 各插件共享库与依赖

| 插件 | 共享库 | 系统依赖 |
|------|--------|----------|
| 知乎爬虫 | `libzhihu_crawler_plugin.so` | `libssl-dev` |
| 百度网盘下载 | `libbaidupan_downloader_plugin.so` | `libssl-dev` |
| 视频转音频 | `libvideo_to_wav_plugin.so` | `ffmpeg` |
| 语音识别 | `libwav_to_text_plugin.so` | 阿里云 NLS SDK 或本地 ASR |
| LLM 文本分析 | `libllm_text_analyzer_plugin.so` | `libssl-dev` |
| LLM 音频编辑 | `libllm_audio_editor_plugin.so` | `ffmpeg` |
