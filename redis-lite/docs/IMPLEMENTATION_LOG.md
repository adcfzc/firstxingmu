# 跳表实现踩坑记录（IMPLEMENTATION_LOG）

> 这份文件是**技术报告的原料**，不是给人看的文档。
> 面试时最有价值的内容不是"我实现了跳表"，而是"我在这 6 个地方判断错了，
> 每一个的错误表现是什么、我怎么定位的"。
>
> 保真度要求：只记录真实发生过的、可复现的现象。

---

## 背景

目标：为 ZSET 实现带 `span` 的跳表，支持 O(log N) 的 `ZRANK` / `ZRANGE`。
参考设计：Redis `t_zset.c` 里的 `zskiplist`。

环境：Windows + MinGW g++ 8.1（`-Wall -Wextra`），Linux/epoll 为目标平台。

---

## 坑 1：`RandomLevel` 的晋升概率在 MinGW 上失真

**代码**

```cpp
// 错误版本
while (lv < kMaxSkipLevel && (std::rand() & 0xFFFF) < static_cast<int>(kSkipListP * 0x10000))
```

**现象**：5000 个元素时跳表层数冲到 **12**，而 p=0.25 的理论期望约为 `log₄(5000) ≈ 6`。

**定位过程**：写了独立调试程序，每 1000 次插入打印一次 `level()`。
数值上 `16384/65535 ≈ 0.25` 看似正确，但 `std::rand()` 在 MinGW 只有 15 位精度
（`RAND_MAX = 32767`），低 16 位里有一位恒为 0，实际分布偏离预期。

**修复**：改成与平台无关的归一化比较

```cpp
while (lv < kMaxSkipLevel &&
       (static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0)) < kSkipListP)
```

**修复后**：N=5000 时 level=7，落在合理区间。

**教训**：用 `rand()` 做位运算来构造概率，隐含依赖 `RAND_MAX` 的实现细节。
跨平台代码里应始终用比值而不是位掩码。

---

## 坑 2：判重用了排序键而不是唯一键

**现象**：`sl.Insert(5.0, "a")` 在 `"a"`（原 score=1.0）已存在时，
**创建了重复节点**，size 变成 3（期望 2）。

**根因**：跳表按 `(score, member)` 排序，我把判重也写成了「两者都相等」：

```cpp
// 错误：要求 score 与 member 同时匹配
if (x != nullptr && x->score == score && x->member == member)
```

但 ZSET 的语义是 **member 唯一、score 可变**。

**连锁后果**（这才是代价所在）：
1. size 比预期大
2. 第 0 层链表被重复项污染
3. rank 累加全错，且错误随插入次数**累积放大**
4. 表现为「rank 时对时错」，极难反推

**修复**：判重只按 member，命中且 score 不同时执行「删除 + 重新插入」
（不能原地改 score，那会破坏有序性）。

**教训**：**排序键 ≠ 唯一键**。判重、索引、去重一律用唯一键；
排序键只用于决定位置。这两者在设计文档里必须分开写。

---

## 坑 3：`span` 的语义定义有歧义（代价最大的一处）

**现象**：rank 查询系统性错误，例如 3 个元素时 `m2` 的 rank 是 4 或 5 而不是 3。

**根因**：我一开始把 `span` 理解为「**距离**」（走几步），
于是哨兵节点的 span 可以是 0。但正确语义是「**覆盖的节点数**」：

| 语义 | 哨兵 span | 不变式 |
|---|---|---|
| 距离（错误） | 0 | Σspan = 可达数 − 1 |
| 覆盖数（正确） | ≥ 1 | Σspan = 可达数 |

两种理解在**单点**上看着都能自圆其说，但累积到 rank 查询时就差 1，
而且**每一层的偏差不同**，导致「有时对、有时错」。

**定位手段**：写了 `SkipList::Validate()` 做不变式自检 ——
这个决定是整个调试过程里最有价值的一步。手推两遍都没找到的问题，
自检第一次运行就精确指出了「第 0 次插入后 level-0 span sum = 0, expected = 1」。

**修复**：改用覆盖语义，并把权威式子唯一定义为

```
span[i] == rank(同层后继) - rank(自己)
```

**教训**：**给核心数据结构的每个字段写下「定义 + 不变式 + 例子」**，
不要只在注释里说「跨度」。歧义会在代码里以最难查的方式显形。
另外：**先写自检函数，再写算法**。

---

## 坑 4：自检函数自己的统计口径错了（假阳性）

**现象**：自检报 `level-0 span sum = 0, expected 1`，但手工推演证明算法是对的。

**根因**：自检按「遍历所有节点、读 `forward[i]`、跳过 nullptr」来统计各层 span 之和。
但 **span 挂在目标节点上，而该层最后一个节点的 `forward[i]` 是 nullptr** ——
它那个 span 依然有意义，却被 `continue` 跳过了，导致统计恰好少 1。

**修复**：改为**逐层沿着 `header_->forward[i]` 走链**，

```cpp
for (const SkipNode* p = header_; p->forward[i] != nullptr; p = p->forward[i]) {
    span_sum += p->forward[i]->span[i];
    ++reachable;
}
```

**教训**：**测试代码本身也是代码，也会有 bug。**
出现「算法推演正确但测试报错」时，要同时怀疑两边 ——
我在这上面浪费的时间不比算法本身少。

---

## 坑 5：`Insert` 里存在「读改写」的顺序依赖

**现象**：`rank[0]` 打印出来完全正确，但插入完成后部分元素的 rank 漂移。

**根因**：同一个节点常常在相邻两层都充当 `update[i]`（搜索路径在第 i 层和
第 i+1 层停在同一节点）。而我在同一次循环里既读又写：

```cpp
// 错误：第 i 次迭代可能破坏第 i+1 次要读的值
node->span[i] = update[i]->span[i] - (new_rank - rank[i]);
update[i]->span[i] = (new_rank - rank[i]) + 1;
```

**修复**：**先快照，后写入**。把 `old_span[]` / `old_next[]` 提前算好，
主循环只做写入，不再读自身正在修改的位置。

**教训**：任何「原地更新」的循环，都要先问一句：
**后续迭代会不会读到本次写坏的值？** 有依赖就必须先快照。

---

## 坑 6：高层 span 的双重写入

**现象**：`m1->span[1]` 的值是 3，而按定义应该是 2。

**根因**：两段循环都写了同一个字段：

```cpp
for (int i = 0; i < new_level; ++i)        update[i]->span[i] = d;      // 写第一遍
for (int i = new_level; i < old_level; ++i) update[i]->span[i] += 1;    // 又加一次
```

**修复**：新节点不出现在的高层，其覆盖数**必须由快照旧值推导**：

```cpp
update[i]->span[i] = old_span[i] + 1;
```

不能写 `+= 1`（可能被写过两遍），也不能写 `= d`（`d` 在该层无意义 ——
新节点在这一层并不存在）。

**教训**：多个循环写同一个字段时，**必须能说清每个字段的唯一写入者**。
否则就需要 merge 语义，而 merge 是最容易出错的。

---

## 环境相关的坑（与算法无关，但同样耗时）

### MinGW 的 `_WIN32_WINNT` 默认值

`WSAPoll` / `WSAPOLLFD` / `POLLRDNORM` / `inet_pton` / `inet_ntop`
在 MinGW 8.1 下全部「未声明」。

**根因**：MinGW 的 `<_mingw.h>`（经 `<cstdio>` 等间接引入）会写入默认
`_WIN32_WINNT = 0x502`(XP)，而上述 API 都是 Vista(0x600) 才引入的。
一旦 `_mingw.h` 先跑，头文件里的 `#ifndef` 就再无机会。

**处理**：
- 事件多路复用后端**改用 `select()`** —— 对 Windows 版本零要求，
  顺带消除了对编译选项的依赖（`select` 的代价是 `FD_SETSIZE` 上限，见 `poller.cpp`）。
- `inet_pton` / `inet_ntop` 无替代品，故在 `platform.h` 里加 `#error` 强制提示，
  并在 `build.bat` 中固定 `-D_WIN32_WINNT=0x0600`。

**教训**：**在错误的战场上优化要及时止损。**
我在这上面花了远超预期的时间，而正确的动作是「换一个有零依赖的实现」，
而不是继续研究宏的包含顺序。

### 链接期找不到 `main`

`Get-ChildItem -Filter *.cpp` **不匹配 `main.cc`**，导致构建脚本从未把
`src/main.cc` 编进去，链接期报 `undefined reference to WinMain`。
同一个坑踩了两次。

**教训**：构建脚本里的文件收集规则要显式覆盖所有扩展名，
或者干脆统一扩展名。隐式 glob 是构建脆弱性的主要来源。

---

## 坑 7：首次在 Linux 上编译，暴露 3 个被 Windows 掩盖的头文件缺失

**背景**：Windows 上编译干净（0 error 0 warning），
我据此认为代码是可移植的。直到把代码传到 Linux 虚拟机首次编译，才发现不是。

**现象**：`platform.cpp` 在 Linux 下报 6 个「未声明」：

```
error: 'addrinfo' was not declared in this scope
error: 'getaddrinfo' was not declared in this scope
error: 'gai_strerror' was not declared in this scope
error: 'freeaddrinfo' was not declared in this scope
error: 'eventfd' was not declared in this scope
error: 'EFD_NONBLOCK' was not declared in this scope
error: 'EFD_CLOEXEC' was not declared in this scope
```

**根因**：`platform.h` 的 Linux 分支漏了两个头文件：

| 缺失头文件 | 提供的符号 |
|---|---|
| `<netdb.h>` | `addrinfo` / `getaddrinfo` / `freeaddrinfo` / `gai_strerror` |
| `<sys/eventfd.h>` | `eventfd` / `EFD_NONBLOCK` / `EFD_CLOEXEC` |

**为什么 Windows 上完全没暴露**：
`platform.h` 用 `#if RL_WINDOWS / #else` 分成两个分支，
Windows 分支引入了 `<ws2tcpip.h>`（提供 `getaddrinfo` 等）和
`<mstcpip.h>`，而 `Notifier` 在 Windows 上走的是 **socketpair** 实现，
根本不调用 `eventfd`。于是这两个头文件在 Windows 上「不需要」，
问题被完整掩盖。

**修复**：在 Linux 分支补上两个 include，并加注释说明用途。

**教训（这条最重要）**：

> **「在一个平台上编译通过」不等于「代码是可移植的」，
> 尤其当代码里有 `#ifdef` 分支时 —— 它只证明了一个分支能过。**

具体到本项目：`platform.h` 是唯一出现平台宏的地方（这是刻意的分层设计），
而恰恰是这个文件从未在 Linux 上编译过。**跨平台抽象层的两个分支
必须都在各自的原生平台上编译验证**，否则这个抽象层就是"未经验证的抽象"。

**延伸思考**：这也是为什么"CI 上跑多平台构建"不是形式主义。
如果本项目有 Linux CI，这个问题在第一次提交时就会暴露，
而不是等到交付前才在虚拟机上发现。

---

## 坑 8：Windows 打包的 tar 不保留 Unix 权限位

**现象**：在 Windows 上用 `tar -czf` 打包源码，传到 Linux 解压后：

```
bash: ./scripts/build.sh: 权限不够
```

**根因**：Windows 文件系统没有 Unix 权限位，bsdtar 打出的归档里
文件权限是默认值，`chmod +x` 的结果没有被保留。

**修复**：解压后先 `chmod +x scripts/*.sh`。
更稳妥的做法是在脚本调用处就用 `bash scripts/build.sh`
（不依赖执行位），本项目两种都支持。

**教训**：**跨平台交付时要显式处理权限**，不能假设它会被保留。
另外 `scripts/build.sh` 的用法说明里应该写明用 `bash` 调用，
而不是 `./`。

---

## 坑 9：`perf` 被 `perf_event_paranoid=4` 拦住

**现象**：`perf record -p <pid>` 报
「Access to performance monitoring and observability operations is limited」。

**根因**：Ubuntu 22.04 的默认 `kernel.perf_event_paranoid` 是 4
（比常见的 2 更严格），普通用户无法采样。

**修复**：`sudo sysctl -w kernel.perf_event_paranoid=1`（临时，重启失效）。

**教训**：性能分析工具在受限环境里会直接被拦。
**排查性能问题前先确认 profiling 工具真的能跑**，
否则会误以为"没有热点"。

---

## 坑 10：关闭路径跨线程操作事件循环（ASan 端到端测试才发现）

**现象**（在 Linux + ASan 下端到端测试）：

```
[FATAL] event_loop.cpp:39 EventLoop called from wrong thread (expected loop thread)
redis-lite ... 已中止（核心已转储）
```

**关键背景**：这不是压测能发现的 bug。
跑 30 万请求全部正常，**一收到 SIGTERM 就 abort**。

**根因**：两条独立的跨线程违规：

1. `Listener::Stop()` 由控制线程调用，直接调 `main_loop()->Unregister()`。
   `Unregister` 有 `AssertInLoopThread` 断言 → 直接 abort。
2. `ConnectionManager::CloseAll()` 在控制线程遍历所有连接并调用 `Close()`，
   而连接的 fd 注册在 **Sub Loop** 上。`Close()` → `MarkClosed()` →
   `loop_->Unregister()` 又是一次跨线程。

**为什么正常流量从没暴露**：连接平时都是**客户端先断开**，
走的路径是「连接自己的 loop 线程里发现对端关闭」——线程亲和天然正确。
「服务端主动关闭」是另一条执行路径，只有优雅退出才触发。
**测试覆盖了前者，后者是盲区。**

**修复**：
1. `Listener::Stop` 把 Unregister 投递到主 loop 线程（`QueueToLoop`），
   在投递任务里先摘 epoll 再关 fd。
2. `Connection::Close()` 开头做线程亲和检查：不在本 loop 线程就
   `QueueToLoop` 投递过去（用 `shared_from_this` 保活）。

**教训（两条，都很重要）**：

1. **「测试通过」只证明被测过的路径正确。**
   要显式问：哪些执行路径从未被执行过？
   —— 答案是关闭路径，因为所有测试都以"客户端断开"收尾。
2. **sanitizer 不只是测内存。**
   它测的是「另一种运行方式」：ASan 下进程 abort 得比 Release 更干脆，
   让我在第一次优雅退出就抓到了问题，而不是等用户报"退出时闪退"。

**这条 bug 直接催生了 CI**：`.github/workflows/ci.yml` 里专门加了
「shutdown path under sanitizers」一步，跑 `scripts/test_shutdown.sh`。
CI 的意义不是"再跑一遍已知的测试"，而是**把盲区变成例行检查**。

---

## 汇总：这些记录在面试里怎么用

| 坑 | 能体现的能力 |
|---|---|
| 3（span 语义） | 数据结构的严谨定义能力；**主动写不变式自检**的工程习惯 |
| 4（自检自身有 bug） | 不盲信工具，能同时怀疑被测量与测量工具 |
| 2（排序键 vs 唯一键） | 领域语义理解（ZSET 的 member 是主键） |
| 5 / 6（读改写、双重写入） | 对「原地更新」这类隐蔽依赖的敏感度 |
| RandomLevel | 跨平台伪随机数的实现差异意识 |
| **7（Linux 首次编译）** | **理解「条件编译分支必须分别验证」；对跨平台抽象的清醒认识** |
| **10（关闭路径崩溃）** | **识别测试盲区：压测覆盖不到的路径（服务端主动关闭）要用专门测试补上** |
| 8（tar 权限位） | 跨平台交付的细节意识 |
| 9（perf 被拦） | 工具受限时的排查思路 |
| `_WIN32_WINNT` | 能判断「什么该修、什么该绕」，及时止损 |

**最值得讲的两条**：

1. **坑 3 的定位过程** —— 手工推演两遍都没找到，
   最后靠**先写自检函数**才让问题自己暴露。
   这个顺序（先定义正确性，再实现算法）比任何具体技巧都更有价值。

2. **坑 7 的盲区** —— "Windows 编译 0 告警"给了我可移植性的**虚假信心**。
   跨平台代码里，`#ifdef` 的每个分支都必须在其原生平台上验证过，
   否则那个抽象层只是"看起来抽象"。
