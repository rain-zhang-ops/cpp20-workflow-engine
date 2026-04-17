# cpp20-workflow-engine

极低资源占用的 C++20 工作流引擎，针对 Linux (WSL) 环境设计。

## 核心特性

- **ConfigWatcher** — inotify + jthread 零 CPU 配置监听
- **ThreadPool** — counting_semaphore 高效线程池
- **PluginManager** — atomic shared_ptr 无锁热更新
- **WorkflowEngine** — DAG 原子依赖计数并发调度

## 技术栈

- C++20 (`-std=c++20`)
- Linux 原生 API (inotify, dlfcn, eventfd, poll)
- 零第三方依赖

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 许可证

MIT License


git clone https://github.com/rain-zhang-ops/cpp20-workflow-engine.git && cd cpp20-workflow-engine/ && mkdir build && cd build && cmake .. && make -j 12
