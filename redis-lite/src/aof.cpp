// Copyright (c) 2025 redis-lite authors. MIT License.
//
// aof.cpp — AOF 实现。

// include 顺序：log.h 必须最先，理由见 platform.h 顶部注释。
// platform.h 紧随其后：Linux 下 fsync(2) 需要 <unistd.h> 的声明，
// 而它必须在 <cstdio> 之前被拉进来。
#include "rl/log.h"
#include "rl/platform.h"

#include "rl/aof.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "rl/resp.h"

#if RL_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rl {

namespace {

// strerror 返回 char*，直接和 const char* 相加是编译错误。
// 统一包一层，也顺便让错误信息带上 errno 数值（排查时比纯文本有用）。
std::string ErrnoText(int e) {
    return "errno=" + std::to_string(e) + " " + std::strerror(e);
}

// 真正的 fsync。Windows 上没有直接等价物，_commit 是最接近的语义
// （把 FILE* 缓冲与 OS 缓存都刷到磁盘）。
bool FsyncFile(std::FILE* fp) {
#if RL_WINDOWS
    return _commit(_fileno(fp)) == 0;
#else
    // Linux 下走 fsync(2)。需要 <unistd.h>，由 platform.h 间接引入。
    return ::fsync(fileno(fp)) == 0;
#endif
}

}  // namespace

const char* AofPolicyName(AofSyncPolicy p) {
    switch (p) {
        case AofSyncPolicy::kAlways:
            return "always";
        case AofSyncPolicy::kEverySec:
            return "everysec";
        case AofSyncPolicy::kNo:
            return "no";
    }
    return "unknown";
}

bool ParseAofPolicy(const std::string& s, AofSyncPolicy* out) {
    if (s == "always") {
        *out = AofSyncPolicy::kAlways;
        return true;
    }
    if (s == "everysec" || s == "every_sec") {
        *out = AofSyncPolicy::kEverySec;
        return true;
    }
    if (s == "no" || s == "none") {
        *out = AofSyncPolicy::kNo;
        return true;
    }
    return false;
}

Aof::Aof() { buffer_.reserve(64 * 1024); }

Aof::~Aof() { Close(); }

bool Aof::Open(const std::string& path, AofSyncPolicy policy, std::string* err) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }

    path_ = path;
    policy_ = policy;

    // "ab"：追加写，文件不存在则创建。追加模式天然避免覆盖历史数据。
    fp_ = std::fopen(path.c_str(), "ab");
    if (fp_ == nullptr) {
        const int e = errno;
        if (err != nullptr) {
            *err = "fopen(" + path + ", ab) failed: " + ErrnoText(e);
        }
        return false;
    }

    // 定位到文件末尾，得到当前大小。
    if (std::fseek(fp_, 0, SEEK_END) != 0) {
        const int e = errno;
        if (err != nullptr) *err = "fseek failed: " + ErrnoText(e);
        std::fclose(fp_);
        fp_ = nullptr;
        return false;
    }
    long sz = std::ftell(fp_);
    file_size_ = (sz < 0) ? 0 : static_cast<int64_t>(sz);

    RL_INFO("AOF opened: path=%s size=%s policy=%s", path_.c_str(),
            Num(static_cast<long long>(file_size_)).c_str(), AofPolicyName(policy_));
    return true;
}

void Aof::Close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ == nullptr) return;

    if (!buffer_.empty()) {
        size_t n = std::fwrite(buffer_.data(), 1, buffer_.size(), fp_);
        if (n != buffer_.size()) {
            RL_ERROR("AOF final flush short write: %s/%s", Num(n).c_str(),
                     Num(buffer_.size()).c_str());
        } else {
            file_size_ += static_cast<int64_t>(n);
        }
        buffer_.clear();
    }

    std::fflush(fp_);
    FsyncFile(fp_);
    std::fclose(fp_);
    fp_ = nullptr;
    RL_INFO("AOF closed: path=%s size=%s", path_.c_str(),
            Num(static_cast<long long>(file_size_)).c_str());
}

void Aof::Append(const std::vector<std::string>& args) {
    if (args.empty()) return;

    std::string encoded;
    encoded.reserve(64);
    EncodeCommand(args, &encoded);

    bool need_sync = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (fp_ == nullptr) return;

        buffer_ += encoded;
        total_appended_ += encoded.size();
        // 缓冲超过阈值必须同步落盘，否则高频写入会把内存吃光。
        need_sync = buffer_.size() >= flush_threshold_;
    }

    if (policy_ == AofSyncPolicy::kAlways) {
        SyncNow();
    } else if (need_sync) {
        SyncNow();
    }
}

void Aof::AppendArgs(std::initializer_list<std::string> args) {
    std::vector<std::string> v(args);
    Append(v);
}

uint64_t Aof::pending_bytes() {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<uint64_t>(buffer_.size());
}

void Aof::FlushIfNeeded() {
    std::string local;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (fp_ == nullptr || buffer_.empty()) return;
        local.swap(buffer_);
    }

    std::string err;
    if (!WriteToFile(local.data(), local.size(), &err) && !err.empty()) {
        RL_ERROR("AOF write failed: %s", err.c_str());
        return;
    }

    // everysec 语义：批量 write + 一次 fsync。
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ != nullptr && policy_ == AofSyncPolicy::kEverySec) {
        std::fflush(fp_);
        FsyncFile(fp_);
    }
}

void Aof::SyncNow() {
    std::string local;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (fp_ == nullptr) return;
        local.swap(buffer_);
    }

    std::string err;
    bool ok = true;
    if (!local.empty()) ok = WriteToFile(local.data(), local.size(), &err);
    if (!ok && !err.empty()) {
        RL_ERROR("AOF sync write failed: %s", err.c_str());
    }

    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ == nullptr) return;
    // 即使 buffer_ 为空也要 fsync：可能上一轮 write 过但还没落盘。
    std::fflush(fp_);
    FsyncFile(fp_);
}

bool Aof::WriteToFile(const char* data, size_t len, std::string* err) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ == nullptr) {
        if (err != nullptr) *err = "AOF not open";
        return false;
    }

    size_t written = 0;
    while (written < len) {
        size_t n = std::fwrite(data + written, 1, len - written, fp_);
        if (n == 0) {
            if (err != nullptr) *err = "fwrite failed: " + std::string(std::strerror(errno));
            return false;
        }
        written += n;
    }
    file_size_ += static_cast<int64_t>(written);
    return true;
}

// ==================================================================== 恢复
bool Aof::LoadAll(const std::string& path, std::vector<std::vector<std::string>>* out,
                  std::string* err, size_t* truncated_bytes) {
    out->clear();
    if (truncated_bytes != nullptr) *truncated_bytes = 0;

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        // 文件不存在是正常情况（首次启动），不是错误。
        if (errno == ENOENT) {
            RL_INFO("AOF file not found, starting with empty dataset: %s", path.c_str());
            return true;
        }
        if (err != nullptr) {
            *err = "fopen(" + path + ", rb) failed: " + std::strerror(errno);
        }
        return false;
    }

    std::string content;
    char chunk[64 * 1024];
    for (;;) {
        size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
        if (n > 0) content.append(chunk, n);
        if (n < sizeof(chunk)) {
            if (std::ferror(fp)) {
                if (err != nullptr) *err = "fread failed on AOF";
                std::fclose(fp);
                return false;
            }
            break;  // EOF
        }
    }
    std::fclose(fp);

    if (content.empty()) {
        RL_INFO("AOF is empty: %s", path.c_str());
        return true;
    }

    // 复用线上同一个解析器 —— 这是 AOF 用 RESP 编码的最大收益。
    RespParser parser;
    parser.Append(content.data(), content.size());

    size_t consumed = 0;
    for (;;) {
        std::vector<std::string> args;
        const size_t before = parser.buffered();
        bool ok = false;
        try {
            ok = parser.Next(&args);
        } catch (const ProtocolException& e) {
            // 文件尾部损坏：截断到最后一个完整命令，并明确告警。
            // 比直接启动失败更合理 —— 崩溃时刻的最后一条写命令本就是半个。
            if (truncated_bytes != nullptr) *truncated_bytes = parser.buffered();
            RL_WARN("AOF truncated at offset %s: %s (kept %s commands)",
                    Num(consumed).c_str(), e.what(), Num(out->size()).c_str());
            return true;
        }
        if (!ok) break;  // 数据不足（尾部半条）
        const size_t after = parser.buffered();
        consumed += before - after;
        if (!args.empty()) out->push_back(std::move(args));
    }

    if (parser.buffered() > 0) {
        if (truncated_bytes != nullptr) *truncated_bytes = parser.buffered();
        RL_WARN("AOF tail incomplete, discarded %s bytes", Num(parser.buffered()).c_str());
    }

    RL_INFO("AOF loaded: %s -> %s commands", path.c_str(), Num(out->size()).c_str());
    return true;
}

}  // namespace rl
