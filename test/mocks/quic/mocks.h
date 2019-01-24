#include "common/http/quic/envoy_quic_connection.h"
#include "common/http/quic/envoy_quic_stream.h"

namespace Envoy {
namespace Http {
namespace Quic {

class MockEnvoyQuicStream : public EnvoyQuicStreamBase {
public:
  MOCK_METHOD2(writeHeaders, void(const HeaderMap& headers, bool end_stream));
  MOCK_METHOD2(writeBody, void(Buffer::Instance& data, bool end_stream));
  MOCK_METHOD1(writeTrailers, void(const HeaderMap& trailers));
};

// Test quic connection which only has one stream.
class MockEnvoyQuicConnection : public EnvoyQuicConnectionBase {
public:
  MOCK_METHOD0(sendGoAway, void());

  // Overridden to create a mock stream.
  EnvoyQuicStreamBase& createNewStream() override {
    quic_stream_.reset(new MockEnvoyQuicStream());
    return *quic_stream_;
  }

private:
  std::unique_ptr<MockEnvoyQuicStream> quic_stream_;
};

} // namespace Quic
} // namespace Http
} // namespace Envoy
