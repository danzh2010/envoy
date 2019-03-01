#include "common/network/io_socket_handle_impl.h"

#include <errno.h>

#include <iostream>

#include "envoy/buffer/buffer.h"

#include "common/api/os_sys_calls_impl.h"
#include "common/common/stack_array.h"
#include "common/network/io_socket_error_impl.h"

using Envoy::Api::SysCallIntResult;
using Envoy::Api::SysCallSizeResult;

namespace Envoy {
namespace Network {

IoSocketHandleImpl::~IoSocketHandleImpl() {
  if (fd_ != -1) {
    IoSocketHandleImpl::close();
  }
}

Api::IoCallUintResult IoSocketHandleImpl::close() {
  ASSERT(fd_ != -1);
  const int rc = ::close(fd_);
  fd_ = -1;
  return Api::IoCallResult<uint64_t>(rc, Api::IoErrorPtr(nullptr, deleteIoError));
}

bool IoSocketHandleImpl::isOpen() const { return fd_ != -1; }

Api::IoCallUintResult IoSocketHandleImpl::readv(uint64_t max_length, Buffer::RawSlice* slices,
                                                uint64_t num_slice) {
  STACK_ARRAY(iov, iovec, num_slice);
  uint64_t num_slices_to_read = 0;
  uint64_t num_bytes_to_read = 0;
  for (; num_slices_to_read < num_slice && num_bytes_to_read < max_length; num_slices_to_read++) {
    iov[num_slices_to_read].iov_base = slices[num_slices_to_read].mem_;
    const size_t slice_length = std::min(slices[num_slices_to_read].len_,
                                         static_cast<size_t>(max_length - num_bytes_to_read));
    iov[num_slices_to_read].iov_len = slice_length;
    num_bytes_to_read += slice_length;
  }
  ASSERT(num_bytes_to_read <= max_length);
  auto& os_syscalls = Api::OsSysCallsSingleton::get();
  const Api::SysCallSizeResult result =
      os_syscalls.readv(fd_, iov.begin(), static_cast<int>(num_slices_to_read));
  return sysCallResultToIoCallResult(result);
}

Api::IoCallUintResult IoSocketHandleImpl::writev(const Buffer::RawSlice* slices,
                                                 uint64_t num_slice) {
  STACK_ARRAY(iov, iovec, num_slice);
  uint64_t num_slices_to_write = 0;
  for (uint64_t i = 0; i < num_slice; i++) {
    if (slices[i].mem_ != nullptr && slices[i].len_ != 0) {
      iov[num_slices_to_write].iov_base = slices[i].mem_;
      iov[num_slices_to_write].iov_len = slices[i].len_;
      num_slices_to_write++;
    }
  }
  if (num_slices_to_write == 0) {
    return Api::IoCallUintResult(0, Api::IoErrorPtr(nullptr, deleteIoError));
  }
  auto& os_syscalls = Api::OsSysCallsSingleton::get();
  const Api::SysCallSizeResult result = os_syscalls.writev(fd_, iov.begin(), num_slices_to_write);
  return sysCallResultToIoCallResult(result);
}

Api::IoCallUintResult
IoSocketHandleImpl::sysCallResultToIoCallResult(const Api::SysCallSizeResult& result) {
  if (result.rc_ >= 0) {
    // Return nullptr as IoError upon success.
    return Api::IoCallUintResult(result.rc_, Api::IoErrorPtr(nullptr, deleteIoError));
  }
  return Api::IoCallUintResult(
      /*rc=*/0, (result.errno_ == EAGAIN
                     // EAGAIN is frequent enough that its memory allocation should be avoided.
                     ? Api::IoErrorPtr(getIoSocketEagainInstance(), deleteIoError)
                     : Api::IoErrorPtr(new IoSocketError(result.errno_), deleteIoError)));
}

} // namespace Network
} // namespace Envoy
