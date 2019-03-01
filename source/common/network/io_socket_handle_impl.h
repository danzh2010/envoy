#pragma once

#include "envoy/api/io_error.h"
#include "envoy/api/os_sys_calls.h"
#include "envoy/network/io_handle.h"

namespace Envoy {
namespace Network {

/**
 * IoHandle derivative for sockets
 */
class IoSocketHandleImpl : public IoHandle {
public:
  explicit IoSocketHandleImpl(int fd = -1) : fd_(fd) {}

  // Close underlying socket if close() hasn't been call yet.
  ~IoSocketHandleImpl() override;

  // TODO(sbelair2)  To be removed when the fd is fully abstracted from clients.
  int fd() const override { return fd_; }

  Api::IoCallUintResult close() override;

  bool isOpen() const override;

  Api::IoCallUintResult readv(uint64_t max_length, Buffer::RawSlice* slices,
                              uint64_t num_slice) override;

  Api::IoCallUintResult writev(const Buffer::RawSlice* slices, uint64_t num_slice) override;

private:
  // Converts a SysCallSizeResult to IoCallUintResult.
  Api::IoCallUintResult sysCallResultToIoCallResult(const Api::SysCallSizeResult& result);

  int fd_;
};

} // namespace Network
} // namespace Envoy
