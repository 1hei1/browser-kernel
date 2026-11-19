// Copyright 2024 Simprint. All rights reserved.
// EventBus 传输层 - POSIX Unix domain socket 客户端实现
// 与 transport_win.cc 的命名管道行为保持一致：IO 线程上阻塞收发，
// 轮询读取（100ms 超时），消息帧格式由 Message::Encode/TryDecode 决定。

#include "mz/eventbus/transport.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)

#define MZ_LOG(msg) VLOG(1) << "[MZ][Transport] " << msg

namespace mz {
namespace eventbus {

namespace {

constexpr size_t kReadBufferSize = 64 * 1024;  // 64KB
// 和 Windows 的 WaitNamedPipe 一样最多等 5 秒，服务端可能稍晚才开始监听
constexpr base::TimeDelta kConnectTimeout = base::Seconds(5);
constexpr base::TimeDelta kConnectRetryInterval = base::Milliseconds(100);
constexpr int kReadPollTimeoutMs = 100;

}  // namespace

UnixSocketTransport::UnixSocketTransport(const std::string& env_id,
                                         TransportDelegate* delegate)
    : env_id_(env_id),
      delegate_(delegate),
      main_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {
  DCHECK(delegate_);
  DETACH_FROM_SEQUENCE(main_sequence_checker_);

  read_buffer_.reserve(kReadBufferSize);

  io_thread_ = std::make_unique<base::Thread>("EventBusSocketIO");
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  io_thread_->StartWithOptions(std::move(options));
}

UnixSocketTransport::~UnixSocketTransport() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);
  Disconnect();
  io_thread_->Stop();
  CloseSocket();
}

std::string UnixSocketTransport::GetSocketPath() const {
  const char* dir = getenv("MZ_EVENTBUS_DIR");
  std::string base = (dir && *dir) ? std::string(dir) : std::string("/tmp");
  if (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base + "/mz_" + env_id_ + ".sock";
}

void UnixSocketTransport::Connect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);

  if (state_ == TransportState::kConnecting ||
      state_ == TransportState::kConnected) {
    return;
  }

  state_ = TransportState::kConnecting;

  io_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&UnixSocketTransport::DoConnect,
                                base::Unretained(this)));
}

void UnixSocketTransport::DoConnect() {
  const std::string path = GetSocketPath();
  MZ_LOG("DoConnect - socket: " << path);

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    NotifyError("socket path too long: " + path);
    return;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  const base::TimeTicks deadline = base::TimeTicks::Now() + kConnectTimeout;
  int last_error = 0;
  while (true) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      NotifyError(std::string("socket() failed: ") + strerror(errno));
      return;
    }
    if (HANDLE_EINTR(connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr))) == 0) {
      fd_ = fd;
      break;
    }
    last_error = errno;
    close(fd);
    // 服务端还没开始监听：ENOENT / ECONNREFUSED，在超时前重试
    if ((last_error != ENOENT && last_error != ECONNREFUSED) ||
        base::TimeTicks::Now() >= deadline) {
      NotifyError(std::string("connect failed: ") + strerror(last_error));
      return;
    }
    base::PlatformThread::Sleep(kConnectRetryInterval);
  }

  MZ_LOG("Socket connected, calling NotifyConnected");
  NotifyConnected();
  ScheduleRead();
}

void UnixSocketTransport::Disconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);

  if (state_ == TransportState::kDisconnected) {
    return;
  }

  io_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&UnixSocketTransport::DoDisconnect,
                                base::Unretained(this)));
}

void UnixSocketTransport::CloseSocket() {
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    fd_ = -1;
  }
}

void UnixSocketTransport::DoDisconnect() {
  CloseSocket();
  NotifyDisconnected();
}

bool UnixSocketTransport::Send(const Message& message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);

  if (state_ != TransportState::kConnected) {
    MZ_LOG("Send: not connected, state=" << static_cast<int>(state_));
    return false;
  }

  std::vector<uint8_t> data = message.Encode();
  return io_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&UnixSocketTransport::DoSend,
                                base::Unretained(this), std::move(data)));
}

void UnixSocketTransport::DoSend(std::vector<uint8_t> data) {
  if (fd_ < 0) {
    MZ_LOG("DoSend: socket closed, skipping");
    return;
  }
  size_t offset = 0;
  while (offset < data.size()) {
    ssize_t n = HANDLE_EINTR(send(fd_, data.data() + offset,
                                  data.size() - offset, MSG_NOSIGNAL));
    if (n < 0) {
      int error = errno;
      NotifyError(std::string("send failed: ") + strerror(error));
      DoDisconnect();
      return;
    }
    offset += static_cast<size_t>(n);
  }
  MZ_LOG("DoSend completed, sent " << offset << " bytes");
}

void UnixSocketTransport::ScheduleRead() {
  if (fd_ < 0) {
    return;
  }
  io_thread_->task_runner()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&UnixSocketTransport::DoRead, base::Unretained(this)),
      base::Milliseconds(10));
}

void UnixSocketTransport::DoRead() {
  if (fd_ < 0) {
    return;
  }

  pollfd pfd = {};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  int ready = HANDLE_EINTR(poll(&pfd, 1, kReadPollTimeoutMs));
  if (ready < 0) {
    NotifyError(std::string("poll failed: ") + strerror(errno));
    return;
  }
  if (ready == 0) {
    // 超时，让发送任务有机会执行后再读
    ScheduleRead();
    return;
  }

  std::vector<uint8_t> buffer(kReadBufferSize);
  ssize_t bytes_read = HANDLE_EINTR(recv(fd_, buffer.data(), buffer.size(), 0));
  if (bytes_read == 0) {
    // 对端关闭
    CloseSocket();
    NotifyDisconnected();
    return;
  }
  if (bytes_read < 0) {
    int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK) {
      ScheduleRead();
      return;
    }
    CloseSocket();
    NotifyError(std::string("recv failed: ") + strerror(error));
    return;
  }

  read_buffer_.insert(read_buffer_.end(), buffer.begin(),
                      buffer.begin() + bytes_read);

  while (!read_buffer_.empty()) {
    auto decode_result =
        Message::TryDecode(read_buffer_.data(), read_buffer_.size());
    if (!decode_result) {
      break;
    }
    auto& [message, consumed] = *decode_result;
    NotifyMessageReceived(std::move(message));
    read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + consumed);
  }

  ScheduleRead();
}

TransportState UnixSocketTransport::GetState() const {
  return state_;
}

bool UnixSocketTransport::IsConnected() const {
  return state_ == TransportState::kConnected;
}

void UnixSocketTransport::NotifyConnected() {
  main_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](UnixSocketTransport* self) {
                       self->state_ = TransportState::kConnected;
                       if (self->delegate_) {
                         self->delegate_->OnConnected();
                       }
                     },
                     base::Unretained(this)));
}

void UnixSocketTransport::NotifyDisconnected() {
  main_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](UnixSocketTransport* self) {
                       self->state_ = TransportState::kDisconnected;
                       if (self->delegate_) {
                         self->delegate_->OnDisconnected();
                       }
                     },
                     base::Unretained(this)));
}

void UnixSocketTransport::NotifyMessageReceived(Message message) {
  main_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](UnixSocketTransport* self, Message msg) {
                       if (self->delegate_) {
                         self->delegate_->OnMessageReceived(std::move(msg));
                       }
                     },
                     base::Unretained(this), std::move(message)));
}

void UnixSocketTransport::NotifyError(const std::string& error) {
  main_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](UnixSocketTransport* self, std::string err) {
                       self->state_ = TransportState::kError;
                       if (self->delegate_) {
                         self->delegate_->OnError(err);
                       }
                     },
                     base::Unretained(this), error));
}

std::unique_ptr<Transport> CreateTransport(const std::string& env_id,
                                           TransportDelegate* delegate) {
  return std::make_unique<UnixSocketTransport>(env_id, delegate);
}

}  // namespace eventbus
}  // namespace mz

#endif  // BUILDFLAG(IS_POSIX)
