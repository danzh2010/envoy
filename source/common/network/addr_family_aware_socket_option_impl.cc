#include "common/network/addr_family_aware_socket_option_impl.h"

#include "envoy/common/exception.h"

#include "common/api/os_sys_calls_impl.h"
#include "common/common/assert.h"
#include "common/network/address_impl.h"
#include "common/network/socket_option_impl.h"

namespace Envoy {
namespace Network {

bool AddrFamilyAwareSocketOptionImpl::setOption(
    Socket& socket, envoy::api::v2::core::SocketOption::SocketState state) const {
  return setIpSocketOption(socket, state, ipv4_option_, ipv6_option_);
}

bool AddrFamilyAwareSocketOptionImpl::setIpSocketOption(
    Socket& socket, envoy::api::v2::core::SocketOption::SocketState state,
    const std::unique_ptr<SocketOptionImpl>& ipv4_option,
    const std::unique_ptr<SocketOptionImpl>& ipv6_option) {
  std::cerr << "============ setIpSocketOption " << &socket << " localAddress() "
            << socket.localAddress() << "\n";
  // If this isn't IP, we're out of luck.
  Address::InstanceConstSharedPtr address;
  const Address::Ip* ip = nullptr;
  try {
    // We have local address when the socket is used in a listener but have to
    // infer the IP from the socket FD when initiating connections.
    // TODO(htuch): Figure out a way to obtain a consistent interface for IP
    // version from socket.
    if (socket.localAddress()) {
      ip = socket.localAddress()->ip();
    } else {
      std::cerr << "============== addressFromIoHandle() \n";
      address = Address::addressFromIoHandle(socket.ioHandle());
      ip = address->ip();
    }
  } catch (const EnvoyException& ex) {
    // Ignore, we get here because we failed in getsockname().
    // TODO(htuch): We should probably clean up this logic to avoid relying on exceptions.
    ENVOY_LOG(error, "getsockname failed: {}", std::string(ex.what()));
  }
  if (ip == nullptr) {
    ENVOY_LOG(warn, "Failed to set IP socket option on non-IP socket");
    return false;
  }
  ENVOY_LOG(error, "=========== ip != nullptr ");

  // If the FD is v4, we can only try the IPv4 variant.
  if (ip->version() == Network::Address::IpVersion::v4) {
    ENVOY_LOG(error, "=========== Ipv4");
    return ipv4_option->setOption(socket, state);
  }

  // If the FD is v6, we first try the IPv6 variant if the platform supports it and fallback to the
  // IPv4 variant otherwise.
  ASSERT(ip->version() == Network::Address::IpVersion::v6);
  if (ipv6_option->isSupported()) {
    ENVOY_LOG(error, "=========== Ipv6");
    return ipv6_option->setOption(socket, state);
  }
  return ipv4_option->setOption(socket, state);
}

} // namespace Network
} // namespace Envoy
