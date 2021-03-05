#include "test/integration/protocol_integration_test.h"

namespace Envoy {

// This will run with HTTP/3 downstream, and HTTP/2 upstream.
INSTANTIATE_TEST_SUITE_P(Protocols, DownstreamProtocolIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams(
                             {Http::CodecClient::Type::HTTP3},
                             {FakeHttpConnection::Type::HTTP1, FakeHttpConnection::Type::HTTP2})),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

// This will run with HTTP/1 and HTTP/2 downstream, and HTTP/3 upstream.
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams(
                             {Http::CodecClient::Type::HTTP1, Http::CodecClient::Type::HTTP2},
                             {FakeHttpConnection::Type::HTTP3})),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

} // namespace Envoy
