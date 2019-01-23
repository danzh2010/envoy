#include "source/common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

/*
 * Interface to communicate with Envoy about incoming connection status change.
 */
class EnvoyQuicConnectionCallback {
public:
  virtual void onConnectionClosed(string reason) PURE;
};

class EnvoyQuicServerConnectionCallback : public EnvoyQuicConnectionCallback {
public:
  virtual StreamImplPtr onNewStream(EnvoyQuicStreamBase& quic_stream) PURE;
};

typedef std::unique_ptr<EnvoyQuicConnectionCallback> EnvoyQuicConnectionCallbackPtr;

/*
 * Actual quic connection should inherit from this class, or own an instance of this
 * class.
 */
class EnvoyQuicConnectionBase {
public:
  // Tells quic to send GO_AWAY.
  virtual void sendGoAway() PURE;

  // Create a new outgoing stream.
  virtual EnvoyQuicStreamBase& createNewStream() PURE;

  // Notify Envoy about new incoming stream.
  void onNewStream(EnvoyQuicStreamBase& quic_stream);

  // Notify Envoy about connection close from either quic layer or remote.
  void onConnectionClosed(string reason);

  void set_callback(EnvoyQuicConnectionCallbackPtr callback) { callback_ = callback; }

private:
  EnvoyQuicConnectionCallbackPtr callback_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
