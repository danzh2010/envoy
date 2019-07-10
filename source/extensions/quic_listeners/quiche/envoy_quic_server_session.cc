#include "extensions/quic_listeners/quiche/envoy_quic_server_session.h"

#pragma GCC diagnostic push
// QUICHE allows unused parameters.
#pragma GCC diagnostic ignored "-Wunused-parameter"
// QUICHE uses offsetof().
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "quiche/quic/core/quic_crypto_server_stream.h"
#pragma GCC diagnostic pop

#include "extensions/quic_listeners/quiche/envoy_quic_server_stream.h"

namespace Envoy {
namespace Quic {
EnvoyQuicServerSession::EnvoyQuicServerSession(
    const quic::QuicConfig& config, const quic::ParsedQuicVersionVector& supported_versions,
    quic::QuicConnection* connection, quic::QuicSession::Visitor* visitor,
    quic::QuicCryptoServerStream::Helper* helper, const quic::QuicCryptoServerConfig* crypto_config,
    quic::QuicCompressedCertsCache* compressed_certs_cache,
    Event::Dispatcher& dispatcher)
    : quic::QuicServerSessionBase(config, supported_versions, connection, visitor, helper,
                                  crypto_config, compressed_certs_cache),
      filter_manager_(*this), dispatcher_(dispatcher), stream_info_(dispatcher.timeSource()) {
      }

quic::QuicCryptoServerStreamBase* EnvoyQuicServerSession::CreateQuicCryptoServerStream(
    const quic::QuicCryptoServerConfig* crypto_config,
    quic::QuicCompressedCertsCache* compressed_certs_cache) {
  return new quic::QuicCryptoServerStream(crypto_config, compressed_certs_cache, this,
                                          stream_helper());
}

quic::QuicSpdyStream* EnvoyQuicServerSession::CreateIncomingStream(quic::QuicStreamId id) {
  if (!ShouldCreateIncomingStream(id)) {
    return nullptr;
  }
  EnvoyQuicServerStream* stream = new EnvoyQuicServerStream(id, this, quic::BIDIRECTIONAL);
  ActivateStream(absl::WrapUnique(stream));
  setUpRequestDecoder(*stream);
  return stream;
}

quic::QuicSpdyStream* EnvoyQuicServerSession::CreateIncomingStream(quic::PendingStream* pending) {
  EnvoyQuicServerStream* stream = new EnvoyQuicServerStream(pending, this, quic::BIDIRECTIONAL);
  ActivateStream(absl::WrapUnique(stream));
  setUpRequestDecoder(*stream);
  return stream;
}

quic::QuicSpdyStream* EnvoyQuicServerSession::CreateOutgoingBidirectionalStream() {
  // Not allow server initiated stream.
  NOT_REACHED_GCOVR_EXCL_LINE;
}

quic::QuicSpdyStream* EnvoyQuicServerSession::CreateOutgoingUnidirectionalStream() {
  NOT_REACHED_GCOVR_EXCL_LINE;
}

void EnvoyQuicServerSession::setUpRequestDecoder(EnvoyQuicStream& stream) {
  ASSERT(http_connection_callbacks_ != nullptr);
  Http::StreamDecoder& decoder = http_connection_callbacks_->newStream(stream);
  stream.setDecoder(decoder);
}

void EnvoyQuicServerSession::OnConnectionClosed(const quic::QuicConnectionCloseFrame& frame,
                                                quic::ConnectionCloseSource source) {
  std::cerr << "HasActiveStreams " << HasActiveRequestStreams() << "\n";
  quic::QuicServerSessionBase::OnConnectionClosed(frame, source);
   std::cerr << "HasActiveStreams " << HasActiveRequestStreams() << "\n";

  for (auto callback : network_connection_callbacks_) {
    callback->onEvent(source == quic::ConnectionCloseSource::FROM_PEER
                                            ? Network::ConnectionEvent::RemoteClose
                                            : Network::ConnectionEvent::LocalClose);
  }
  transport_failure_reason_ = absl::StrCat(quic::QuicErrorCodeToString(frame.quic_error_code), " with details: ", frame.error_details);
}

void EnvoyQuicServerSession::addWriteFilter(Network::WriteFilterSharedPtr filter) {
  filter_manager_.addWriteFilter(filter);
}

void EnvoyQuicServerSession::addFilter(Network::FilterSharedPtr filter) { filter_manager_.addFilter(filter); }

void EnvoyQuicServerSession::addReadFilter(Network::ReadFilterSharedPtr filter) {
  filter_manager_.addReadFilter(filter);
}

bool EnvoyQuicServerSession::initializeReadFilters() {
  return filter_manager_.initializeReadFilters();
}

void EnvoyQuicServerSession::addConnectionCallbacks(Network::ConnectionCallbacks& cb) {
  network_connection_callbacks_.push_back(&cb);
}

void EnvoyQuicServerSession::close(Network::ConnectionCloseType type) {
  // TODO(danzh): Implement FlushWrite and FlushWriteAndDelay mode.
  ASSERT(type == Network::ConnectionCloseType::NoFlush);
  connection()->CloseConnection(quic::QUIC_NO_ERROR, "Closed by application", quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
}
void EnvoyQuicServerSession::setDelayedCloseTimeout(std::chrono::milliseconds /*timeout*/) {
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}

std::chrono::milliseconds EnvoyQuicServerSession::delayedCloseTimeout() const {
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}

absl::string_view EnvoyQuicServerSession::requestedServerName() const {
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}

 const Network::Address::InstanceConstSharedPtr& EnvoyQuicServerSession::remoteAddress() const {
   NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
 }

  const Network::Address::InstanceConstSharedPtr& EnvoyQuicServerSession::localAddress() const {
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }

 const Ssl::ConnectionInfo* EnvoyQuicServerSession::ssl() const {
   // TODO(danzh): construct Ssl::ConnectionInfo from crypto stream
   NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
 }

void EnvoyQuicServerSession::rawWrite(Buffer::Instance& /*data*/, bool /*end_stream*/) {
  // TODO(danzh): maybe send via MessageFrame?
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}


} // namespace Quic
} // namespace Envoy
