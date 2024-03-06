#pragma once

#include "log.hpp"
#include <vector>
#include <stdexcept>
#include <functional>
#include <unordered_map>
#include <cstring>
#include <queue>
#include <mutex>
#include <thread>
#include <any>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

Log lg(Onefile);

class Buffer {
private:
    const char* ReadPos() const { return &_buffer[_read_idx]; }
    char* WritePos() { return &_buffer[_write_idx]; }
    std::size_t FrontSize() const { return _read_idx; }
    std::size_t BackSize() const { return _buffer.size() - _write_idx; }

    void MoveReadIdx(std::size_t len) {
        if (_read_idx + len > _write_idx) throw std::out_of_range("move read idx out of range");
        _read_idx += len;
    }
    void MoveWriteIdx(std::size_t len) {
        if (len > BackSize()) throw std::out_of_range("move write idx out of range");
        _write_idx += len;
    }

    void EnsureWritable(std::size_t len) {
        if (len > BackSize()) {
            if (len > WritableSize()) {
                // 如果空间不够，那就直接扩容，不进行移动
                // 尽量少地进行移动是为了减少内存拷贝的次数，提高性能
                _buffer.resize(_buffer.size() + len);
            }
            else {
                // 将数据移动到起始位置
                std::copy(ReadPos(), static_cast<const char*>(WritePos()), &_buffer[0]);
                _write_idx -= FrontSize();
                _read_idx = 0;
            }
        }
    }
    char* FindCRLF() {
        for (auto p = ReadPos(); p < WritePos(); p++) {
            if (*p == '\n') return const_cast<char*>(p);
        }
        return nullptr;
    }
public:
    explicit Buffer(std::size_t size = 1024) : _buffer(size), _read_idx(0), _write_idx(0) {}
    Buffer(const Buffer& buf) : Buffer() { Write(buf); } // 拷贝构造函数（委托构造）

    std::size_t ReadableSize() const { return _write_idx - _read_idx; }
    std::size_t WritableSize() const { return BackSize() + FrontSize(); }

    void Read(void* buf, std::size_t len, bool pop = false) {
        if (len > ReadableSize()) return;
        std::copy(ReadPos(), ReadPos() + len, static_cast<char*>(buf));
        if (pop) MoveReadIdx(len);
    }
    std::string ReadAsString(std::size_t len, bool pop = false) {
        if (len > ReadableSize()) return "";
        std::string str(ReadPos(), len);
        if (pop) MoveReadIdx(len);
        return str;
    }
    std::string ReadLine(bool pop = false) {
        auto p = FindCRLF();
        if (p) return ReadAsString(p - ReadPos() + 1, pop);
        else return "";
    }
    void Write(const void* data, std::size_t len, bool push = true) {
        EnsureWritable(len);
        std::copy(static_cast<const char*>(data), static_cast<const char*>(data) + len, WritePos());
        if (push) MoveWriteIdx(len);
    }
    void Write(const std::string& str, bool push = true) { Write(str.data(), str.size(), push); }
    void Write(const Buffer& buf, bool push = true) { Write(buf.ReadPos(), buf.ReadableSize(), push); }
    void Clear() { _read_idx = _write_idx = 0; }
private:
    std::vector<char> _buffer;
    std::size_t _read_idx;
    std::size_t _write_idx;
};

class Socket {
public:
    Socket() : _sockfd(-1) {}
    explicit Socket(int fd) : _sockfd(fd) {}
    ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // 获取套接字描述符
    int GetFd() const { return _sockfd; }
    // 创建套接字
    bool Create() {
        _sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_sockfd == -1) {
            lg(Error, "create socket failed");
            return false;
        }
        return true;
    }
    // 绑定地址
    bool Bind(const std::string& ip, uint16_t port) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        socklen_t len = sizeof(addr);
        if (bind(_sockfd, reinterpret_cast<struct sockaddr*>(&addr), len) == -1) {
            lg(Error, "bind address %s:%d failed", ip.c_str(), port);
            return false;
        }
        return true;
    }
    // 监听
    bool Listen(int backlog = 1024) {
        if (listen(_sockfd, backlog) == -1) {
            lg(Error, "listen failed");
            return false;
        }
        return true;
    }
    // 客户发起连接
    bool Connect(const std::string& ip, uint16_t port) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        socklen_t len = sizeof(addr);
        if (connect(_sockfd, reinterpret_cast<struct sockaddr*>(&addr), len) == -1) {
            lg(Error, "connect server %s:%d failed", ip.c_str(), port);
            return false;
        }
        return true;
    }
    // 获取新连接
    int Accept() {
        int newfd = accept(_sockfd, nullptr, nullptr);
        if (newfd == -1) lg(Error, "accept failed");
        return newfd;
    }
    // 接收数据
    ssize_t Recv(void* buf, std::size_t len, int flag = 0) {
        ssize_t ret = recv(_sockfd, buf, len, flag);
        if (ret == -1) {
            // EAGAIN: 没有数据可读
            // EINTR:  被信号中断
            if (errno == EAGAIN || errno == EINTR) return 0;
            else lg(Error, "recv failed");
        }
        return ret;
    }
    // 发送数据
    ssize_t Send(const void* buf, std::size_t len, int flag = 0) {
        ssize_t ret = send(_sockfd, buf, len, flag);
        if (ret == -1) {
            // EAGAIN: 没有数据可写
            // EINTR:  被信号中断
            if (errno == EAGAIN || errno == EINTR) return 0;
            else lg(Error, "send failed");
        }
        return ret;
    }
    // 关闭套接字
    void Close() {
        if (_sockfd != -1) {
            close(_sockfd);
            _sockfd = -1;
        }
    }
    // 创建服务端连接
    bool CreateServer(uint16_t port, bool block = true, const std::string& ip = "0.0.0.0", int backlog = 1024) {
        if (!Create()) return false;
        if (!Bind(ip, port)) return false;
        if (!Listen(backlog)) return false;
        if (block == false) NonBlock();
        ReuseAddr();
        return true;
    }
    // 创建客户端连接
    bool CreateClient(uint16_t port, const std::string& ip) {
        if (!Create()) return false;
        if (!Connect(ip, port)) return false;
        NonBlock();
        return true;
    }
    // 设置地址、端口复用
    void ReuseAddr() {
        int opt = 1; // 1: 开启 0: 关闭
        if (setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            lg(Error, "set reuse address failed");
        }
        opt = 1;
        if (setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
            lg(Error, "set reuse port failed");
        }
    }
    // 设置非阻塞
    void NonBlock() {
        // 获取当前标志
        int opt = fcntl(_sockfd, F_GETFL, 0);
        if (opt == -1) {
            lg(Error, "get socket flag failed");
            return;
        }
        // 增加非阻塞标志
        if (fcntl(_sockfd, F_SETFL, opt | O_NONBLOCK) == -1) {
            lg(Error, "set nonblock failed");
        }
    }
private:
    int _sockfd;
};

class EventLoop;
// 事件循环，对fd进行监控
class Channel {
public:
    using callback_t = std::function<void()>;
    Channel(int fd, EventLoop* loop) : _fd(fd), _events(0), _revents(0), _loop(loop) {};
    ~Channel() { close(_fd); }
    void SetReadCallback(callback_t cb) { _read_cb = cb; }
    void SetWriteCallback(callback_t cb) { _write_cb = cb; }
    void SetErrorCallback(callback_t cb) { _error_cb = cb; }
    void SetCloseCallback(callback_t cb) { _close_cb = cb; }
    void SetEventCallback(callback_t cb) { _event_cb = cb; }
    void SetRevents(int revents) { _revents = revents; }

    int GetFd() const { return _fd; }
    int GetEvents() const { return _events; }
    // 是否监控读事件
    bool Readable() const { return _events & EPOLLIN; }
    // 是否监控写事件
    bool Writable() const { return _events & EPOLLOUT; }
    // 启动读事件监控
    void EnableRead() {
        _events |= EPOLLIN;
        Update();
    }
    // 禁用读事件监控
    void DisableRead() {
        _events &= ~EPOLLIN;
        Update();
    }
    // 启动写事件监控
    void EnableWrite() {
        _events |= EPOLLOUT;
        Update();
    }
    // 禁用写事件监控
    void DisableWrite() {
        _events &= ~EPOLLOUT;
        Update();
    }
    // 禁用所有事件监控
    void DisableAll() {
        _events = 0;
        Update();
    }
    // 更新监控
    void Update();
    // 移除监控
    void Remove();
    // 事件处理
    void HandleEvent() {
        // EPOLLIN:  有数据可读
        // EPOLLPRI: 有紧急数据可读
        // EPOLLOUT: 有数据可写
        // EPOLLERR: 错误
        // EPOLLHUP: 对端关闭
        // EPOLLRDHUP: 对端关闭连接
        if (_event_cb) _event_cb(); // 注意次序
        if ((_revents & EPOLLIN) || (_revents & EPOLLPRI) || (_revents & EPOLLRDHUP)) {
            if (_read_cb) _read_cb();
        }

        if (_revents & EPOLLERR) {
            if (_error_cb) _error_cb();
        }
        else if (_revents & EPOLLOUT) {
            if (_write_cb) _write_cb();
        }
        else if (_revents & EPOLLHUP) {
            if (_close_cb) _close_cb(); // 注意其他回调不要close, 否则会重复close
        }
    }
private:
    int _fd;
    int _events; // 需要监控的事件
    int _revents; // 实际触发的事件
    EventLoop* _loop;

    callback_t _read_cb;
    callback_t _write_cb;
    callback_t _error_cb;
    callback_t _close_cb;
    callback_t _event_cb; // 任意事件回调
};

class Poller {
private:
    void EpollOp(int op, Channel* ch) {
        int fd = ch->GetFd();
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = ch->GetEvents();
        int ret = epoll_ctl(_epollfd, op, fd, &ev);
        if (ret == -1) {
            lg(Error, "epoll op failed");
            throw std::runtime_error("epoll op failed");
        }
    }
    bool HasChannel(int fd) const { return _channels.find(fd) != _channels.end(); }
public:
    // 创建epoll
    // EPOLL_CLOEXEC: 进程执行exec时关闭文件描述符
    Poller() : _epollfd(epoll_create1(EPOLL_CLOEXEC)), _events(1024) {
        if (_epollfd == -1) {
            lg(Error, "create epoll failed");
            throw std::runtime_error("create epoll failed");
        }
    }
    ~Poller() { close(_epollfd); }

    void Update(Channel* ch) {
        if (HasChannel(ch->GetFd())) {
            EpollOp(EPOLL_CTL_MOD, ch);
        }
        else {
            EpollOp(EPOLL_CTL_ADD, ch);
            _channels[ch->GetFd()] = ch;
        }
    }

    void Remove(Channel* ch) {
        auto it = _channels.find(ch->GetFd());
        if (it != _channels.end()) {
            EpollOp(EPOLL_CTL_DEL, ch);
            _channels.erase(it);
        }
    }

    // 开始监控
    void Poll(std::vector<Channel*>& active, int timeout = -1) {
        int n = epoll_wait(_epollfd, _events.data(), _events.size(), timeout);
        if (n == -1) {
            if (errno == EINTR) {
                lg(Warning, "epoll wait interrupted");
                return;
            }
            else {
                lg(Error, "epoll wait failed: %s", strerror(errno));
                throw std::runtime_error("epoll wait failed");
            }
        }
        for (int i = 0; i < n; i++) {
            auto ch = _channels.find(_events[i].data.fd);
            if (ch == _channels.end()) {
                lg(Error, "channel not found");
                throw std::runtime_error("channel not found");
            }
            ch->second->SetRevents(_events[i].events);
            active.push_back(ch->second);
        }
    }
private:
    int _epollfd;
    std::vector<struct epoll_event> _events;
    std::unordered_map<int, Channel*> _channels;
};

class TimerTask {
public:
    using TaskFunc = std::function<void()>;
    using ReleaseFunc = std::function<void()>;

    TimerTask(uint64_t id, uint64_t timeout, const TaskFunc& task) : _id(id), _timeout(timeout), _task(task) {}
    void SetRelease(const ReleaseFunc& release) { _release = release; }
    void Cancel() { _canceled = true; }
    uint64_t Timeout() const { return _timeout; }
    ~TimerTask() { if (!_canceled) _task(); _release(); }
private:
    uint64_t _id; // 任务对象的唯一标识
    uint64_t _timeout; // 任务的超时时间
    TaskFunc _task; // 任务的回调函数
    ReleaseFunc _release; // 任务的释放函数(释放TimerWheel中的任务对象)
    bool _canceled = false; // 任务是否被取消
};

class TimerWheel {
private:
    using TaskPtr = std::shared_ptr<TimerTask>;
    using TaskWeakPtr = std::weak_ptr<TimerTask>;

    void Tick() {
        // 时间轮的指针向前移动
        _tick = (_tick + 1) % _wheelSize;
        // 执行当前时间轮上的任务
        _wheel[_tick].clear();
    }
    void OnTime() {
        uint64_t res;
        ssize_t n = read(_timerfd, &res, sizeof(res));
        if (n != sizeof(res)) {
            if (errno == EAGAIN || errno == EINTR) return;
            else {
                lg(Fatal, "read timerfd failed");
                throw std::runtime_error("read timerfd failed");
            }
        }
        Tick();
    }
    void _addTask(uint64_t id, uint64_t timeout, const TimerTask::TaskFunc& task) {
        TaskPtr pt(new TimerTask(id, timeout, task));
        pt->SetRelease([this, id]() {
            _taskMap.erase(id);
            });
        _taskMap[id] = TaskWeakPtr(pt);
        _wheel[(_tick + timeout) % _wheelSize].push_back(pt);
    }
    void _refreshTask(uint64_t id) {
        // 通过id找到任务对象, 构造一个新的任务对象, 添加到时间轮中
        auto it = _taskMap.find(id);
        if (it != _taskMap.end()) {
            TaskPtr pt = it->second.lock();
            if (pt) _wheel[(_tick + pt->Timeout()) % _wheelSize].push_back(pt);
        }
    }
    void _removeTask(uint64_t id) {
        auto it = _taskMap.find(id);
        if (it != _taskMap.end()) {
            TaskPtr pt = it->second.lock();
            if (pt) pt->Cancel();
            _taskMap.erase(id);
        }
    }
public:
    TimerWheel(EventLoop* loop)
        : _tick(0)
        , _wheelSize(60)
        , _timerfd(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC))
        , _loop(loop)
        , _timerch(new Channel(_timerfd, loop)) {
        _wheel.resize(_wheelSize);
        // 创建定时器
        if (_timerfd == -1) {
            lg(Fatal, "create timerfd failed");
            throw std::runtime_error("create timerfd failed");
        }
        struct itimerspec ts;
        ts.it_interval.tv_sec = 1;
        ts.it_interval.tv_nsec = 0;
        ts.it_value.tv_sec = 1;
        ts.it_value.tv_nsec = 0;
        timerfd_settime(_timerfd, 0, &ts, nullptr);
        // 创建定时器事件
        _timerch->SetReadCallback(std::bind(&TimerWheel::OnTime, this));
        _timerch->EnableRead();
    }
    ~TimerWheel() { close(_timerfd); }

    // 添加任务, 需要考虑线程安全
    void AddTask(uint64_t id, uint64_t timeout, const TimerTask::TaskFunc& task);
    // 刷新任务
    void RefreshTask(uint64_t id);
    // 删除任务
    void RemoveTask(uint64_t id);
    // 是否有任务
    bool HasTask(uint64_t id) const { return _taskMap.find(id) != _taskMap.end(); }
private:
    std::vector<std::vector<TaskPtr>> _wheel; // 时间轮
    std::unordered_map<uint64_t, TaskWeakPtr> _taskMap; // 任务对象的映射
    size_t _tick; // 当前时间轮的索引
    size_t _wheelSize; // 时间轮的大小(最大超时时间)
    int _timerfd; // 定时器fd
    EventLoop* _loop; // 事件循环
    std::unique_ptr<Channel> _timerch; // 定时器事件
};

class EventLoop {
public:
    using callback_t = std::function<void()>;

    EventLoop()
        : _eventfd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
        , _tid(std::this_thread::get_id())
        , _eventch(new Channel(_eventfd, this))
        , _timerWheel(this) {
        if (_eventfd == -1) {
            lg(Fatal, "create eventfd failed");
            throw std::runtime_error("create eventfd failed");
        }
        _eventch->SetReadCallback(std::bind(&EventLoop::ReadEventfd, this));
        _eventch->EnableRead();
    }
    ~EventLoop() {
        delete _eventch;
        close(_eventfd);
    }

    // 判断将要执行的任务是否在当前线程中, 是则执行, 否则放入队列中
    void RunInLoop(const callback_t& cb) {
        if (IsInLoopThread()) cb();
        else QueueInLoop(cb);
    }
    // 添加事件监控
    void UpdateEvent(Channel* ch) { _poller.Update(ch); }
    // 移除事件监控
    void RemoveEvent(Channel* ch) { _poller.Remove(ch); }
    // 添加定时任务
    void RunAfter(uint64_t id, uint64_t timeout, const TimerTask::TaskFunc& task) {
        _timerWheel.AddTask(id, timeout, task);
    }
    // 刷新定时任务
    void RefreshAfter(uint64_t id) { _timerWheel.RefreshTask(id); }
    // 删除定时任务
    void RemoveAfter(uint64_t id) { _timerWheel.RemoveTask(id); }
    // 是否有定时任务
    bool HasAfter(uint64_t id) const { return _timerWheel.HasTask(id); }

    void Start() {
        // 事件监控
        std::vector<Channel*> _active;
        _poller.Poll(_active);
        // 事件处理
        for (auto& ch : _active) {
            ch->HandleEvent();
        }
        // 执行任务
        RunPendingTasks();
    }
private:
    void ReadEventfd() {
        uint64_t res;
        ssize_t n = read(_eventfd, &res, sizeof(res));
        if (n != sizeof(res)) {
            if (errno == EAGAIN || errno == EINTR) return;
            else {
                lg(Fatal, "read eventfd failed");
                throw std::runtime_error("read eventfd failed");
            }
        }
    }

    void Wakeup() {
        // 唤醒事件循环
        uint64_t one = 1;
        // 会触发事件循环的读事件
        ssize_t n = write(_eventfd, &one, sizeof(one));
        if (n != sizeof(one)) {
            lg(Error, "write eventfd failed");
        }
    }

    // 判断当前线程是否是事件循环所在的线程
    bool IsInLoopThread() const { return std::this_thread::get_id() == _tid; }
    // 压入任务池
    void QueueInLoop(const callback_t& cb) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pending.push(cb);
        }
        Wakeup();
    }
    // 执行任务
    void RunPendingTasks() {
        std::queue<callback_t> tasks;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            tasks.swap(_pending);
        }
        while (!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }
private:
    int _eventfd;
    std::thread::id _tid; // 保证线程安全
    Channel* _eventch;
    Poller _poller; // 一一对应
    TimerWheel _timerWheel;
    std::queue<callback_t> _pending;
    std::mutex _mutex;
};

void Channel::Update() { _loop->UpdateEvent(this); }
void Channel::Remove() { _loop->RemoveEvent(this); }

void TimerWheel::AddTask(uint64_t id, uint64_t timeout, const TimerTask::TaskFunc& task) {
    _loop->RunInLoop(std::bind(&TimerWheel::_addTask, this, id, timeout, task));
}
void TimerWheel::RefreshTask(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::_refreshTask, this, id));
}
void TimerWheel::RemoveTask(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::_removeTask, this, id));
}

enum class ConnectionState {
    // k通常代表常量或枚举类型
    kDisconnected,
    kConnecting,
    kConnected,
    kDisconnecting,
};
class Connection;
using PtrConnection = std::shared_ptr<Connection>;
class Connection {
public:
private:
    uint64_t _id; // 连接的唯一标识
    int _fd; // 连接的套接字
    ConnectionState _state; // 连接的状态
    Socket _sock; // 套接字
    Channel _channel; // 事件通道
    Buffer _input; // 输入缓冲区
    Buffer _output; // 输出缓冲区
    std::any _context; // 上下文

    using ConnectedCallback = std::function<void(const PtrConnection&)>;
    using MessageCallback = std::function<void(const PtrConnection&, Buffer*)>;
    using CloseCallback = std::function<void(const PtrConnection&)>;
    using EventCallback = std::function<void(const PtrConnection&)>;

    ConnectedCallback _connected_cb;
    MessageCallback _message_cb;
    CloseCallback _close_cb;
    EventCallback _event_cb;
};