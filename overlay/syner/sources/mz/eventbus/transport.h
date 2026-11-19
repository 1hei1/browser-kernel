// Copyright 2024 Simprint. All rights reserved.
// EventBus 传输层 - Named Pipe 客户端

#ifndef MZ_EVENTBUS_TRANSPORT_H_
#define MZ_EVENTBUS_TRANSPORT_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/thread.h"
#include "mz/eventbus/message.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace mz {
namespace eventbus {

// 传输层状态
enum class TransportState {
  kDisconnected,
  kConnecting,
  kConnected,
  kError
};

// 传输层回调接口
class TransportDelegate {
 public:
  virtual ~TransportDelegate() = default;

  // 连接成功
  virtual void OnConnected() = 0;

  // 连接断开
  virtual void OnDisconnected() = 0;

  // 收到消息
  virtual void OnMessageReceived(Message message) = 0;

  // 发生错误
  virtual void OnError(const std::string& error) = 0;
};

// 传输层接口
class Transport {
 public:
  virtual ~Transport() = default;

  // 连接到服务器
  virtual void Connect() = 0;

  // 断开连接
  virtual void Disconnect() = 0;

  // 发送消息
  virtual bool Send(const Message& message) = 0;

  // 获取当前状态
  virtual TransportState GetState() const = 0;

  // 是否已连接
  virtual bool IsConnected() const = 0;
};

#if BUILDFLAG(IS_WIN)

// Windows Named Pipe 传输实现
class NamedPipeTransport : public Transport {
 public:
  // env_id: 环境标识，用于构建管道路径
  // delegate: 回调委托，生命周期需由调用者管理
  NamedPipeTransport(const std::string& env_id, TransportDelegate* delegate);
  ~NamedPipeTransport() override;

  NamedPipeTransport(const NamedPipeTransport&) = delete;
  NamedPipeTransport& operator=(const NamedPipeTransport&) = delete;

  // Transport 实现
  void Connect() override;
  void Disconnect() override;
  bool Send(const Message& message) override;
  TransportState GetState() const override;
  bool IsConnected() const override;

 private:
  // IO 线程上执行的操作
  void DoConnect();
  void DoDisconnect();
  void DoSend(std::vector<uint8_t> data);
  void ScheduleRead();  // 调度异步读取
  void DoRead();        // 执行一次读取

  // 在主线程上回调
  void NotifyConnected();
  void NotifyDisconnected();
  void NotifyMessageReceived(Message message);
  void NotifyError(const std::string& error);

  // 构建管道路径
  std::wstring GetPipePath() const;

  std::string env_id_;
  raw_ptr<TransportDelegate> delegate_;  // 非拥有指针

  HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
  TransportState state_ = TransportState::kDisconnected;

  // 接收缓冲区
  std::vector<uint8_t> read_buffer_;

  // IO 线程
  std::unique_ptr<base::Thread> io_thread_;

  // 用于回调到主线程
  scoped_refptr<base::SequencedTaskRunner> main_task_runner_;

  SEQUENCE_CHECKER(main_sequence_checker_);
};

#elif BUILDFLAG(IS_POSIX)

// POSIX Unix domain socket 传输实现（Linux 无界面服务器用）。
// 套接字路径：$MZ_EVENTBUS_DIR/mz_<env_id>.sock，未设置时为 /tmp/mz_<env_id>.sock，
// 由 mz-server 监听；字节流格式与 Windows 命名管道完全一致。
class UnixSocketTransport : public Transport {
 public:
  UnixSocketTransport(const std::string& env_id, TransportDelegate* delegate);
  ~UnixSocketTransport() override;

  UnixSocketTransport(const UnixSocketTransport&) = delete;
  UnixSocketTransport& operator=(const UnixSocketTransport&) = delete;

  void Connect() override;
  void Disconnect() override;
  bool Send(const Message& message) override;
  TransportState GetState() const override;
  bool IsConnected() const override;

 private:
  void DoConnect();
  void DoDisconnect();
  void DoSend(std::vector<uint8_t> data);
  void ScheduleRead();
  void DoRead();
  void CloseSocket();

  void NotifyConnected();
  void NotifyDisconnected();
  void NotifyMessageReceived(Message message);
  void NotifyError(const std::string& error);

  std::string GetSocketPath() const;

  std::string env_id_;
  raw_ptr<TransportDelegate> delegate_;

  int fd_ = -1;  // 只在 IO 线程上读写
  TransportState state_ = TransportState::kDisconnected;
  std::vector<uint8_t> read_buffer_;
  std::unique_ptr<base::Thread> io_thread_;
  scoped_refptr<base::SequencedTaskRunner> main_task_runner_;

  SEQUENCE_CHECKER(main_sequence_checker_);
};

#endif  // BUILDFLAG(IS_WIN)

// 创建平台相关的传输实现
std::unique_ptr<Transport> CreateTransport(const std::string& env_id,
                                            TransportDelegate* delegate);

}  // namespace eventbus
}  // namespace mz

#endif  // MZ_EVENTBUS_TRANSPORT_H_
