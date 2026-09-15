// Copyright (c) 2025 redis-lite authors. MIT License.
//
// poller.cpp — epoll(ET) 与 WSAPoll 双后端实现。

#include "rl/poller.h"

#include <algorithm>
#include <cstring>

#include "rl/log.h"

namespace rl {

#if RL_LINUX

// =============================================================== Linux: epoll
Poller::Poller() = default;

Poller::~Poller() {
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
}

bool Poller::Init(std::string* err) {
    // EPOLL_CLOEXEC：fork 出的子进程不会继承，避免 fd 泄漏。
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        if (err != nullptr) *err = "epoll_create1 failed: " + LastSocketError();
        return false;
    }
    return true;
}

bool Poller::SupportsEdgeTrigger() { return true; }

bool Poller::Register(Socket fd, uint32_t events, std::string* err) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    // EPOLLET 是本项目性能设计的核心：只在状态变化时通知一次，
    // 因此业务层必须循环读到 EAGAIN。见 event_loop.cpp / connection.cpp。
    ev.events = events | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (err != nullptr) *err = "epoll_ctl(ADD) failed: " + LastSocketError();
        return false;
    }
    return true;
}

bool Poller::Modify(Socket fd, uint32_t events, std::string* err) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
        if (err != nullptr) *err = "epoll_ctl(MOD) failed: " + LastSocketError();
        return false;
    }
    return true;
}

bool Poller::Unregister(Socket fd, std::string* err) {
    // DEL 时 epoll_event 参数在 2.6.9+ 可为 nullptr。
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
        if (err != nullptr) *err = "epoll_ctl(DEL) failed: " + LastSocketError();
        return false;
    }
    return true;
}

int Poller::Wait(std::vector<ReadyEvent>* out, int timeout_ms, std::string* err) {
    constexpr int kMaxEvents = 1024;
    epoll_event events[kMaxEvents];

    int n;
    for (;;) {
        n = epoll_wait(epfd_, events, kMaxEvents, timeout_ms);
        if (n >= 0) break;
        if (errno == EINTR) continue;  // 被信号打断，重试
        if (err != nullptr) *err = "epoll_wait failed: " + LastSocketError();
        return -1;
    }

    out->clear();
    out->reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        ReadyEvent re;
        re.fd = events[i].data.fd;
        uint32_t e = kEventNone;
        if (events[i].events & (EPOLLIN | EPOLLPRI)) e |= kEventReadable;
        if (events[i].events & EPOLLOUT) e |= kEventWritable;
        if (events[i].events & EPOLLERR) e |= kEventError;
        // EPOLLRDHUP: 对端半关闭（写了 FIN），这比 EPOLLHUP 更早，能及时感知。
        if (events[i].events & (EPOLLHUP | EPOLLRDHUP)) e |= kEventHup;
        re.events = e;
        out->push_back(re);
    }
    return n;
}

#else  // ==================================================== Windows: select()

Poller::Poller() = default;
Poller::~Poller() = default;

bool Poller::Init(std::string* /*err*/) { return true; }

// select 只有电平触发语义。这一点必须诚实暴露给上层：
// 上层据此决定读路径的形态（本项目仍然循环读到 EAGAIN，对两种语义都正确）。
bool Poller::SupportsEdgeTrigger() { return false; }

bool Poller::Register(Socket fd, uint32_t events, std::string* err) {
    if (entries_.size() >= kMaxSelectFds) {
        // select 的 fd 数量上限是编译期常量，无法动态扩容。
        // 与其静默行为异常，不如在注册点就明确报错。
        if (err != nullptr) {
            *err = "select backend: fd limit reached (" + Num(kMaxSelectFds) +
                   "). Rebuild with a larger FD_SETSIZE or use the Linux/epoll build.";
        }
        return false;
    }

    for (Entry& e : entries_) {
        if (e.fd == fd) {
            e.events = events;
            return true;
        }
    }
    entries_.push_back(Entry{fd, events});
    return true;
}

bool Poller::Modify(Socket fd, uint32_t events, std::string* err) {
    return Register(fd, events, err);
}

bool Poller::Unregister(Socket fd, std::string* /*err*/) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].fd == fd) {
            entries_[i] = entries_.back();
            entries_.pop_back();
            return true;
        }
    }
    return true;
}

int Poller::Wait(std::vector<ReadyEvent>* out, int timeout_ms, std::string* err) {
    out->clear();

    if (entries_.empty()) {
        // 没有关注的 fd：退化为 sleep，避免忙等烧 CPU。
        if (timeout_ms > 0) Sleep(static_cast<DWORD>(timeout_ms));
        return 0;
    }

    fd_set read_set, write_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    Socket max_fd = 0;
    for (const Entry& e : entries_) {
        if (e.events & kEventReadable) FD_SET(e.fd, &read_set);
        if (e.events & kEventWritable) FD_SET(e.fd, &write_set);
        // select 的 except_set 用于带外数据，这里借它统一探测错误。
        FD_SET(e.fd, &except_set);
        if (e.fd > max_fd) max_fd = e.fd;
    }

    timeval tv;
    timeval* tvp = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int n = select(static_cast<int>(max_fd) + 1, &read_set, &write_set, &except_set, tvp);
    if (n == SOCKET_ERROR) {
        if (err != nullptr) *err = "select failed: " + LastSocketError();
        return -1;
    }
    if (n == 0) return 0;

    for (const Entry& e : entries_) {
        uint32_t events = kEventNone;
        if (e.events & kEventReadable) {
            if (FD_ISSET(e.fd, &read_set)) events |= kEventReadable;
        }
        if (e.events & kEventWritable) {
            if (FD_ISSET(e.fd, &write_set)) events |= kEventWritable;
        }
        // except_set 命中意味着 socket 出错或收到带外数据 —— 两者都应触发关闭。
        if (FD_ISSET(e.fd, &except_set)) events |= kEventError;

        if (events == kEventNone) continue;

        ReadyEvent re;
        re.fd = e.fd;
        re.events = events;
        out->push_back(re);
    }

    // select 在 Windows 上可能只报告 except 而不报告 read（对端 RST 场景），
    // 上面已把 except 映射成 kEventError，上层会走关闭路径。
    return static_cast<int>(out->size());
}

#endif

}  // namespace rl
