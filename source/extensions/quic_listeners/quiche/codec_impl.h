#pragma once

#include "envoy/http/codec.h"
#include "envoy/registry/registry.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "common/http/http3/quic_codec_factory.h"
#include "common/http/http3/well_known_names.h"

#include "extensions/quic_listeners/quiche/envoy_quic_client_session.h"
#include "extensions/quic_listeners/quiche/envoy_quic_server_session.h"

namespace Envoy {
namespace Quic {

// QuicHttpConnectionImplBase instance is a thin QUIC codec just providing quic interface to HCM.
// Owned by HCM and created during onNewConnection() if the network connection
// is a QUIC connection.
class QuicHttpConnectionImplBase : public virtual Http::Connection,
                                   protected Logger::Loggable<Logger::Id::quic> {
public:
  QuicHttpConnectionImplBase(QuicFilterManagerConnectionImpl& quic_session,
                             const uint32_t max_headers_kb, const uint32_t max_headers_count)
      : quic_session_(quic_session), max_headers_kb_(max_headers_kb),
        max_headers_count_(max_headers_count) {}

  // Http::Connection
  Http::Status dispatch(Buffer::Instance& /*data*/) override {
    // Bypassed. QUIC connection already hands all data to streams.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  Http::Protocol protocol() override { return Http::Protocol::Http3; }
  // Returns true if the session has data to send but queued in connection or
  // stream send buffer.
  bool wantsToWrite() override;

  uint32_t max_headers_kb() const { return max_headers_kb_; }

  uint32_t max_headers_count() const { return max_headers_count_; }

protected:
  QuicFilterManagerConnectionImpl& quic_session_;

private:
  uint32_t max_headers_kb_;
  const uint32_t max_headers_count_;
};

class QuicHttpServerConnectionImpl : public QuicHttpConnectionImplBase,
                                     public Http::ServerConnection {
public:
  QuicHttpServerConnectionImpl(
      EnvoyQuicServerSession& quic_session, Http::ServerConnectionCallbacks& callbacks,
      const uint32_t max_request_headers_kb, const uint32_t max_request_headers_count,
      envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
          headers_with_underscores_action);

  // Http::Connection
  void goAway() override;
  void shutdownNotice() override;
  void onUnderlyingConnectionAboveWriteBufferHighWatermark() override;
  void onUnderlyingConnectionBelowWriteBufferLowWatermark() override;

private:
  EnvoyQuicServerSession& quic_server_session_;
};

class QuicHttpClientConnectionImpl : public QuicHttpConnectionImplBase,
                                     public Http::ClientConnection {
public:
  QuicHttpClientConnectionImpl(EnvoyQuicClientSession& session,
                               Http::ConnectionCallbacks& callbacks,
                               const uint32_t max_request_headers_kb,
                               const uint32_t max_request_headers_count);

  // Http::ClientConnection
  Http::RequestEncoder& newStream(Http::ResponseDecoder& response_decoder) override;

  // Http::Connection
  void goAway() override { NOT_REACHED_GCOVR_EXCL_LINE; }
  void shutdownNotice() override { NOT_REACHED_GCOVR_EXCL_LINE; }
  void onUnderlyingConnectionAboveWriteBufferHighWatermark() override;
  void onUnderlyingConnectionBelowWriteBufferLowWatermark() override;

private:
  EnvoyQuicClientSession& quic_client_session_;
};

// A factory to create QuicHttpClientConnection.
class QuicHttpClientConnectionFactoryImpl : public Http::QuicHttpClientConnectionFactory {
public:
  std::unique_ptr<Http::ClientConnection>
  createQuicClientConnection(Network::Connection& connection, Http::ConnectionCallbacks& callbacks,
                             const uint32_t max_response_headers_kb,
                             const uint32_t max_response_headers_count) override;

  std::string name() const override { return Http::QuicCodecNames::get().Quiche; }
};

// A factory to create QuicHttpServerConnection.
class QuicHttpServerConnectionFactoryImpl : public Http::QuicHttpServerConnectionFactory {
public:
  std::unique_ptr<Http::ServerConnection> createQuicServerConnection(
      Network::Connection& connection, Http::ConnectionCallbacks& callbacks,
      const uint32_t max_request_headers_kb, const uint32_t max_request_headers_count,
      envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
          headers_with_underscores_action) override;

  std::string name() const override { return Http::QuicCodecNames::get().Quiche; }
};

DECLARE_FACTORY(QuicHttpClientConnectionFactoryImpl);
DECLARE_FACTORY(QuicHttpServerConnectionFactoryImpl);

} // namespace Quic
} // namespace Envoy
