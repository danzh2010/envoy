#include "common/http/quic/codec_impl.h"

#include "common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

void ConnectionImplBase::goAway() override { quic_connection_.sendGoAway(); }

StreamImplPtr ServerConnectionImpl::createActiveStream(size_t stream_id) {
  StreamImplPtr stream(new StreamImpl(*this));
  stream->decoder_ = callbacks_.newStream(*stream);
  return stream;
}

void ConnectionImplBase::onConnectionClosed(string reason) PURE;

void StreamImpl::onHeaders(HeaderMap& headers) {
  // Fake a POST request.
  auto request_headers = absl::make_unique<HeaderMapImpl>({
      {Http::Headers::get().Method, "POST"},
      {Http::Headers::get().Host, ""},
      {Http::Headers::get().Path, "/foo"}});
   decoder_.decodeHeaders(std::move(headers), false);
}false

void StreamImpl::onData(Buffer::Instance& data, bool end_stream) {
  decoder_.decodeData(data, end);

}

void StreamImpl::onTrailers(HeaderMap& trailers) {
  auto request_trailers = absl::make_unique<HeaderMapImpl>({
      {"trailer_key", "trailer_value"}});
  decoder_.decodeTrailers(std::move(trailers));
}

void StreamImpl::encodeHeaders(const HeaderMap& headers, bool end_stream) {
  quic_stream_.writeHeaders(headers, end_stream);
}

void StreamImpl::encodeData(Buffer::Instance& data, bool end_stream) {
  quic_stream_.writeBody(data, end_stream);
}

void StreamImpl::encodeTrailers(const HeaderMap& trailers) {
  quic_stream_.writeTrailers(trailers);
}

} // namespace Quic
} // namespace Http
} // namespace Envoy
