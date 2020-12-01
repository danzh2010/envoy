#include "envoy/event/dispatcher.h"
#include "envoy/network/address.h"

#include "extensions/quic_listeners/quiche/envoy_quic_alarm_factory.h"
#include "extensions/quic_listeners/quiche/envoy_quic_connection_helper.h"
#include "extensions/quic_listeners/quiche/quic_transport_socket_factory.h"

#include "test/integration/ssl_utility.h"
#include "test/integration/utility.h"
#include "test/mocks/server/transport_socket_factory_context.h"
#include "test/test_common/network_utility.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#include "quiche/quic/core/http/quic_client_push_promise_index.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "quiche/quic/core/quic_utils.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Envoy {
namespace Quic {

class TestUtils {
public:
  static std::unique_ptr<Quic::QuicClientTransportSocketFactory>
  createQuicClientTransportSocketFactory(const Ssl::ClientSslTransportOptions& options,
                                         Api::Api& api, const std::string& san_to_match);

  static std::unique_ptr<quic::QuicCryptoClientConfig>
  createQuicCryptoClientConfig(Stats::Scope& scope, Api::Api& api, const std::string& san_to_match,
                               TimeSource& time_source);

  // Initiate a QUIC connection with the highest supported version. If not
  // supported by server, this connection will fail.
  static Network::ClientConnectionPtr
  makeClientConnection(const quic::ParsedQuicVersionVector& supported_versions,
                       Event::Dispatcher& dispatcher,
                       Network::Address::InstanceConstSharedPtr& server_addr,
                       Network::Address::InstanceConstSharedPtr& client_addr,
                       quic::QuicConnectionId conn_id, Quic::EnvoyQuicConnectionHelper& conn_helper,
                       Quic::EnvoyQuicAlarmFactory& alarm_factory, quic::QuicConfig& quic_config,
                       quic::QuicServerId server_id, quic::QuicCryptoClientConfig& crypto_config,
                       quic::QuicClientPushPromiseIndex& push_promise_index);

  static BufferingStreamDecoderPtr
  makeSingleHttp3Request(uint32_t port, const std::string& method, const std::string& url,
                         const std::string& body, Network::Address::IpVersion ip_version,
                         const std::string& host = "host", const std::string& content_type = "");

  static BufferingStreamDecoderPtr
  makeSingleHttp3Request(Network::Address::InstanceConstSharedPtr& addr, const std::string& method,
                         const std::string& url, const std::string& body, const std::string& host,
                         const std::string& content_type);
};

} // namespace Quic
} // namespace Envoy
