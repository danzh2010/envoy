#pragma once

#include "envoy/buffer/buffer.h"
#include "envoy/http/header_map.h"

namespace Envoy {
namespace Http {
namespace Quic {

/*
 * Interface to communicate with Envoy about incoming headers, body and
 * trailers.
 */
class EnvoyQuicStreamCallbacks {
public:
  virtual ~EnvoyQuicStreamCallbacks() {}

  virtual void onHeaders(HeaderMapPtr&& headers) PURE;
  virtual void onData(Buffer::Instance& data, bool end_stream) PURE;
  virtual void onTrailers(HeaderMapPtr&& trailers) PURE;
};

typedef std::unique_ptr<EnvoyQuicStreamCallbacks> EnvoyQuicStreamCallbacksPtr;

/*
 * Actual quic stream should inherit from this class, or own an instance of this
 * class.
 */
class EnvoyQuicStreamBase {
public:
  virtual ~EnvoyQuicStreamBase() {}

  virtual void writeHeaders(const HeaderMap& headers, bool end_stream) PURE;

  // Take over all the data.
  virtual void writeBody(Buffer::Instance& data, bool end_stream) PURE;
  virtual void writeTrailers(const HeaderMap& trailers) PURE;

  // Notify Envoy through callback.
  void onHeadersComplete(HeaderMapPtr headers);
  void onData(Buffer::Instance& data, bool end_stream);
  void onTrailersComplete(HeaderMapPtr trailers);

  void setCallbacks(EnvoyQuicStreamCallbacksPtr callbacks) { callbacks_ = std::move(callbacks); }

private:
  // To be used when quic stream receives headers, body and trailers.
  EnvoyQuicStreamCallbacksPtr callbacks_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
