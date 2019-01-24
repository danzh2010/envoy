#include "common/http/quic/codec_impl.h"

#include "common/http/header_map_impl.h"

namespace Envoy {
namespace Http {
namespace Quic {

void ConnectionImplBase::goAway() { quic_connection_->sendGoAway(); }

void ConnectionImplBase::onConnectionClosed(std::string /*reason*/) {
  // To be implemented.
}

StreamImplPtr ServerConnectionImpl::onNewStream(EnvoyQuicStreamBase& quic_stream) {
  StreamImplPtr stream(new StreamImpl(*this, quic_stream));
  stream->setDecoder(&callbacks_.newStream(*stream));
  quic_stream.setCallbacks(std::move(stream));
  return stream;
}

Http::StreamEncoder& ClientConnectionImpl::newStream(StreamDecoder& response_decoder) {
  EnvoyQuicStreamBase& quic_stream = quicConnection()->createNewStream();
  StreamImplPtr stream(new StreamImpl(*this, quic_stream));
  stream->setDecoder(&response_decoder);
  quic_stream.setCallbacks(std::move(stream));
  return *stream;
}

void StreamImpl::onHeaders(HeaderMapPtr&& headers) {
  decoder_->decodeHeaders(std::move(headers), false);
}

void StreamImpl::onData(Buffer::Instance& data, bool end_stream) {
  decoder_->decodeData(data, end_stream);
}

void StreamImpl::onTrailers(HeaderMapPtr&& trailers) {
  decoder_->decodeTrailers(std::move(trailers));
}

void StreamImpl::encodeHeaders(const HeaderMap& headers, bool end_stream) {
  quic_stream_.writeHeaders(headers, end_stream);
}

void StreamImpl::encodeData(Buffer::Instance& data, bool end_stream) {
  quic_stream_.writeBody(data, end_stream);
}

void StreamImpl::encodeTrailers(const HeaderMap& trailers) { quic_stream_.writeTrailers(trailers); }

} // namespace Quic
} // namespace Http
} // namespace Envoy
