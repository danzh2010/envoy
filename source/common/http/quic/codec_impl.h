#pragma once

#include <string>

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/network/connection.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/logger.h"
#include "common/http/codec_helper.h"
#include "common/http/quic/envoy_quic_connection.h"
#include "common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

class EnvoyQuicConnectionBase;
class EnvoyQuicStreamBase;

/**
 * Base class for QUIC client and server codecs.
 */
class ConnectionImplBase : public virtual Connection,
                           protected Logger::Loggable<Logger::Id::quic>,
                           public EnvoyQuicConnectionCallback {
public:
  ConnectionImplBase() : quic_connection_(nullptr){};

  void setQuicConnection(EnvoyQuicConnectionBase* connection) { quic_connection_ = connection; }

  ~ConnectionImplBase() override {}

  // Implements Http::Connection.
  void dispatch(Buffer::Instance& /*data*/) override {
    // No-op. Currently data should come from EnvoyQuicStreamCallbacks.
  }
  void goAway() override;
  Protocol protocol() override { return Protocol::Quic; }
  void shutdownNotice() override {
    // To be implemented.
  }
  bool wantsToWrite() override {
    // TODO(danzh): return based on quic stack flow control and congestion
    // control.
    return true;
  }
  // Propagate network connection watermark events to each stream on the connection.
  void onUnderlyingConnectionAboveWriteBufferHighWatermark() override {
    // To be implemented.
  }
  void onUnderlyingConnectionBelowWriteBufferLowWatermark() override {
    // To be implemented.
  }

  // EnvoyQuicConnectionCallback
  void onConnectionClosed(std::string reason) override;

protected:
  EnvoyQuicConnectionBase* quicConnection() { return quic_connection_; }

private:
  EnvoyQuicConnectionBase* quic_connection_;
};

class StreamImpl;
typedef std::unique_ptr<StreamImpl> StreamImplPtr;

class ServerConnectionImpl : public ServerConnection, public ConnectionImplBase {
public:
  ServerConnectionImpl(Network::Connection& connection, ServerConnectionCallbacks& callbacks);

  // Implements EnvoyQuicConnectionCallback.
  StreamImplPtr onNewStream(EnvoyQuicStreamBase& quic_stream) override;

private:
  Http::ServerConnectionCallbacks& callbacks_;
};

class ClientConnectionImpl : public ClientConnection, public ConnectionImplBase {
public:
  ClientConnectionImpl(Network::Connection& connection, ConnectionCallbacks& callbacks);

  // Implements EnvoyQuicConnectionCallback.
  StreamImplPtr onNewStream(EnvoyQuicStreamBase& /*quic_stream*/) override {
    // Client does not support incoming stream.
    ASSERT(false);
  }

  // Implements Http::ClientConnection
  Http::StreamEncoder& newStream(StreamDecoder& response_decoder) override;

private:
  Http::ConnectionCallbacks& callbacks_;
};

/**
 * Base class for client and server side streams.
 */
class StreamImpl : public Stream,
                   public StreamEncoder,
                   public Event::DeferredDeletable,
                   public StreamCallbackHelper,
                   public EnvoyQuicStreamCallbacks {
public:
  StreamImpl(ConnectionImplBase& parent, EnvoyQuicStreamBase& quic_stream)
      : parent_(parent), decoder_(nullptr), quic_stream_(quic_stream) {}

  // Http::StreamEncoder
  void encode100ContinueHeaders(const HeaderMap& headers) override;
  void encodeHeaders(const HeaderMap& headers, bool end_stream) override;
  void encodeData(Buffer::Instance& data, bool end_stream) override;
  void encodeTrailers(const HeaderMap& trailers) override;
  Stream& getStream() override { return *this; }
  void encodeMetadata(const MetadataMapVector& metadata_map_vector) override;

  // Http::Stream
  void addCallbacks(StreamCallbacks& callbacks) override { addCallbacks_(callbacks); }
  void removeCallbacks(StreamCallbacks& callbacks) override { removeCallbacks_(callbacks); }
  void resetStream(StreamResetReason reason) override;
  virtual void readDisable(bool disable) override;
  virtual uint32_t bufferLimit() override;

  // EnvoyQUicStreamCallbacks
  void onHeaders(HeaderMapPtr&& headers);
  void onData(Buffer::Instance& data, bool end_stream);
  void onTrailers(HeaderMapPtr&& trailers);

  void setDecoder(StreamDecoder* decoder) { decoder_ = decoder; }

private:
  ConnectionImplBase& parent_;
  StreamDecoder* decoder_;
  EnvoyQuicStreamBase& quic_stream_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
