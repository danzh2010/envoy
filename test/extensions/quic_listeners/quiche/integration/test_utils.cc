#include "test/extensions/quic_listeners/quiche/integration/test_utils.h"

#include "common/api/api_impl.h"

#include "extensions/quic_listeners/quiche/envoy_quic_client_connection.h"
#include "extensions/quic_listeners/quiche/envoy_quic_client_session.h"
#include "extensions/quic_listeners/quiche/envoy_quic_proof_verifier.h"
#include "extensions/transport_sockets/tls/context_config_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/test_common/environment.h"
#include "test/test_common/utility.h"

namespace Envoy {
namespace Quic {

std::unique_ptr<Quic::QuicClientTransportSocketFactory>
TestUtils::createQuicClientTransportSocketFactory(const Ssl::ClientSslTransportOptions& options,
                                                  Api::Api& api, const std::string& san_to_match) {
  std::string yaml_plain = R"EOF(
  common_tls_context:
    validation_context:
      trusted_ca:
        filename: "{{ test_rundir }}/test/config/integration/certs/cacert.pem"
)EOF";
  envoy::extensions::transport_sockets::tls::v3::UpstreamTlsContext tls_context;
  TestUtility::loadFromYaml(TestEnvironment::substitute(yaml_plain), tls_context);
  auto* common_context = tls_context.mutable_common_tls_context();

  if (options.alpn_) {
    common_context->add_alpn_protocols("h3");
  }
  if (options.san_) {
    common_context->mutable_validation_context()->add_match_subject_alt_names()->set_exact(
        san_to_match);
  }
  for (const std::string& cipher_suite : options.cipher_suites_) {
    common_context->mutable_tls_params()->add_cipher_suites(cipher_suite);
  }
  if (!options.sni_.empty()) {
    tls_context.set_sni(options.sni_);
  }

  common_context->mutable_tls_params()->set_tls_minimum_protocol_version(options.tls_version_);
  common_context->mutable_tls_params()->set_tls_maximum_protocol_version(options.tls_version_);

  testing::NiceMock<Server::Configuration::MockTransportSocketFactoryContext> mock_factory_ctx;
  ON_CALL(mock_factory_ctx, api()).WillByDefault(testing::ReturnRef(api));
  auto cfg = std::make_unique<Extensions::TransportSockets::Tls::ClientContextConfigImpl>(
      tls_context, options.sigalgs_, mock_factory_ctx);
  return std::make_unique<Quic::QuicClientTransportSocketFactory>(std::move(cfg));
}

std::unique_ptr<quic::QuicCryptoClientConfig>
TestUtils::createQuicCryptoClientConfig(Stats::Scope& scope, Api::Api& api,
                                        const std::string& san_to_match, TimeSource& time_source) {
  return std::make_unique<quic::QuicCryptoClientConfig>(
      std::make_unique<Quic::EnvoyQuicProofVerifier>(
          scope,
          TestUtils::createQuicClientTransportSocketFactory(
              Ssl::ClientSslTransportOptions().setAlpn(true).setSan(true), api, san_to_match)
              ->clientContextConfig(),
          time_source));
}

Network::ClientConnectionPtr TestUtils::makeClientConnection(
    const quic::ParsedQuicVersionVector& supported_versions, Event::Dispatcher& dispatcher,
    Network::Address::InstanceConstSharedPtr& server_addr,
    Network::Address::InstanceConstSharedPtr& client_addr, quic::QuicConnectionId conn_id,
    Quic::EnvoyQuicConnectionHelper& conn_helper, Quic::EnvoyQuicAlarmFactory& alarm_factory,
    quic::QuicConfig& quic_config, quic::QuicServerId server_id,
    quic::QuicCryptoClientConfig& crypto_config,
    quic::QuicClientPushPromiseIndex& push_promise_index) {
  // TODO(danzh) Implement retry upon version mismatch and modify test frame work to specify a
  // different version set on server side to test that.
  auto connection = std::make_unique<Quic::EnvoyQuicClientConnection>(
      conn_id, server_addr, conn_helper, alarm_factory, supported_versions, client_addr, dispatcher,
      nullptr);
  auto session = std::make_unique<Quic::EnvoyQuicClientSession>(
      quic_config, supported_versions, std::move(connection), server_id, &crypto_config,
      &push_promise_index, dispatcher, 0);
  session->Initialize();
  return session;
}

BufferingStreamDecoderPtr
TestUtils::makeSingleHttp3Request(uint32_t port, const std::string& method, const std::string& url,
                                  const std::string& body, Network::Address::IpVersion ip_version,
                                  const std::string& host, const std::string& content_type) {
  auto addr = Network::Utility::resolveUrl(
      fmt::format("udp://{}:{}", Network::Test::getLoopbackAddressUrlString(ip_version), port));
  return makeSingleHttp3Request(addr, method, url, body, host, content_type);
}

BufferingStreamDecoderPtr
TestUtils::makeSingleHttp3Request(Network::Address::InstanceConstSharedPtr& addr,
                                  const std::string& method, const std::string& url,
                                  const std::string& body, const std::string& host,
                                  const std::string& content_type) {
  NiceMock<Stats::MockIsolatedStatsStore> mock_stats_store;
  NiceMock<Random::MockRandomGenerator> random;
  Event::GlobalTimeSystem time_system;
  NiceMock<Random::MockRandomGenerator> random_generator;
  Api::Impl api(Thread::threadFactoryForTest(), mock_stats_store, time_system,
                Filesystem::fileSystemForTest(), random_generator);
  Event::DispatcherPtr dispatcher(api.allocateDispatcher("test_thread"));
  std::shared_ptr<Upstream::MockClusterInfo> cluster{new NiceMock<Upstream::MockClusterInfo>()};
  Upstream::HostDescriptionConstSharedPtr host_description{
      Upstream::makeTestHostDescription(cluster, "udp://127.0.0.1:80", time_system)};

  quic::QuicConfig quic_config;
  quic::QuicServerId server_id{"lyft.com", 443, false};
  std::string quic_client_san_to_match{"spiffe://lyft.com/backend-team"};
  quic::QuicClientPushPromiseIndex push_promise_index;
  Quic::EnvoyQuicConnectionHelper conn_helper(*dispatcher);
  Quic::EnvoyQuicAlarmFactory alarm_factory(*dispatcher, *conn_helper.GetClock());

  auto crypto_config = TestUtils::createQuicCryptoClientConfig(
      mock_stats_store, api, quic_client_san_to_match, time_system);
  auto client_addr = Network::Address::InstanceConstSharedPtr();
  Network::ClientConnectionPtr envoy_connection =
      makeClientConnection(quic::CurrentSupportedVersions(), *dispatcher, addr, client_addr,
                           quic::QuicUtils::CreateRandomConnectionId(), conn_helper, alarm_factory,
                           quic_config, server_id, *crypto_config, push_promise_index);
  Network::ClientConnection* connection = envoy_connection.get();

  Http::CodecClientProd client(Http::CodecClient::Type::HTTP3, std::move(envoy_connection),
                               host_description, *dispatcher, random);

  struct ConnectionCallbacks : public Network::ConnectionCallbacks {
    ConnectionCallbacks(Event::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
      if (event == Network::ConnectionEvent::Connected) {
        connected_ = true;
        dispatcher_.exit();
      } else if (event == Network::ConnectionEvent::RemoteClose) {
        dispatcher_.exit();
      } else {
        if (!connected_) {
          // Before handshake gets established, any connection failure should exit the loop. I.e. a
          // QUIC connection may fail of INVALID_VERSION if both this client doesn't support any of
          // the versions the server advertised before handshake established. In this case the
          // connection is closed locally and this is in a blocking event loop.
          dispatcher_.exit();
        }
      }
    }

    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    Event::Dispatcher& dispatcher_;
    bool connected_{false};
  };
  ConnectionCallbacks connection_callbacks(*dispatcher);
  connection->addConnectionCallbacks(connection_callbacks);
  // setCodecConnectionCallbacks(codec_callbacks_);
  dispatcher->run(Event::Dispatcher::RunType::Block);

  BufferingStreamDecoderPtr response(
      new BufferingStreamDecoder([&]() -> void { dispatcher->exit(); }));
  Http::RequestEncoder& encoder = client.newStream(*response);
  encoder.getStream().addCallbacks(*response);

  Http::TestRequestHeaderMapImpl headers;
  headers.setMethod(method);
  headers.setPath(url);
  headers.setHost(host);
  headers.setReferenceScheme(Http::Headers::get().SchemeValues.Http);
  if (!content_type.empty()) {
    headers.setContentType(content_type);
  }
  EXPECT_TRUE(encoder.encodeHeaders(headers, body.empty()).ok());
  if (!body.empty()) {
    Buffer::OwnedImpl body_buffer(body);
    encoder.encodeData(body_buffer, true);
  }

  dispatcher->run(Event::Dispatcher::RunType::Block);
  client.close();
  return response;
}

} // namespace Quic
} // namespace Envoy
