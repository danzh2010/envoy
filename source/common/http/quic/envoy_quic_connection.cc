#include "common/http/quic/envoy_quic_connection.h"

#include "common/http/quic/codec_impl.h"

namespace Envoy {
namespace Http {
namespace Quic {

void EnvoyQuicConnectionBase::onNewStream(EnvoyQuicStreamBase& quic_stream) {
  callback_->onNewStream(quic_stream);
}

void EnvoyQuicConnectionBase::onConnectionClosed(std::string reason) {
  callback_->onConnectionClosed(reason);
}

} // namespace Quic
} // namespace Http
} // namespace Envoy
