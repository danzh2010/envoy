#include "envoy/event/deferred_deletable.h"
#include "envoy/http/codec.h"
#include "envoy/network/connection.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/logger.h"
#include "common/http/codec_helper.h"

namespace Envoy {
namespace Http {
namespace Quic {

/**
 * Base class for QUIC client and server codecs.
 */
class ConnectionImplBase : public virtual Connection,
                           protected Logger::Loggable<Logger::Id::quic>,
                           EnvoyQuicConnectionCallback {
public:
  ConnectionImplBase(Network::Connection& connection) : connection_(connection){};

  ~ConnectionImplBase() override;

  // Http::Connection
  void dispatch(Buffer::Instance& data) override {
    // No-op. Currently data should come from EnvoyQuicStreamCallbacks.
  }
  void goAway() override { quic_connection_.sendGoAway(); }
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
  StreamImplPtr createActiveStream(size_t stream_id) override;

  void onConnectionClosed(string reason) override;

private:
  EnvoyQuicConnectionBase& quic_connection_;
};

class ServerConnectionImpl : public ServerConnection, public ConnectionImplBase {
public:
  ServerConnectionImpl(Network::Connection& connection, ServerConnectionCallbacks& callbacks);

protected:
  void processData(Buffer::Instance& data) override;

private:
  ServerConnectionCallbacks& callbacks_;
};

class ClientConnectionImpl : public ClientConnection, public ConnectionImplBase {
public:
  ClientConnectionImpl(Network::Connection& connection, ConnectionCallbacks& callbacks);

  // Http::ClientConnection
  Http::StreamEncoder& newStream(StreamDecoder& response_decoder) override;

private:
  // ConnectionImpl
  ConnectionCallbacks& callbacks() override { return callbacks_; }
  int onBeginHeaders(const nghttp2_frame* frame) override;
  int onHeader(const nghttp2_frame* frame, HeaderString&& name, HeaderString&& value) override;

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
  StreamImple(ConnectionImpl& parent) : parent_(parent) {}

  // Http::StreamEncoder
  void encode100ContinueHeaders(const HeaderMap& headers) override;
  void encodeHeaders(const HeaderMap& headers, bool end_stream) override;
  void encodeData(Buffer::Instance& data, bool end_stream) override;
  void encodeTrailers(const HeaderMap& trailers) override;
  Stream& getStream() override { return *this; }
  void encodeMetadata(const MetadataMap& metadata_map) override;

  // Http::Stream
  void addCallbacks(StreamCallbacks& callbacks) override { addCallbacks_(callbacks); }
  void removeCallbacks(StreamCallbacks& callbacks) override { removeCallbacks_(callbacks); }
  void resetStream(StreamResetReason reason) override;
  virtual void readDisable(bool disable) override;
  virtual uint32_t bufferLimit() override;

  // EnvoyQUicStreamCallbacks
  void onHeaders(HeaderMap& headers);
  void onData(Buffer::Instance& data, bool end_stream);
  void onTrailers(HeaderMap& trailers);

private:
  StreamDecoder& decoder_;
  EnvoyQuicStreamBase& quic_stream_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
