#include "test/integration/http_protocol_integration.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
std::vector<HttpProtocolTestParams> HttpProtocolIntegrationTest::getProtocolTestParams(
    const std::vector<Http::CodecClient::Type>& downstream_protocols,
    const std::vector<FakeHttpConnection::Type>& upstream_protocols) {
  std::vector<HttpProtocolTestParams> ret;

  for (auto ip_version : TestEnvironment::getIpVersionsForTest()) {
    for (auto downstream_protocol : downstream_protocols) {
      for (auto upstream_protocol : upstream_protocols) {
        ret.push_back(HttpProtocolTestParams{ip_version, downstream_protocol, upstream_protocol});
      }
    }
  }
  return ret;
}

std::string HttpProtocolIntegrationTest::protocolTestParamsToString(
    const ::testing::TestParamInfo<HttpProtocolTestParams>& params) {
  std::string downstream_protocol;
  switch (params.param.downstream_protocol) {
  case Http::CodecClient::Type::HTTP1:
    downstream_protocol = "HttpDownstream_";
    break;
  case Http::CodecClient::Type::HTTP2:
    downstream_protocol = "Http2Downstream_";
    break;
  case Http::CodecClient::Type::HTTP3:
    downstream_protocol = "Http3Downstream_";
    break;
  }
  return absl::StrCat((params.param.version == Network::Address::IpVersion::v4 ? "IPv4_" : "IPv6_"),
                      downstream_protocol,
                      (params.param.upstream_protocol == FakeHttpConnection::Type::HTTP2
                           ? "Http2Upstream"
                           : "HttpUpstream"));
}

} // namespace Envoy
