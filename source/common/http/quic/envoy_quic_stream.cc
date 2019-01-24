#include "common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

void EnvoyQuicStreamBase::onHeadersComplete(HeaderMapPtr headers) {
  return callbacks_->onHeaders(std::move(headers));
}

void EnvoyQuicStreamBase::onData(Buffer::Instance& data, bool end_stream) {
  return callbacks_->onData(data, end_stream);
}

void EnvoyQuicStreamBase::onTrailersComplete(HeaderMapPtr trailers) {
  return callbacks_->onTrailers(std::move(trailers));
}

} // namespace Quic
} // namespace Http
} // namespace Envoy
