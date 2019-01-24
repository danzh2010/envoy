#pragma once

#include <string>

#include "common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

class StreamImpl;
typedef std::unique_ptr<StreamImpl> StreamImplPtr;

/*
 * Interface to communicate with Envoy about incoming connection status change.
 */
class EnvoyQuicConnectionCallback {
public:
  virtual ~EnvoyQuicConnectionCallback() {}
  virtual void onConnectionClosed(std::string reason) PURE;
  virtual StreamImplPtr onNewStream(EnvoyQuicStreamBase& quic_stream) PURE;
};

typedef std::unique_ptr<EnvoyQuicConnectionCallback> EnvoyQuicConnectionCallbackPtr;

/*
 * Actual quic connection should inherit from this class, or own an instance of this
 * class.
 */
class EnvoyQuicConnectionBase {
public:
  virtual ~EnvoyQuicConnectionBase() {}

  // Tells quic to send GO_AWAY.
  virtual void sendGoAway() PURE;

  // Create a new outgoing stream.
  virtual EnvoyQuicStreamBase& createNewStream() PURE;

  // Notify Envoy about new incoming stream.
  void onNewStream(EnvoyQuicStreamBase& quic_stream);

  // Notify Envoy about connection close from either quic layer or remote.
  void onConnectionClosed(std::string reason);

  void set_callback(EnvoyQuicConnectionCallback* callback) { callback_ = callback; }

private:
  EnvoyQuicConnectionCallback* callback_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
