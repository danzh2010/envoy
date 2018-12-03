#include "common/network/listen_socket_impl.h"

#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include "envoy/common/exception.h"

#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/network/address_impl.h"
#include "common/network/utility.h"

namespace Envoy {
namespace Network {

void ListenSocketImpl::doBind() {
  const Api::SysCallIntResult result = local_address_->bind(*io_handle_);
  if (result.rc_ == -1) {
    close();
    throw EnvoyException(
        fmt::format("cannot bind '{}': {}", local_address_->asString(), strerror(result.errno_)));
  }
  if (local_address_->type() == Address::Type::Ip && local_address_->ip()->port() == 0) {
    // If the port we bind is zero, then the OS will pick a free port for us (assuming there are
    // any), and we need to find out the port number that the OS picked.
    local_address_ = Address::addressFromIoHandle(*io_handle_);
  }
}

void ListenSocketImpl::setListenSocketOptions(const Network::Socket::OptionsSharedPtr& options) {
  if (!Network::Socket::applyOptions(options, *this,
                                     envoy::api::v2::core::SocketOption::STATE_PREBIND)) {
    throw EnvoyException("ListenSocket: Setting socket options failed");
  }
}

TcpListenSocket::TcpListenSocket(const Address::InstanceConstSharedPtr& address,
                                 const Network::Socket::OptionsSharedPtr& options,
                                 bool bind_to_port)
    : ListenSocketImpl(address->socket(Address::SocketType::Stream), address) {
  RELEASE_ASSERT(!isClosed(), "");

  // TODO(htuch): This might benefit from moving to SocketOptionImpl.
  int on = 1;
  Api::SysCallIntResult result =
      io_handle_->setSocketOption(SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  RELEASE_ASSERT(result.rc_ != -1, "");

  setListenSocketOptions(options);

  if (bind_to_port) {
    doBind();
  }
}

TcpListenSocket::TcpListenSocket(IoHandlePtr io_handle,
                                 const Address::InstanceConstSharedPtr& address,
                                 const Network::Socket::OptionsSharedPtr& options)
    : ListenSocketImpl(std::move(io_handle), address) {
  setListenSocketOptions(options);
}

UdsListenSocket::UdsListenSocket(const Address::InstanceConstSharedPtr& address)
    : ListenSocketImpl(address->socket(Address::SocketType::Stream), address) {
  RELEASE_ASSERT(!isClosed(), "");
  doBind();
}

UdsListenSocket::UdsListenSocket(IoHandlePtr io_handle,
                                 const Address::InstanceConstSharedPtr& address)
    : ListenSocketImpl(std::move(io_handle), address) {}

} // namespace Network
} // namespace Envoy
