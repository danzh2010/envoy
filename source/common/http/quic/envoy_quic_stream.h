/*
 * Interface to communicate with Envoy about incoming headers, body and
 * trailers.
 */
class EnvoyQuicStreamCallbacks {
public:
  virtual void onHeaders(HeaderMap& headers) PURE;
  virtual void onData(Buffer::Instance& data, bool end_stream) PURE;
  virtual void onTrailers(HeaderMap& trailers) PURE;
};

typedef std::unique_ptr<EnvoyQuicStreamCallbacks> EnvoyQuicStreamCallbacksPtr;

/*
 * Actual quic stream should inherit from this class, or own an instance of this
 * class.
 */
class EnvoyQuicStreamBase {
public:
  virtual void writeHeaders(const HeaderMap& headers, bool end_stream) PURE;

  // Take over all the data.
  virtual void writeBody(Buffer::Instance& data, bool end_stream) PURE;

  virtual void writeTrailers(const HeaderMap& trailers) PURE;

  void setCallback(EnvoyQuicStreamCallbacksPtr callback) {
    callback_ = std::move(callback);
  }

private:
  // To be used when quic stream receives headers, body and trailers.
  EnvoyQuicStreamCallbacksPtr callback_;
};


