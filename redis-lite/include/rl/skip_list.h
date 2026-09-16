// Copyright (c) 2025 redis-lite authors. MIT License.
//
// skip_list.h — 带 span 的跳表（ZSet 的有序索引）
//
// ============================ 为什么需要它 ============================
//
// ZSet 的三类操作要求不同的复杂度：
//   1) ZSCORE / ZINCRBY  按 member 查 score      → O(1)     [由 dict 承担]
//   2) ZRANGE / ZRANGEBYSCORE 范围遍历            → O(log N + M)
//   3) ZRANK 按 member 查排名（第几名）           → O(log N) [本文件]
//
// 第 3 条是关键：std::set 的节点**不知道自己的排名**，只能
// std::distance(begin, it) 数出来，平均 O(N/2)。
// 红黑树要支持它必须额外维护 size 域并在旋转时修正，实现复杂。
//
// 跳表用 span 天然解决：每条前向指针附带「它跨过了多少个第 0 层节点」，
// 于是沿途累加就是排名。这是 Redis 选跳表做 ZSET 的主要理由之一。
//
// ============================ span 的定义（唯一权威） ============================
//
//   rank(header) = 0，第 1 个真实节点 rank = 1，依次递增。
//   记「虚拟表尾」的 rank = size + 1（它在第 0 层尾节点之后）。
//
//   ★  span[i](x) = rank(target) - rank(x)
//      其中 target = x->forward[i]（非空时），否则为虚拟表尾。
//
// 由该定义直接推出贯穿全篇的核心不变式：
//
//   ★  rank(x->forward[i]) == rank(x) + span[i](x)
//
// 以及两个推论：
//   - 第 0 层每条 span 都是 1（相邻节点排名差 1），尾节点覆盖到虚拟表尾
//   - 「累加 span」直接得到排名，没有任何 ±1 修正
//
// ⚠️ 历史教训：我最初把 span 理解成「跨度/距离」并让哨兵为 0，与「rank 差」
//    只差 1，但**每一层的偏差不同**，导致 rank「有时对有时错」——
//    这是本项目最难定位的一类 bug。现在定义只有一条式子，其余全部由它推导。
//
// ============================ 并发说明 ============================
// 非线程安全，需调用方加锁。本项目由 Store 的分片互斥量提供。

#ifndef RL_SKIP_LIST_H
#define RL_SKIP_LIST_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rl {

// 最大层数：可高效支撑 2^32 个元素（Redis 同为 32）。
constexpr int kMaxSkipLevel = 32;

// 晋升概率 1/4。平均每节点指针数 1/(1-p) = 1.33，
// 比 1/2 更省内存，代价是查找多约 1 步。Redis 用 0.25。
constexpr double kSkipListP = 0.25;

using SkipScore = double;

// ---------------------------------------------------------------- 节点
// struct 而非 class：上层需要读 forward/span 来构造响应，
// 刻意把「遍历能力」暴露出去，避免为每种查询写一个回调。
struct SkipNode {
    std::string member;
    SkipScore score = 0.0;

    // 变长数组：实际分配 level 个元素。
    // 不用 std::vector 是为了让节点一次分配完，减少分配次数、保持内存连续。
    SkipNode** forward = nullptr;
    unsigned int* span = nullptr;
    int level = 1;

    SkipNode(int lv, SkipScore sc, std::string m)
        : member(std::move(m)), score(sc), level(lv) {}

    // ZSET 的全序：先比 score，score 相同比 member 字典序。
    bool Less(SkipScore other_score, const std::string& other_member) const {
        if (score != other_score) return score < other_score;
        return member < other_member;
    }
};

SkipNode* SkipNodeCreate(int level, SkipScore score, const std::string& member);
void SkipNodeDestroy(SkipNode* node);

enum class UpdateResult { kInserted, kUpdated };

class SkipList {
public:
    SkipList();
    ~SkipList();

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // ---------------------------------------------------------------- 写
    // 插入**已知不存在**的 member。不做判重，O(log N)。
    //
    // ★ 这是 ZSet 使用的入口：member 的存在性由 ZSet 的字典 O(1) 判掉，
    //   跳表不需要再扫一遍。
    //
    //   为什么必须分成两个入口：跳表按 (score, member) 排序，只有 member
    //   无法直接定位，判重只能线性扫 —— 若把判重放进主插入路径，
    //   每次 ZADD 都变成 O(N)，规模一大就崩（N=100 万时不可用）。
    //   这是本项目实现过程中真实踩到的一处性能陷阱。
    void InsertNew(SkipScore score, const std::string& member);

    // 插入或更新（**含 O(N) 判重**）。member 已存在则更新 score。
    // 供跳表独立使用与单元测试；ZSet 走 InsertNew。
    UpdateResult Insert(SkipScore score, const std::string& member);

    // 按 (score, member) 删除。
    bool Delete(SkipScore score, const std::string& member);

    // 按 (score, member) 精确定位。找到返回 true。
    // 复杂度 O(log N)：key 有序，直接走跳表搜索。
    bool Find(SkipScore score, const std::string& member) const;

    // ---------------------------------------------------------------- 排名
    // ZRANK，**0-based**（与 Redis 一致）。不存在返回 false。
    // ★ O(log N)：搜索路径上顺带累加 span 即得排名。
    bool Rank(SkipScore score, const std::string& member, unsigned long* rank) const;

    // ZRANGE by rank。start/stop 为 **1-based 闭区间**（由调用方归一化）。
    void RangeByRank(unsigned long start, unsigned long stop,
                     std::vector<const SkipNode*>* out) const;

    // ZRANGEBYSCORE 的起点：第一个 score >= min_score 的节点。
    // 返回 nullptr 表示没有满足条件的节点。
    const SkipNode* LowerBoundByScore(SkipScore min_score) const;

    // ---------------------------------------------------------------- 访问
    size_t size() const { return length_; }
    int level() const { return level_; }
    const SkipNode* header() const { return header_; }
    const SkipNode* first() const { return header_->forward[0]; }

    // 最后一个节点。从最高层一路走到尽头 —— 顶层节点很少，近似 O(log N)。
    // 空表返回 nullptr。
    const SkipNode* Last() const {
        const SkipNode* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->forward[i] != nullptr) x = x->forward[i];
        }
        return (x == header_) ? nullptr : x;
    }

    size_t MemoryUsage() const;
    void Clear();

    // ---------------------------------------------------------- 不变式自检
    // 校验 span 的全部不变式。出错时把细节写进 err 并返回 false。
    //
    // 为什么单独写这个函数（本项目最重要的工程决定之一）：
    //   span 的 bug 不崩溃、不报错，只让 rank 慢慢漂移。
    //   我手工推演两遍都没定位到，而把「该满足什么」显式写成断言后，
    //   问题第一次运行就自己暴露了。**先定义正确性，再调算法。**
    bool Validate(std::string* err = nullptr) const;

    // 仅供测试：接管层数生成，用于构造可复现的层数分布。
    // 传 nullptr 恢复默认随机。生产路径不应使用。
    void SetLevelGeneratorForTest(unsigned int (*gen)(void* ctx, int max_level), void* ctx);

private:
    int NextLevel();

    SkipNode* header_ = nullptr;
    int level_ = 1;      // 当前有效层数
    size_t length_ = 0;  // 元素个数

    unsigned int (*level_gen_)(void* ctx, int max_level) = nullptr;
    void* level_gen_ctx_ = nullptr;
};

// score ↔ 字符串。整数不带小数点，非整数用 %.17g 保证往返精度。
std::string SkipScoreToString(SkipScore score);
bool ParseSkipScore(const char* data, size_t len, SkipScore* out);

}  // namespace rl

#endif  // RL_SKIP_LIST_H
