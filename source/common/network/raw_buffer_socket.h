#pragma once

#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"
#include "envoy/network/transport_socket.h"

#include "source/common/common/logger.h"
#include "source/common/network/transport_socket_options_impl.h"

namespace Envoy {
namespace Network {

class RawBufferSocket : public TransportSocket, protected Logger::Loggable<Logger::Id::connection> {
public:
  // Network::TransportSocket
  void setTransportSocketCallbacks(TransportSocketCallbacks& callbacks) override;
  std::string protocol() const override;
  absl::string_view failureReason() const override;
  bool canFlushClose() override { return true; }
  void closeSocket(Network::ConnectionEvent) override {}
  void onConnected() override;
  IoResult doRead(Buffer::Instance& buffer) override;
  IoResult doWrite(Buffer::Instance& buffer, bool end_stream) override;
  Ssl::ConnectionInfoConstSharedPtr ssl() const override { return nullptr; }
  bool startSecureTransport() override { return false; }
  void configureInitialCongestionWindow(uint64_t, std::chrono::microseconds) override {}

protected:
  TransportSocketCallbacks* transportSocketCallbacks() const { return callbacks_; };

private:
  bool shutdown_{};
  TransportSocketCallbacks* callbacks_{};
};

class RawBufferSocketFactory : public CommonTransportSocketFactory {
public:
  // Network::TransportSocketFactory
  TransportSocketPtr
  createTransportSocket(TransportSocketOptionsConstSharedPtr options) const override;
  bool implementsSecureTransport() const override;
  absl::string_view defaultServerNameIndication() const override { return ""; }
};

} // namespace Network
} // namespace Envoy
