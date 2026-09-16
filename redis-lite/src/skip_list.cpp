// Copyright (c) 2025 redis-lite authors. MIT License.
//
// skip_list.cpp — 带 span 的跳表实现
//
// 本文件的 span 更新公式是经过完整推导的（见文件内注释）。推导要点：
//
//   定义：span[i](x) = rank(target) - rank(x)，target 为空时用虚拟表尾(size+1)
//
//   插入新节点 N（排名 R）后，受影响的是「搜索路径上的 update[i]」：
//     i < new_level  : N 取代了 update[i] 原后继
//     i >= new_level : 后继未变，但它的排名因 N 插入而 +1
//
//   两个区间的写入互不重叠，因此不存在「同一字段被两处写」的问题。
//   （此前版本正是在这里出错：读改写顺序依赖 + 双重写入，见 IMPLEMENTATION_LOG）

#include "rl/skip_list.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "rl/log.h"

namespace rl {

// ================================================================ 节点分配
SkipNode* SkipNodeCreate(int level, SkipScore score, const std::string& member) {
    SkipNode* node = new SkipNode(level, score, member);
    node->forward = new SkipNode*[static_cast<size_t>(level)]();
    node->span = new unsigned int[static_cast<size_t>(level)]();
    return node;
}

void SkipNodeDestroy(SkipNode* node) {
    if (node == nullptr) return;
    delete[] node->forward;
    delete[] node->span;
    delete node;
}

// ================================================================ 构造/析构
SkipList::SkipList() {
    // 头节点不存数据，层数固定为最大层 —— 它是所有层的入口。
    header_ = SkipNodeCreate(kMaxSkipLevel, 0.0, std::string());

    // ★ 空表状态下，按 span 的定义：
    //     span[i] = rank(target) - rank(header)，target 为空时用虚拟表尾。
    //   空表 length_ = 0，虚拟表尾 rank = 1，rank(header) = 0 → span[i] = 1。
    //   不能留成 0：Insert 的第一个元素会读取「原后继的排名」
    //   (rank[i] + span[i])，得到 0 而不是 1，随后算出零 span。
    //   实测症状：插入第一个元素后 Validate 报 "zero span on node m0"。
    for (int i = 0; i < kMaxSkipLevel; ++i) {
        header_->span[i] = 1;
    }
    level_ = 1;
    length_ = 0;
}

SkipList::~SkipList() {
    // ★ 必须显式释放头节点。
    //
    //   Clear() 只释放链表上的数据节点并「复位」头节点，头节点本身是
    //   构造时 new 出来的，不受 Clear 管理。漏掉这一句时每个 SkipList
    //   泄漏约 128 字节（头节点的 32 个 forward + 32 个 span + 节点体）。
    //
    //   这个泄漏由 AddressSanitizer 在 CI 里抓到（2.7MB / 18456 次分配），
    //   单测本身完全感知不到 —— 内存泄漏的典型特征。
    Clear();
    SkipNodeDestroy(header_);
    header_ = nullptr;
}

void SkipList::Clear() {
    SkipNode* node = header_->forward[0];
    while (node != nullptr) {
        SkipNode* next = node->forward[0];
        SkipNodeDestroy(node);
        node = next;
    }
    for (int i = 0; i < kMaxSkipLevel; ++i) {
        header_->forward[i] = nullptr;
        // 与构造时一致：空表下 span = 虚拟表尾 rank - 0 = 1
        header_->span[i] = 1;
    }
    level_ = 1;
    length_ = 0;
}

// ================================================================ 随机层数
int SkipList::NextLevel() {
    if (level_gen_ != nullptr) {
        // 测试注入的生成器：返回 1..max_level。
        const unsigned int lv = level_gen_(level_gen_ctx_, kMaxSkipLevel);
        if (lv < 1) return 1;
        if (lv > static_cast<unsigned int>(kMaxSkipLevel)) return kMaxSkipLevel;
        return static_cast<int>(lv);
    }

    int lv = 1;
    // 每层以 kSkipListP 的概率继续晋升。
    //
    // ⚠️ 不要用位掩码写法（如 (rand() & 0xFFFF) < p * 0x10000）：
    //    MinGW 的 rand() 只有 15 位有效精度，实际分布会偏离预期
    //    （实测 5000 个元素时层数冲到 12，期望约 6）。
    //    归一化比值与平台无关。
    while (lv < kMaxSkipLevel &&
           (static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0)) <
               kSkipListP) {
        ++lv;
    }
    return lv;
}

void SkipList::SetLevelGeneratorForTest(unsigned int (*gen)(void* ctx, int max_level),
                                        void* ctx) {
    level_gen_ = gen;
    level_gen_ctx_ = ctx;
}

// ================================================================ 插入
UpdateResult SkipList::Insert(SkipScore score, const std::string& member) {
    // 按 member 判重（ZSET 语义：member 是唯一键，score 可变）。
    //
    // 教训：判重必须用**唯一键**，不能用排序键 (score, member)。
    // 用排序键判重会导致「ZADD 已存在 member 的新分数」创建重复节点，
    // 污染第 0 层链表、让 rank 累加全错，且错误随插入次数累积。
    //
    // ⚠️ 这次扫描是 O(N)。**生产路径不走这里** —— ZSet 用字典 O(1) 判重后
    //    直接调 InsertNew。这个入口只服务于跳表的独立使用与单元测试。
    //    若把判重留在主插入路径上，ZADD 会退化成 O(N)（实测 N=100 万时不可用）。
    for (SkipNode* existing = header_->forward[0]; existing != nullptr;
         existing = existing->forward[0]) {
        if (existing->member == member) {
            if (existing->score == score) return UpdateResult::kUpdated;
            // 分数变了：必须「删旧 + 插新」。
            // 不能原地改 score —— 那会破坏 (score, member) 的有序性。
            const SkipScore old = existing->score;
            const std::string keep = member;
            Delete(old, keep);
            InsertNew(score, keep);
            return UpdateResult::kUpdated;
        }
    }

    InsertNew(score, member);
    return UpdateResult::kInserted;
}

void SkipList::InsertNew(SkipScore score, const std::string& member) {
    // ============================ 搜索插入位置 ============================
    //
    // update[i]：第 i 层中最后一个 key < 目标 的节点。
    // rank[i]   ：update[i] 的排名（头节点为 0）。
    SkipNode* update[kMaxSkipLevel];
    unsigned long rank[kMaxSkipLevel];

    SkipNode* x = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        rank[i] = (i == level_ - 1) ? 0 : rank[i + 1];
        while (x->forward[i] != nullptr && x->forward[i]->Less(score, member)) {
            // ★ 累加的是 **x 自己的** span[i]，不是后继的。
            //   不变式：rank(x->forward[i]) == rank(x) + span[i](x)
            //   span[i] 描述的是「x 发出的那条指针」。
            //
            //   写成 x->forward[i]->span[i] 是错的，而且极具欺骗性：
            //   第 0 层每个节点的 span 都是 1，两者恰好相等 ——
            //   于是纯第 0 层场景全部正确，只有跨层时排名才偏。
            //   这正是本项目「rank 时对时错」的根因，最终由穷举测试定位。
            rank[i] += x->span[i];
            x = x->forward[i];
        }
        update[i] = x;
    }

    const unsigned long new_rank = rank[0] + 1;
    const int new_level = NextLevel();
    const int old_level = level_;

    // 新层数超过当前高度：补齐中间层。
    // 这些层里新节点是第一个元素，update 是头节点、rank 为 0。
    //
    // ★ header 在这些层的 span 必须初始化为「旧虚拟表尾的排名」= length_ + 1。
    //   原因：新建层的 header->forward[i] 是 nullptr，按定义
    //   span = rank(虚拟表尾) - rank(header) = (length_ + 1) - 0 = length_ + 1。
    //   若漏掉这一步（或写成 length_），后面 old_next_rank 会算错并下溢成
    //   4294967295 —— 这正是此前实测到的现象。
    if (new_level > level_) {
        for (int i = level_; i < new_level; ++i) {
            rank[i] = 0;
            update[i] = header_;
            header_->span[i] = static_cast<unsigned int>(length_ + 1);
        }
        level_ = new_level;
    }

    SkipNode* node = SkipNodeCreate(new_level, score, member);

    // ============================ 更新 span ============================
    //
    // 对 i < new_level：N 插在 update[i] 原后继之前，取代它成为新后继。
    //   old_next_rank[i] = rank[i] + update[i]->span[i]   （原后继或虚拟表尾的排名）
    //   N->span[i]        = old_next_rank[i] + 1 - new_rank
    //       —— 原后继的排名因 N 插入而 +1，故为 (old_next_rank + 1) - new_rank
    //   update[i]->span[i] = new_rank - rank[i]
    //       —— update[i] 的新后继就是 N
    //
    // 自检：i == 0 时 rank[0] + span[0] = old_next_rank，且 new_rank = rank[0]+1。
    //   若原后继为空 → old_next_rank = length_+1，N 成为新尾节点，
    //   N->span[0] = (length_+1) + 1 - (rank[0]+1) = length_ + 1 - rank[0]，
    //   而 N 的排名正是 rank[0]+1，故 span = (length_+1) - new_rank + 1 ✓
    for (int i = 0; i < new_level; ++i) {
        const unsigned long old_next_rank = rank[i] + update[i]->span[i];

        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;

        node->span[i] = static_cast<unsigned int>(old_next_rank + 1 - new_rank);
        update[i]->span[i] = static_cast<unsigned int>(new_rank - rank[i]);
    }

    // 对 new_level <= i < old_level：N 不出现在该层，update[i] 的后继不变，
    // 但该后继的排名因 N 插入而 +1（N 在它之前），故 span += 1。
    //
    // 为什么后继的排名一定 +1：update[i] 是 N 的前驱（rank[i] < new_rank），
    // 而搜索停在这里意味着后继的排名 >= new_rank（等于虚拟表尾时也 > new_rank）。
    // 因此后继一定排在 N 之后，排名整体后移一位。
    for (int i = new_level; i < old_level; ++i) {
        update[i]->span[i] += 1;
    }

    ++length_;
}

// ================================================================ 删除
bool SkipList::Delete(SkipScore score, const std::string& member) {
    SkipNode* update[kMaxSkipLevel];

    SkipNode* x = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->forward[i] != nullptr && x->forward[i]->Less(score, member)) {
            x = x->forward[i];
        }
        update[i] = x;
    }

    x = x->forward[0];
    if (x == nullptr || x->score != score || x->member != member) return false;

    // ---- span 修正 ----
    //
    // 情况一：x 出现在第 i 层（update[i]->forward[i] == x）
    //   删前：update[i] --u--> x --v--> next
    //   删后：update[i] ------u+v-1-----> next
    //   推导：rank(x) = rank(update[i]) + u，rank(next) = rank(x) + v。
    //         删除后 rank(next) 减 1（x 之后的所有节点排名前移），
    //         新 span = (rank(x) + v - 1) - rank(update[i]) = u + v - 1 ✓
    //
    // 情况二：x 不在第 i 层（x->level <= i）
    //   update[i] 的后继不变，但它的排名会 -1（后继排在 x 之后），故 span -= 1。
    //   不会下溢：update[i] 与后继之间至少隔着 x 一个节点，故原 span >= 2。
    for (int i = 0; i < level_; ++i) {
        if (update[i]->forward[i] == x) {
            update[i]->span[i] += x->span[i] - 1;
            update[i]->forward[i] = x->forward[i];
        } else {
            update[i]->span[i] -= 1;
        }
    }

    SkipNodeDestroy(x);
    --length_;

    // 收缩：丢掉顶部已空的层，避免 level_ 只增不减。
    while (level_ > 1 && header_->forward[level_ - 1] == nullptr) {
        --level_;
    }

    return true;
}

// ================================================================ 查找 / 排名
bool SkipList::Find(SkipScore score, const std::string& member) const {
    const SkipNode* x = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->forward[i] != nullptr && x->forward[i]->Less(score, member)) {
            x = x->forward[i];
        }
    }
    x = x->forward[0];
    return x != nullptr && x->score == score && x->member == member;
}

bool SkipList::Rank(SkipScore score, const std::string& member, unsigned long* rank) const {
    // ★ 排名在搜索路径上顺带累加，不做任何额外遍历。
    //   这正是 span 的价值：O(log N) 而不是 O(N)。
    unsigned long r = 0;
    const SkipNode* x = header_;

    for (int i = level_ - 1; i >= 0; --i) {
        while (x->forward[i] != nullptr && x->forward[i]->Less(score, member)) {
            // ★ x 自己的 span（见 Insert 里的详细说明）
            r += x->span[i];
            x = x->forward[i];
        }
    }

    x = x->forward[0];
    if (x == nullptr || x->score != score || x->member != member) return false;

    if (rank != nullptr) *rank = r;  // 0-based
    return true;
}

void SkipList::RangeByRank(unsigned long start, unsigned long stop,
                           std::vector<const SkipNode*>* out) const {
    // start/stop 为 1-based 闭区间。
    if (out == nullptr || start > stop || start > length_) return;

    const SkipNode* x = header_;

    // 先用 span 一次跳过多层，把 O(start) 降到 O(log N + 剩余步数)。
    //
    // ★ 条件必须是 traversed + span < start（不是 <=）：
    //   目标是把 traversed 停在 start - 1，即「第 start 个元素的前驱」。
    //   写成 <= 时若某步恰好把 traversed 推到 start，就停在「第 start 个元素
    //   自己」身上，随后再走一步会跳到 start + 1 ——
    //   表现为「范围查询总是少返回第一个元素」。
    unsigned long traversed = 0;
    for (int i = level_ - 1; i >= 0; --i) {
        // ★ 同样用 x 自己的 span（见 Insert 里的详细说明）
        while (x->forward[i] != nullptr && traversed + x->span[i] < start) {
            traversed += x->span[i];
            x = x->forward[i];
        }
    }

    // 此时 rank(x) == start - 1，走一步即到第 start 个元素。
    x = x->forward[0];

    for (unsigned long i = start; i <= stop && x != nullptr; ++i) {
        out->push_back(x);
        x = x->forward[0];
    }
}

const SkipNode* SkipList::LowerBoundByScore(SkipScore min_score) const {
    // 只按 score 比较的第一个节点（ZRANGEBYSCORE 的起点）。
    const SkipNode* x = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->forward[i] != nullptr && x->forward[i]->score < min_score) {
            x = x->forward[i];
        }
    }
    return x->forward[0];
}

// ================================================================ 不变式自检
bool SkipList::Validate(std::string* err) const {
    auto fail = [err](const std::string& msg) {
        if (err != nullptr) *err = msg;
        return false;
    };

    // 第一步：沿第 0 层走一遍，得到每个节点的真实 rank。
    // 用真实 rank 反查 span，这是「按定义校验」而不是「按实现校验」。
    std::vector<const SkipNode*> nodes;
    std::unordered_map<const SkipNode*, unsigned long> rank_of;
    nodes.reserve(length_ + 1);
    rank_of.reserve((length_ + 1) * 2);
    nodes.push_back(header_);
    rank_of[header_] = 0;

    unsigned long r = 0;
    for (const SkipNode* p = header_->forward[0]; p != nullptr; p = p->forward[0]) {
        nodes.push_back(p);
        rank_of[p] = ++r;
        if (nodes.size() > length_ + 1) {
            return fail("cycle detected on level 0 (more nodes than length_)");
        }
    }

    if (r != length_) {
        return fail("level-0 node count = " + std::to_string(r) +
                    ", expected length_ = " + std::to_string(length_));
    }

    // 第 0 层必须是严格递增的（keys 唯一且有序）
    for (size_t i = 1; i < nodes.size(); ++i) {
        if (!nodes[i - 1]->Less(nodes[i]->score, nodes[i]->member)) {
            return fail("level-0 ordering violated at index " + std::to_string(i));
        }
    }

    const unsigned long tail_rank = static_cast<unsigned long>(length_) + 1;

    // 第二步：逐层、逐链路，按定义验证 span。
    //
    //   ★ span[i] 存在**发出指针的那个节点自己身上**：
    //       span[i](p) == rank(p->forward[i]) - rank(p)
    //     所以检查的是 p->span[i]，不是 target->span[i]。
    //
    //   ⚠️ 这一点我写错过一次，导致自检误报 "span mismatch on m0
    //      (span=2, expected 1)" —— 算法本身是对的，是校验读错了字段。
    //      （与 IMPLEMENTATION_LOG 里「自检函数统计口径错误」是同一类问题：
    //       排查时既要怀疑被测代码，也要怀疑测试代码。）
    for (int i = 0; i < level_; ++i) {
        const SkipNode* p = header_;
        while (p->forward[i] != nullptr) {
            const SkipNode* target = p->forward[i];
            const unsigned long p_rank = rank_of[p];
            const unsigned long t_rank = rank_of[target];

            // 出现在第 i 层的节点，其 level 必须 > i
            if (target->level <= i) {
                return fail("level " + std::to_string(i) +
                            ": reachable node has level <= " + std::to_string(i));
            }

            // span 至少为 1（后继严格排在自己之后）
            if (p->span[i] == 0) {
                return fail("level " + std::to_string(i) + ": zero span on node " +
                            (p == header_ ? std::string("<header>") : p->member));
            }

            if (p->span[i] != t_rank - p_rank) {
                return fail("level " + std::to_string(i) + ": span mismatch on node " +
                            (p == header_ ? std::string("<header>") : p->member) +
                            " at level " + std::to_string(i) + " (span=" +
                            std::to_string(p->span[i]) + ", expected " +
                            std::to_string(t_rank - p_rank) + ")");
            }

            p = target;
        }
    }

    // 第三步：nullptr 后继的节点，其 span 必须覆盖到虚拟表尾。
    //
    // ⚠️ 只检查 **活动层**（i < level_）：header 的 level 是 kMaxSkipLevel，
    //    若不加这个限制，会对 level_ 以上的非活动层做检查 ——
    //    那些层的 span 不参与维护，会误报。
    for (const SkipNode* p : nodes) {
        const int lim = (p->level < level_) ? p->level : level_;
        for (int i = 0; i < lim; ++i) {
            if (p->forward[i] == nullptr) {
                const unsigned long expect = tail_rank - rank_of[p];
                if (p->span[i] != expect) {
                    return fail("level " + std::to_string(i) + ": tail span mismatch on " +
                                (p == header_ ? std::string("<header>") : p->member) +
                                " (span=" + std::to_string(p->span[i]) + ", expected " +
                                std::to_string(expect) + ")");
                }
            }
        }
    }

    // 第四步：校验层数收缩不变式 —— 顶层必须有节点。
    if (level_ > 1 && header_->forward[level_ - 1] == nullptr) {
        return fail("level_ = " + std::to_string(level_) +
                    " but the top level is empty (should have been shrunk)");
    }

    if (err != nullptr) err->clear();
    return true;
}

// ================================================================ 内存
size_t SkipList::MemoryUsage() const {
    size_t total = sizeof(SkipList);
    total += sizeof(SkipNode);
    total += static_cast<size_t>(kMaxSkipLevel) * (sizeof(SkipNode*) + sizeof(unsigned int));

    for (const SkipNode* node = header_->forward[0]; node != nullptr; node = node->forward[0]) {
        total += sizeof(SkipNode);
        total += node->member.capacity();
        total += static_cast<size_t>(node->level) * (sizeof(SkipNode*) + sizeof(unsigned int));
    }
    return total;
}

// ================================================================ score 格式化
std::string SkipScoreToString(SkipScore score) {
    char buf[64];
    if (score == static_cast<SkipScore>(static_cast<long long>(score))) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(score));
    } else {
        std::snprintf(buf, sizeof(buf), "%.17g", score);
    }
    return std::string(buf);
}

bool ParseSkipScore(const char* data, size_t len, SkipScore* out) {
    if (data == nullptr || len == 0 || len >= 64) return false;

    char buf[64];
    std::memcpy(buf, data, len);
    buf[len] = '\0';

    // 显式拒绝 inf / nan（与 Redis 的 ZADD 行为一致）
    char lower[64];
    for (size_t i = 0; i <= len; ++i) {
        char c = buf[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (std::strstr(lower, "inf") != nullptr || std::strstr(lower, "nan") != nullptr) {
        return false;
    }

    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == nullptr || *end != '\0' || end == buf) return false;  // 尾部垃圾必须拒绝
    if (!std::isfinite(v)) return false;

    if (out != nullptr) *out = v;
    return true;
}

}  // namespace rl
