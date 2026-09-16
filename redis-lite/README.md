# redis-lite

一个用 C++17 从零实现的高性能 KV 服务，面向 C++ 后端 / 基础架构岗的简历项目。

**核心特征**：多 Reactor 网络层 + 分片存储引擎 + AOF 崩溃一致性 + 有序集合。

---

## 30 秒速览

| 维度 | 数据 |
|---|---|
| 代码规模 | ~6500 行 C++17（含测试），零第三方依赖 |
| 网络模型 | 主从 Reactor，epoll **边缘触发（ET）**，每连接单线程亲和 |
| 存储引擎 | 64 分片哈希表 + 每分片独立互斥量 |
| 数据结构 | 字符串、整数编码、**有序集合（ZSet）** |
| 持久化 | AOF（RESP 格式）+ `always`/`everysec`/`no` 三档 fsync |
| 命令数 | 31 个（含 11 个 ZSET 命令） |
| 单元测试 | **97 用例 / 3348 断言，全部通过**（Windows 与 Linux 双平台） |
| **压测（Linux/epoll ET，2 核 VM）** | **297,942 ops/s，P50 15.8µs，P99 46.8µs** |
| 压测（Windows/select，功能基线） | 136,405 ops/s，P99 88.2µs |
| 崩溃恢复 | `kill -9` 强杀后重启，数据与 TTL 无损 |
| 编译告警 | **0**（`g++ 11.4 -Wall -Wextra` / `MinGW 8.1`） |

**性能关键结论**（完整矩阵见 `docs/TECHNICAL_REPORT.md` 第 6 节）：

- **pipeline 是决定性因素**：单线程下 1→64 让吞吐从 1.6 万涨到 21.8 万（**13.6 倍**）
- **线程数不该超过物理核数**：2 核 VM 上 4 线程比 2 线程**低 3%**
- **低 pipeline 下尾延迟差 23 倍**（P99 1120µs vs 46.8µs），瓶颈在排队而非服务端处理
- `perf` 显示最大的热点只有 3.05%，**profile 平坦**，瓶颈在 syscall 与内存拷贝而非 CPU

---

## 快速开始

### Linux（推荐，走 epoll ET）

```bash
./scripts/build.sh test      # 编译 + 跑单元测试
./build/bin/redis-lite --port 6379 --threads 4 --aof
./build/bin/rl-bench -c 50 -n 200000 -P 16 -d 64 -t set
```

### Windows（开发验证用，走 select）

```bat
scripts\build.bat test
scripts\build.bat run
build-win\rl-bench.exe -c 8 -n 40000 -P 16 -d 64 -t set
```

> Windows 构建必须带 `-D_WIN32_WINNT=0x0600`（脚本已内置）。
> 原因：MinGW 的 `_mingw.h` 默认把目标锁在 XP(0x502)，会让 `inet_pton` 未声明。
> 详见 `docs/IMPLEMENTATION_LOG.md`。

### 用 redis-cli 连接

本实现完整兼容 RESP2 协议，官方客户端可直接连：

```bash
redis-cli -p 6379
> SET user:1 "alice"
> ZADD leaderboard 100 alice 85 bob 95 carol
> ZRANGE leaderboard 0 -1 WITHSCORES
> ZRANK leaderboard bob
```

---

## 支持的命令

**字符串 / 通用**
`PING` `ECHO` `SET`（支持 `EX/PX/EXAT/PXAT/NX/XX/KEEPTTL`）`GET` `DEL` `EXISTS`
`INCR` `DECR` `INCRBY` `DECRBY` `EXPIRE` `PEXPIRE` `PEXPIREAT` `TTL` `PTTL`
`TYPE` `KEYS`（glob 匹配）`DBSIZE` `FLUSHALL` `INFO`

**有序集合**
`ZADD` `ZSCORE` `ZCARD` `ZINCRBY` `ZRANK` `ZREVRANK`
`ZRANGE` `ZREVRANGE` `ZRANGEBYSCORE` `ZCOUNT` `ZREM`

---

## 目录结构

```
redis-lite/
├── include/rl/           公共头文件（15 个）
├── src/                  实现（16 个 .cpp/.cc）
├── tests/                单元测试（4 个套件 + 极简测试框架）
├── tools/bench.cc        自研压测工具（含尾延迟分位数统计）
├── scripts/              Linux / Windows 构建脚本
├── CMakeLists.txt
└── docs/
    ├── TECHNICAL_REPORT.md     ★ 技术报告（架构、取舍、复杂度、性能）
    └── IMPLEMENTATION_LOG.md   ★ 实现踩坑记录（11 个真实问题与定位过程）
```

---

## 分层架构

```
app        main / server / config          进程生命周期、信号、组装
protocol   resp / command                  字节流 ⇄ 命令，RESP2 编解码、命令表
net        event_loop / pool / listener    Reactor、ET 读写、连接生命周期
           connection / manager / poller
storage    store（分片哈希 + TTL）        数据与过期
           zset（双索引有序集合）
           aof（持久化）
common     platform / poller / log        跨平台抽象（唯一出现平台宏的地方）
           stats
```

**依赖严格单向**：`protocol` 不认识 socket，`storage` 不认识网络。
这条线守住之后，替换任何一层都不影响其他层 —— 本项目实测有效：
把 `unordered_map` 换成自研结构、把 ZSet 从跳表换成 `std::set`，
上层命令层一行都没改。

---

## 已完成的验证

```bash
# 单元测试：97 用例 / 3348 断言
./build/bin/rl-tests

# 只跑某一类
./build/bin/rl-tests zset
./build/bin/rl-tests skiplist   # 已移除，见技术报告的取舍说明
```

测试覆盖的关键边界：
- RESP **半包**（逐字节喂入）、**粘包**（一条缓冲多条命令）、CRLF 跨段
- 协议错误（非法长度、尾部无 CRLF、超长 multibulk）
- **二进制安全**：value 含 `\r\n` 与 `\0` 的往返
- 分片哈希的并发正确性（8 线程并发 INCR 无丢失）
- AOF **尾部截断**恢复（模拟掉电写了半条命令）
- ZSet **双索引一致性**（3000 次随机操作与参考实现交叉验证）

---

## 已知缺口（如实记录）

| 缺口 | 原因 | 影响面 |
|---|---|---|
| `ZRANK` 是 O(N) 而非 O(log N) | 用 `std::set` 无排名信息；带 span 的跳表实现未在预算内调通 | 仅 `ZRANK`/`ZREVRANK` |
| 无 AOF 重写（compaction） | 文件随写入单调增长 | 长期运行的磁盘占用 |
| 无主从复制 / 集群 | 单机形态 | 可用性 |
| Windows 后端用 `select` | 消除对 `_WIN32_WINNT` 的构建依赖 | 连接数上限 `FD_SETSIZE` |
| 未支持事务 / 发布订阅 | 与项目主线（单机高性能 KV）无关 | — |

补齐路径与优先级见 `docs/TECHNICAL_REPORT.md` 的「后续演进」章节。

---

## 许可

MIT
