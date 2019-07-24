#pragma GCC diagnostic push
// QUICHE allows unused parameters.
#pragma GCC diagnostic ignored "-Wunused-parameter"
// QUICHE uses offsetof().
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/test_tools/crypto_test_utils.h"
#include "quiche/quic/test_tools/quic_test_utils.h"

#pragma GCC diagnostic pop

#include <string>

#include "extensions/quic_listeners/quiche/envoy_quic_server_session.h"
#include "extensions/quic_listeners/quiche/codec_impl.h"
#include "extensions/quic_listeners/quiche/envoy_quic_connection_helper.h"
#include "extensions/quic_listeners/quiche/envoy_quic_alarm_factory.h"
#include "extensions/quic_listeners/quiche/envoy_quic_utils.h"
#include "extensions/quic_listeners/quiche/envoy_quic_fake_proof_source.h"

#include "envoy/stats/stats_macros.h"
#include "common/event/libevent_scheduler.h"
#include "server/configuration_impl.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/stream_decoder.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/test_common/global.h"
#include "test/test_common/simulated_time_system.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;

#include <iostream>

namespace Envoy {
namespace Quic {

class TestEnvoyQuicConnection : public EnvoyQuicConnection {
public:
  TestEnvoyQuicConnection(quic::QuicConnectionHelperInterface* helper,
                          quic::QuicAlarmFactory* alarm_factory, quic::QuicPacketWriter* writer,
                          const quic::ParsedQuicVersionVector& supported_versions,
                          Network::ListenerConfig& listener_config, Server::ListenerStats& stats)
      : EnvoyQuicConnection(quic::test::TestConnectionId(),
                            quic::QuicSocketAddress(quic::QuicIpAddress::Any6(), 12345), helper,
                            alarm_factory, writer, /*owns_writer=*/false,
                            quic::Perspective::IS_SERVER, supported_versions, listener_config,
                            stats) {}

  Network::Connection::ConnectionStats& connectionStats() const {
    return EnvoyQuicConnection::connectionStats();
  }

  MOCK_METHOD2(SendConnectionClosePacket, void(quic::QuicErrorCode, const std::string&));
};

class EnvoyQuicServerSessionTest : public ::testing::Test {
public:
  EnvoyQuicServerSessionTest()
      : api_(Api::createApiForTest(time_system_)), dispatcher_(api_->allocateDispatcher()),
        connection_helper_(*dispatcher_),
        alarm_factory_(*dispatcher_, *connection_helper_.GetClock()),
        quic_version_(quic::ParsedVersionOfIndex(quic::CurrentSupportedVersions(), 0)),
        listener_stats_({ALL_LISTENER_STATS(POOL_COUNTER(listener_config_.listenerScope()),
                                            POOL_GAUGE(listener_config_.listenerScope()),
                                            POOL_HISTOGRAM(listener_config_.listenerScope()))}),
        quic_connection_(&connection_helper_, &alarm_factory_, &writer_, quic_version_,
                         listener_config_, listener_stats_),
        crypto_config_(quic::QuicCryptoServerConfig::TESTING, quic::QuicRandom::GetInstance(),
                       std::make_unique<EnvoyQuicFakeProofSource>(),
                       quic::KeyExchangeSource::Default()),
        envoy_quic_session_(quic_config_, quic_version_, &quic_connection_, /*visitor=*/nullptr,
                            &crypto_stream_helper_, &crypto_config_, &compressed_certs_cache_,
                            *dispatcher_),
        read_filter_(new Network::MockReadFilter()) {
    EXPECT_EQ(time_system_.systemTime(), envoy_quic_session_.streamInfo().startTime());
    time_system_.sleep(std::chrono::milliseconds(1));
    ON_CALL(writer_, WritePacket(_, _, _, _, _))
        .WillByDefault(testing::Return(quic::WriteResult(quic::WRITE_STATUS_OK, 1)));
    ON_CALL(crypto_stream_helper_, CanAcceptClientHello(_, _, _, _, _)).WillByDefault(Return(true));
  }

  void SetUp() override { envoy_quic_session_.Initialize(); }

  bool installReadFilter() {
    // Setup read filter.
    envoy_quic_session_.addReadFilter(read_filter_);
    EXPECT_TRUE(read_filter_->callbacks_->connection().isQuic());
    EXPECT_EQ(envoy_quic_session_.id(), read_filter_->callbacks_->connection().id());
    EXPECT_EQ(&envoy_quic_session_, &read_filter_->callbacks_->connection());
    read_filter_->callbacks_->connection().addConnectionCallbacks(network_connection_callbacks_);
    read_filter_->callbacks_->connection().setConnectionStats(
        {read_total_, read_current_, write_total_, write_current_, nullptr, nullptr});
    EXPECT_EQ(&read_total_, &quic_connection_.connectionStats().read_total_);
    EXPECT_CALL(*read_filter_, onNewConnection()).WillOnce(Invoke([this]() {
      // Create ServerConnection instance and setup callbacks for it.
      http_connection_ = std::make_unique<QuicHttpServerConnectionImpl>(envoy_quic_session_,
                                                                        http_connection_callbacks_);
      // Stop iteration to avoid calling getRead/WriteBuffer().
      return Network::FilterStatus::StopIteration;
    }));
    return envoy_quic_session_.initializeReadFilters();
  }

  void TearDown() override {
    if (quic_connection_.connected()) {
      EXPECT_CALL(quic_connection_,
                  SendConnectionClosePacket(quic::QUIC_NO_ERROR, "Closed by application"));
      EXPECT_CALL(network_connection_callbacks_, onEvent(Network::ConnectionEvent::LocalClose));
      envoy_quic_session_.close(Network::ConnectionCloseType::NoFlush);
    }
  }

protected:
  Event::SimulatedTimeSystemHelper time_system_;
  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  EnvoyQuicConnectionHelper connection_helper_;
  EnvoyQuicAlarmFactory alarm_factory_;
  quic::ParsedQuicVersionVector quic_version_;
  testing::NiceMock<quic::test::MockPacketWriter> writer_;
  testing::NiceMock<Network::MockListenerConfig> listener_config_;
  Server::ListenerStats listener_stats_;
  TestEnvoyQuicConnection quic_connection_;
  quic::QuicConfig quic_config_;
  quic::QuicCryptoServerConfig crypto_config_;
  testing::NiceMock<quic::test::MockQuicCryptoServerStreamHelper> crypto_stream_helper_;
  EnvoyQuicServerSession envoy_quic_session_;
  quic::QuicCompressedCertsCache compressed_certs_cache_{100};
  std::shared_ptr<Network::MockReadFilter> read_filter_;
  Network::MockConnectionCallbacks network_connection_callbacks_;
  Http::MockServerConnectionCallbacks http_connection_callbacks_;
  testing::StrictMock<Stats::MockCounter> read_total_;
  testing::StrictMock<Stats::MockGauge> read_current_;
  testing::StrictMock<Stats::MockCounter> write_total_;
  testing::StrictMock<Stats::MockGauge> write_current_;
  Http::ServerConnectionPtr http_connection_;
};

TEST_F(EnvoyQuicServerSessionTest, NewStream) {
  installReadFilter();

  Http::MockStreamDecoder request_decoder;
  EXPECT_CALL(http_connection_callbacks_, newStream(_, false))
      .WillOnce(testing::ReturnRef(request_decoder));
  quic::QuicSpdyStream* stream =
      reinterpret_cast<quic::QuicSpdyStream*>(envoy_quic_session_.GetOrCreateStream(5u));
  // Receive a GET request on created stream.
  quic::QuicHeaderList headers;
  headers.OnHeaderBlockStart();
  std::string host("www.abc.com");
  headers.OnHeader(":authority", host);
  headers.OnHeader(":method", "GET");
  headers.OnHeader(":path", "/");
  headers.OnHeaderBlockEnd(/*uncompressed_header_bytes=*/0, /*compressed_header_bytes=*/0);
  // Request headers should be propogated to decoder.
  EXPECT_CALL(request_decoder, decodeHeaders_(_, /*end_stream=*/true))
      .WillOnce(Invoke([&host](const Http::HeaderMapPtr& decoded_headers, bool) {
        EXPECT_EQ(host, decoded_headers->Host()->value().getStringView());
        EXPECT_EQ("/", decoded_headers->Path()->value().getStringView());
        EXPECT_EQ(Http::Headers::get().MethodValues.Get,
                  decoded_headers->Method()->value().getStringView());
      }));
  EXPECT_CALL(request_decoder, decodeData(_, true))
      .Times(testing::AtMost(1))
      .WillOnce(Invoke([](Buffer::Instance& buffer, bool) { EXPECT_EQ(0, buffer.length()); }));
  stream->OnStreamHeaderList(/*fin=*/true, headers.uncompressed_header_bytes(), headers);
}

TEST_F(EnvoyQuicServerSessionTest, OnResetFrame) {
  installReadFilter();

  Http::MockStreamDecoder request_decoder;
  Http::MockStreamCallbacks stream_callbacks;
  EXPECT_CALL(http_connection_callbacks_, newStream(_, false))
      .WillRepeatedly(Invoke([&request_decoder, &stream_callbacks](Http::StreamEncoder& encoder,
                                                                   bool) -> Http::StreamDecoder& {
        encoder.getStream().addCallbacks(stream_callbacks);
        return request_decoder;
      }));
  quic::QuicStream* stream1 = envoy_quic_session_.GetOrCreateStream(5u);
  quic::QuicRstStreamFrame rst1(/*control_frame_id=*/1u, stream1->id(),
                                quic::QUIC_ERROR_PROCESSING_STREAM, /*bytes_written=*/0u);
  EXPECT_CALL(stream_callbacks, onResetStream(Http::StreamResetReason::RemoteReset, _));
  stream1->OnStreamReset(rst1);

  quic::QuicStream* stream2 = envoy_quic_session_.GetOrCreateStream(7u);
  quic::QuicRstStreamFrame rst2(/*control_frame_id=*/1u, stream2->id(), quic::QUIC_REFUSED_STREAM,
                                /*bytes_written=*/0u);
  EXPECT_CALL(stream_callbacks,
              onResetStream(Http::StreamResetReason::RemoteRefusedStreamReset, _));
  stream2->OnStreamReset(rst2);

  quic::QuicStream* stream3 = envoy_quic_session_.GetOrCreateStream(9u);
  quic::QuicRstStreamFrame rst3(/*control_frame_id=*/1u, stream3->id(),
                                quic::QUIC_STREAM_CONNECTION_ERROR, /*bytes_written=*/0u);
  EXPECT_CALL(stream_callbacks, onResetStream(Http::StreamResetReason::ConnectionFailure, _));
  stream3->OnStreamReset(rst3);
}

TEST_F(EnvoyQuicServerSessionTest, ConnectionClose) {
  installReadFilter();

  std::string error_details("dummy details");
  quic::QuicErrorCode error(quic::QUIC_INVALID_FRAME_DATA);
  quic::QuicConnectionCloseFrame frame(error, error_details);
  EXPECT_CALL(network_connection_callbacks_, onEvent(Network::ConnectionEvent::RemoteClose));
  quic_connection_.OnConnectionCloseFrame(frame);
  EXPECT_EQ(absl::StrCat(quic::QuicErrorCodeToString(error), " with details: ", error_details),
            envoy_quic_session_.transportFailureReason());
  EXPECT_EQ(Network::Connection::State::Closed, envoy_quic_session_.state());
}

TEST_F(EnvoyQuicServerSessionTest, ConnectionCloseWithActiveStream) {
  installReadFilter();

  Http::MockStreamDecoder request_decoder;
  Http::MockStreamCallbacks stream_callbacks;
  EXPECT_CALL(http_connection_callbacks_, newStream(_, false))
      .WillOnce(Invoke([&request_decoder, &stream_callbacks](Http::StreamEncoder& encoder,
                                                             bool) -> Http::StreamDecoder& {
        encoder.getStream().addCallbacks(stream_callbacks);
        return request_decoder;
      }));
  quic::QuicStream* stream = envoy_quic_session_.GetOrCreateStream(5u);
  EXPECT_CALL(quic_connection_,
              SendConnectionClosePacket(quic::QUIC_NO_ERROR, "Closed by application"));
  EXPECT_CALL(network_connection_callbacks_, onEvent(Network::ConnectionEvent::LocalClose));
  EXPECT_CALL(stream_callbacks, onResetStream(Http::StreamResetReason::ConnectionTermination, _));
  envoy_quic_session_.close(Network::ConnectionCloseType::NoFlush);
  EXPECT_EQ(Network::Connection::State::Closed, envoy_quic_session_.state());
  EXPECT_TRUE(stream->write_side_closed() && stream->reading_stopped());
}

TEST_F(EnvoyQuicServerSessionTest, InitializeFilterChain) {
  // Generate a CHLO packet.
  quic::CryptoHandshakeMessage chlo = quic::test::crypto_test_utils::GenerateDefaultInchoateCHLO(
      connection_helper_.GetClock(), quic::CurrentSupportedVersions()[0].transport_version,
      &crypto_config_);
  chlo.SetVector(quic::kCOPT, quic::QuicTagVector{quic::kREJ});
  std::string packet_content(chlo.GetSerialized().AsStringPiece());
  auto encrypted_packet =
      std::unique_ptr<quic::QuicEncryptedPacket>(quic::test::ConstructEncryptedPacket(
          quic_connection_.connection_id(), quic::EmptyQuicConnectionId(), /*version_flag=*/true,
          /*reset_flag*/ false, /*packet_number=*/1, packet_content));

  quic::QuicSocketAddress self_address(quic::QuicIpAddress::Loopback4(), 12344);
  quic::QuicSocketAddress peer_address(quic::QuicIpAddress::Loopback4(), 12345);
  auto packet = std::unique_ptr<quic::QuicReceivedPacket>(
      quic::test::ConstructReceivedPacket(*encrypted_packet, connection_helper_.GetClock()->Now()));

  // Receiving above packet should trigger filter chain retrival.
  Network::MockFilterChainManager filter_chain_manager;
  EXPECT_CALL(listener_config_, filterChainManager()).WillOnce(ReturnRef(filter_chain_manager));
  Network::MockFilterChain filter_chain;
  EXPECT_CALL(filter_chain_manager, findFilterChain(_)).WillOnce(Return(&filter_chain));
  std::vector<Network::FilterFactoryCb> filter_factory{[this](
                                                           Network::FilterManager& filter_manager) {
    filter_manager.addReadFilter(read_filter_);
    read_filter_->callbacks_->connection().addConnectionCallbacks(network_connection_callbacks_);
  }};
  EXPECT_CALL(filter_chain, networkFilterFactories()).WillOnce(ReturnRef(filter_factory));
  EXPECT_CALL(*read_filter_, onNewConnection())
      // Stop iteration to avoid calling getRead/WriteBuffer().
      .WillOnce(Return(Network::FilterStatus::StopIteration));
  EXPECT_CALL(listener_config_.filter_chain_factory_, createNetworkFilterChain(_, _))
      .WillOnce(Invoke([](Network::Connection& connection,
                          const std::vector<Network::FilterFactoryCb>& filter_factories) {
        EXPECT_EQ(1u, filter_factories.size());
        Server::Configuration::FilterChainUtility::buildFilterChain(connection, filter_factories);
        return true;
      }));
  // A reject should be sent because of the inchoate CHLO.
  EXPECT_CALL(writer_, WritePacket(_, _, _, _, _))
      .WillOnce(testing::Return(quic::WriteResult(quic::WRITE_STATUS_OK, 1)));
  quic_connection_.ProcessUdpPacket(self_address, peer_address, *packet);
  EXPECT_TRUE(quic_connection_.connected());
  EXPECT_FALSE(envoy_quic_session_.IsEncryptionEstablished());
}

} // namespace Quic
} // namespace Envoy
