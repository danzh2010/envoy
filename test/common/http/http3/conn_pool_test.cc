#include <chrono>
#include <memory>

#include "source/common/http/http3/conn_pool.h"
#include "source/common/quic/quic_transport_socket_factory.h"

#include "test/common/http/common.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/server/transport_socket_factory_context.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/test_common/simulated_time_system.h"

using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Http {
namespace Http3 {

class Http3ConnPoolImplPeer {
public:
  static std::list<Envoy::ConnectionPool::ActiveClientPtr>&
  connectingClients(Http3ConnPoolImpl& pool) {
    return pool.connecting_clients_;
  }
  static quic::QuicServerId& getServerId(Http3ConnPoolImpl& pool) { return pool.server_id_; }
};

class MockPoolConnectResultCallback : public PoolConnectResultCallback {
public:
  MOCK_METHOD(void, onHandshakeComplete, ());
};

class Http3ConnPoolImplTest : public Event::TestUsingSimulatedTime, public testing::Test {
public:
  Http3ConnPoolImplTest() {
    EXPECT_CALL(context_.context_manager_, createSslClientContext(_, _, _))
        .WillRepeatedly(Return(ssl_context_));
    factory_.emplace(std::unique_ptr<Envoy::Ssl::ClientContextConfig>(
                         new NiceMock<Ssl::MockClientContextConfig>),
                     context_);
    factory_->initialize();
  }

  void initialize() {
    EXPECT_CALL(mockHost(), address()).WillRepeatedly(Return(test_address_));
    EXPECT_CALL(mockHost(), transportSocketFactory()).WillRepeatedly(testing::ReturnRef(*factory_));
    EXPECT_CALL(mockHost().cluster_, connectTimeout())
        .WillRepeatedly(Return(std::chrono::milliseconds(10000)));

    new Event::MockSchedulableCallback(&dispatcher_);
    Network::ConnectionSocket::OptionsSharedPtr options;
    Network::TransportSocketOptionsConstSharedPtr transport_options;
    pool_ = allocateConnPool(dispatcher_, random_, host_, Upstream::ResourcePriority::Default,
                             options, transport_options, state_, quic_stat_names_, {}, store_,
                             makeOptRef<PoolConnectResultCallback>(connect_result_callback_),
                             quic_info_);
    EXPECT_EQ(3000, Http3ConnPoolImplPeer::getServerId(*pool_).port());
  }

  Upstream::MockHost& mockHost() { return static_cast<Upstream::MockHost&>(*host_); }

  NiceMock<Event::MockDispatcher> dispatcher_;
  Quic::PersistentQuicInfoImpl quic_info_{dispatcher_, 45};
  Upstream::HostSharedPtr host_{new NiceMock<Upstream::MockHost>};
  NiceMock<Random::MockRandomGenerator> random_;
  Upstream::ClusterConnectivityState state_;
  Network::Address::InstanceConstSharedPtr test_address_ =
      Network::Utility::resolveUrl("tcp://127.0.0.1:3000");
  NiceMock<Server::Configuration::MockTransportSocketFactoryContext> context_;
  absl::optional<Quic::QuicClientTransportSocketFactory> factory_;
  Ssl::ClientContextSharedPtr ssl_context_{new Ssl::MockClientContext()};
  Stats::IsolatedStoreImpl store_;
  Quic::QuicStatNames quic_stat_names_{store_.symbolTable()};
  std::unique_ptr<Http3ConnPoolImpl> pool_;
  MockPoolConnectResultCallback connect_result_callback_;
};

class MockQuicClientTransportSocketFactory : public Quic::QuicClientTransportSocketFactory {
public:
  MockQuicClientTransportSocketFactory(
      Ssl::ClientContextConfigPtr config,
      Server::Configuration::TransportSocketFactoryContext& factory_context)
      : Quic::QuicClientTransportSocketFactory(move(config), factory_context) {}

  MOCK_METHOD(Envoy::Ssl::ClientContextSharedPtr, sslCtx, ());
};

TEST_F(Http3ConnPoolImplTest, FastFailWithoutSecretsLoaded) {
  MockQuicClientTransportSocketFactory factory{
      std::unique_ptr<Envoy::Ssl::ClientContextConfig>(new NiceMock<Ssl::MockClientContextConfig>),
      context_};

  EXPECT_CALL(factory, sslCtx()).WillRepeatedly(Return(nullptr));

  EXPECT_CALL(mockHost(), address()).WillRepeatedly(Return(test_address_));
  EXPECT_CALL(mockHost(), transportSocketFactory()).WillRepeatedly(testing::ReturnRef(factory));
  // The unique pointer of this object will be returned in createSchedulableCallback_ of
  // dispatcher_, so there is no risk of object leak.
  new Event::MockSchedulableCallback(&dispatcher_);
  Network::ConnectionSocket::OptionsSharedPtr options;
  Network::TransportSocketOptionsConstSharedPtr transport_options;
  ConnectionPool::InstancePtr pool =
      allocateConnPool(dispatcher_, random_, host_, Upstream::ResourcePriority::Default, options,
                       transport_options, state_, quic_stat_names_, {}, store_,
                       makeOptRef<PoolConnectResultCallback>(connect_result_callback_), quic_info_);

  EXPECT_EQ(static_cast<Http3ConnPoolImpl*>(pool.get())->instantiateActiveClient(), nullptr);
}

TEST_F(Http3ConnPoolImplTest, FailWithSecretsBecomeEmpty) {
  MockQuicClientTransportSocketFactory factory{
      std::unique_ptr<Envoy::Ssl::ClientContextConfig>(new NiceMock<Ssl::MockClientContextConfig>),
      context_};

  Ssl::ClientContextSharedPtr ssl_context(new Ssl::MockClientContext());
  EXPECT_CALL(factory, sslCtx())
      .WillOnce(Return(ssl_context))
      .WillOnce(Return(nullptr))
      .WillRepeatedly(Return(ssl_context));

  EXPECT_CALL(mockHost(), address()).WillRepeatedly(Return(test_address_));
  EXPECT_CALL(mockHost(), transportSocketFactory()).WillRepeatedly(testing::ReturnRef(factory));
  new Event::MockSchedulableCallback(&dispatcher_);
  Network::ConnectionSocket::OptionsSharedPtr options;
  Network::TransportSocketOptionsConstSharedPtr transport_options;
  ConnectionPool::InstancePtr pool =
      allocateConnPool(dispatcher_, random_, host_, Upstream::ResourcePriority::Default, options,
                       transport_options, state_, quic_stat_names_, {}, store_,
                       makeOptRef<PoolConnectResultCallback>(connect_result_callback_), quic_info_);

  MockResponseDecoder decoder;
  ConnPoolCallbacks callbacks;
  EXPECT_CALL(callbacks.pool_failure_, ready());
  EXPECT_EQ(pool->newStream(decoder, callbacks), nullptr);
}

TEST_F(Http3ConnPoolImplTest, CreationAndNewStream) {
  initialize();

  MockResponseDecoder decoder;
  ConnPoolCallbacks callbacks;

  ConnectionPool::Cancellable* cancellable = pool_->newStream(decoder, callbacks);
  EXPECT_NE(nullptr, cancellable);
  std::list<Envoy::ConnectionPool::ActiveClientPtr>& clients =
      Http3ConnPoolImplPeer::connectingClients(*pool_);
  EXPECT_EQ(1u, clients.size());
  EXPECT_CALL(connect_result_callback_, onHandshakeComplete()).WillOnce(Invoke([cancellable]() {
    cancellable->cancel(Envoy::ConnectionPool::CancelPolicy::Default);
  }));
  pool_->onConnectionEvent(*clients.front(), "", Network::ConnectionEvent::Connected);
}

} // namespace Http3
} // namespace Http
} // namespace Envoy
