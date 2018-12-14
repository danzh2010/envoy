
/*
 * Used to notify Envoy quic's status change.
 */
class EnvoyQuicConnectionCallback {
public:
  virtual StreamImplPtr createActiveStream(size_t stream_id) PURE;

  virtual void onConnectionClosed(string reason) PURE;
};

typedef std::unique_ptr<EnvoyQuicConnectionCallback> EnvoyQuicConnectionCallbackPtr;

class EnvoyQuicConnectionBase {
public:
  virtual void sendGoAway() PURE;

  void set_callback(EnvoyQuicConnectionCallbackPtr callback) {
    callback_ = callback;
  }

private:
  EnvoyQuicConnectionCallbackPtr callback_;
};

