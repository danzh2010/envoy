#include "source/common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

void EnvoyQuicStreamBase::onHeadersComplete(HeaderMap& headers) {
  return callbacks_->onHeaders(headers);
}

void EnvoyQuicStreamBase::onData(Buffer::Instance& data, bool end_stream) {
  return callbacks_->onData(data, end_stream);
}

void EnvoyQuicStreamBase::onTrailersComplete(HeaderMap& trailers) {
  return callbacks_->onTrailers(trailers);
}


} // namespace Quic
} // namespace Http
} // namespace Envoy
