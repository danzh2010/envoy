#include "test/integration/fake_upstream.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "common/buffer/buffer_impl.h"
#include "common/config/utility.h"
#include "common/http/header_map_impl.h"
#include "common/http/http1/codec_impl.h"
#include "common/http/http2/codec_impl.h"
#include "common/http/http3/well_known_names.h"
#include "common/network/address_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/socket_option_factory.h"
#include "common/network/utility.h"

#include "server/connection_handler_impl.h"

#include "test/test_common/network_utility.h"
#include "test/test_common/utility.h"

#include "absl/strings/str_cat.h"
#include "absl/synchronization/notification.h"

using namespace std::chrono_literals;

using std::chrono::milliseconds;
using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;

namespace Envoy {

FakeStream::FakeStream(FakeHttpConnection& parent, Http::ResponseEncoder& encoder,
                       Event::TestTimeSystem& time_system)
    : parent_(parent), encoder_(encoder), time_system_(time_system) {
  encoder.getStream().addCallbacks(*this);
}

void FakeStream::decodeHeaders(Http::RequestHeaderMapPtr&& headers, bool end_stream) {
  absl::MutexLock lock(&lock_);
  headers_ = std::move(headers);
  setEndStream(end_stream);
}

void FakeStream::decodeData(Buffer::Instance& data, bool end_stream) {
  received_data_ = true;
  absl::MutexLock lock(&lock_);
  body_.add(data);
  setEndStream(end_stream);
}

void FakeStream::decodeTrailers(Http::RequestTrailerMapPtr&& trailers) {
  absl::MutexLock lock(&lock_);
  setEndStream(true);
  trailers_ = std::move(trailers);
}

void FakeStream::decodeMetadata(Http::MetadataMapPtr&& metadata_map_ptr) {
  for (const auto& metadata : *metadata_map_ptr) {
    duplicated_metadata_key_count_[metadata.first]++;
    metadata_map_.insert(metadata);
  }
}

void FakeStream::postToConnectionThread(std::function<void()> cb) {
  parent_.connection().dispatcher().post(cb);
}

void FakeStream::encode100ContinueHeaders(const Http::ResponseHeaderMap& headers) {
  std::shared_ptr<Http::ResponseHeaderMap> headers_copy(
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers));
  parent_.connection().dispatcher().post([this, headers_copy]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.encode100ContinueHeaders(*headers_copy);
  });
}

void FakeStream::encodeHeaders(const Http::HeaderMap& headers, bool end_stream) {
  std::shared_ptr<Http::ResponseHeaderMap> headers_copy(
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers));
  if (add_served_by_header_) {
    headers_copy->addCopy(Http::LowerCaseString("x-served-by"),
                          parent_.connection().addressProvider().localAddress()->asString());
  }

  parent_.connection().dispatcher().post([this, headers_copy, end_stream]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.encodeHeaders(*headers_copy, end_stream);
  });
}

void FakeStream::encodeData(absl::string_view data, bool end_stream) {
  parent_.connection().dispatcher().post([this, data, end_stream]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    Buffer::OwnedImpl fake_data(data.data(), data.size());
    encoder_.encodeData(fake_data, end_stream);
  });
}

void FakeStream::encodeData(uint64_t size, bool end_stream) {
  parent_.connection().dispatcher().post([this, size, end_stream]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    Buffer::OwnedImpl data(std::string(size, 'a'));
    encoder_.encodeData(data, end_stream);
  });
}

void FakeStream::encodeData(Buffer::Instance& data, bool end_stream) {
  std::shared_ptr<Buffer::Instance> data_copy = std::make_shared<Buffer::OwnedImpl>(data);
  parent_.connection().dispatcher().post([this, data_copy, end_stream]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.encodeData(*data_copy, end_stream);
  });
}

void FakeStream::encodeTrailers(const Http::HeaderMap& trailers) {
  std::shared_ptr<Http::ResponseTrailerMap> trailers_copy(
      Http::createHeaderMap<Http::ResponseTrailerMapImpl>(trailers));
  parent_.connection().dispatcher().post([this, trailers_copy]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.encodeTrailers(*trailers_copy);
  });
}

void FakeStream::encodeResetStream() {
  parent_.connection().dispatcher().post([this]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.getStream().resetStream(Http::StreamResetReason::LocalReset);
  });
}

void FakeStream::encodeMetadata(const Http::MetadataMapVector& metadata_map_vector) {
  parent_.connection().dispatcher().post([this, &metadata_map_vector]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.encodeMetadata(metadata_map_vector);
  });
}

void FakeStream::readDisable(bool disable) {
  parent_.connection().dispatcher().post([this, disable]() -> void {
    {
      absl::MutexLock lock(&lock_);
      if (saw_reset_) {
        // Encoded already deleted.
        return;
      }
    }
    encoder_.getStream().readDisable(disable);
  });
}

void FakeStream::onResetStream(Http::StreamResetReason, absl::string_view) {
  absl::MutexLock lock(&lock_);
  saw_reset_ = true;
}

AssertionResult FakeStream::waitForHeadersComplete(milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  const auto reached = [this]()
                           ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return headers_ != nullptr; };
  if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
    return AssertionFailure() << "Timed out waiting for headers.";
  }
  return AssertionSuccess();
}

namespace {
// Perform a wait on a condition while still allowing for periodic client dispatcher runs that
// occur on the current thread.
bool waitForWithDispatcherRun(Event::TestTimeSystem& time_system, absl::Mutex& lock,
                              const std::function<bool()>& condition,
                              Event::Dispatcher& client_dispatcher, milliseconds timeout)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
  Event::TestTimeSystem::RealTimeBound bound(timeout);
  while (bound.withinBound()) {
    // Wake up every 5ms to run the client dispatcher.
    if (time_system.waitFor(lock, absl::Condition(&condition), 5ms)) {
      return true;
    }

    // Run the client dispatcher since we may need to process window updates, etc.
    client_dispatcher.run(Event::Dispatcher::RunType::NonBlock);
  }
  return false;
}
} // namespace

AssertionResult FakeStream::waitForData(Event::Dispatcher& client_dispatcher, uint64_t body_length,
                                        milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  if (!waitForWithDispatcherRun(
          time_system_, lock_,
          [this, body_length]()
              ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return (body_.length() >= body_length); },
          client_dispatcher, timeout)) {
    return AssertionFailure() << "Timed out waiting for data.";
  }
  return AssertionSuccess();
}

AssertionResult FakeStream::waitForData(Event::Dispatcher& client_dispatcher,
                                        absl::string_view data, milliseconds timeout) {
  auto succeeded = waitForData(client_dispatcher, data.length(), timeout);
  if (succeeded) {
    Buffer::OwnedImpl buffer(data.data(), data.length());
    if (!TestUtility::buffersEqual(body(), buffer)) {
      return AssertionFailure() << body().toString() << " not equal to " << data;
    }
  }
  return succeeded;
}

AssertionResult FakeStream::waitForEndStream(Event::Dispatcher& client_dispatcher,
                                             milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  if (!waitForWithDispatcherRun(
          time_system_, lock_,
          [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return end_stream_; }, client_dispatcher,
          timeout)) {
    return AssertionFailure() << "Timed out waiting for end of stream.";
  }
  return AssertionSuccess();
}

AssertionResult FakeStream::waitForReset(milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  if (!time_system_.waitFor(lock_, absl::Condition(&saw_reset_), timeout)) {
    return AssertionFailure() << "Timed out waiting for reset.";
  }
  return AssertionSuccess();
}

void FakeStream::startGrpcStream() {
  encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
}

void FakeStream::finishGrpcStream(Grpc::Status::GrpcStatus status) {
  encodeTrailers(Http::TestResponseTrailerMapImpl{
      {"grpc-status", std::to_string(static_cast<uint32_t>(status))}});
}

// The TestHttp1ServerConnectionImpl outlives its underlying Network::Connection
// so must not access the Connection on teardown. To achieve this, clear the
// read disable calls to avoid checking / editing the Connection blocked state.
class TestHttp1ServerConnectionImpl : public Http::Http1::ServerConnectionImpl {
public:
  using Http::Http1::ServerConnectionImpl::ServerConnectionImpl;

  void onMessageComplete() override {
    ServerConnectionImpl::onMessageComplete();

    if (activeRequest().has_value() && activeRequest().value().request_decoder_) {
      // Undo the read disable from the base class - we have many tests which
      // waitForDisconnect after a full request has been read which will not
      // receive the disconnect if reading is disabled.
      activeRequest().value().response_encoder_.readDisable(false);
    }
  }
  ~TestHttp1ServerConnectionImpl() override {
    if (activeRequest().has_value()) {
      activeRequest().value().response_encoder_.clearReadDisableCallsForTests();
    }
  }
};

FakeHttpConnection::FakeHttpConnection(
    FakeUpstream& fake_upstream, SharedConnectionWrapper& shared_connection, Type type,
    Event::TestTimeSystem& time_system, uint32_t max_request_headers_kb,
    uint32_t max_request_headers_count,
    envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
        headers_with_underscores_action)
    : FakeConnectionBase(shared_connection, time_system), type_(type) {
  if (type == Type::HTTP1) {
    Http::Http1Settings http1_settings;
    // For the purpose of testing, we always have the upstream encode the trailers if any
    http1_settings.enable_trailers_ = true;
    Http::Http1::CodecStats& stats = fake_upstream.http1CodecStats();
    codec_ = std::make_unique<TestHttp1ServerConnectionImpl>(
        shared_connection_.connection(), stats, *this, http1_settings, max_request_headers_kb,
        max_request_headers_count, headers_with_underscores_action);
  } else if (type == Type::HTTP2) {
    envoy::config::core::v3::Http2ProtocolOptions http2_options =
        ::Envoy::Http2::Utility::initializeAndValidateOptions(
            envoy::config::core::v3::Http2ProtocolOptions());
    http2_options.set_allow_connect(true);
    http2_options.set_allow_metadata(true);
    Http::Http2::CodecStats& stats = fake_upstream.http2CodecStats();
    codec_ = std::make_unique<Http::Http2::ServerConnectionImpl>(
        shared_connection_.connection(), *this, stats, random_, http2_options,
        max_request_headers_kb, max_request_headers_count, headers_with_underscores_action);
  } else {
    ASSERT(type == Type::HTTP3);
    envoy::config::core::v3::Http3ProtocolOptions http3_options;
    codec_ = std::unique_ptr<Http::ServerConnection>(
        Config::Utility::getAndCheckFactoryByName<Http::QuicHttpServerConnectionFactory>(
            Http::QuicCodecNames::get().Quiche)
            .createQuicServerConnection(shared_connection_.connection(), *this,
                                        max_request_headers_kb, max_request_headers_count,
                                        headers_with_underscores_action));
  }
  shared_connection_.connection().addReadFilter(
      Network::ReadFilterSharedPtr{new ReadFilter(*this)});
}

AssertionResult FakeConnectionBase::close(std::chrono::milliseconds timeout) {
  ENVOY_LOG(trace, "FakeConnectionBase close");
  if (!shared_connection_.connected()) {
    return AssertionSuccess();
  }
  return shared_connection_.executeOnDispatcher(
      [](Network::Connection& connection) {
        connection.close(Network::ConnectionCloseType::FlushWrite);
      },
      timeout);
}

AssertionResult FakeConnectionBase::readDisable(bool disable, std::chrono::milliseconds timeout) {
  return shared_connection_.executeOnDispatcher(
      [disable](Network::Connection& connection) { connection.readDisable(disable); }, timeout);
}

Http::RequestDecoder& FakeHttpConnection::newStream(Http::ResponseEncoder& encoder, bool) {
  absl::MutexLock lock(&lock_);
  new_streams_.emplace_back(new FakeStream(*this, encoder, time_system_));
  return *new_streams_.back();
}

void FakeHttpConnection::onGoAway(Http::GoAwayErrorCode code) {
  ASSERT(type_ >= Type::HTTP2);
  // Usually indicates connection level errors, no operations are needed since
  // the connection will be closed soon.
  ENVOY_LOG(info, "FakeHttpConnection receives GOAWAY: ", code);
}

void FakeHttpConnection::encodeGoAway() {
  ASSERT(type_ >= Type::HTTP2);

  shared_connection_.connection().dispatcher().post([this]() { codec_->goAway(); });
}

void FakeHttpConnection::encodeProtocolError() {
  ASSERT(type_ >= Type::HTTP2);

  Http::Http2::ServerConnectionImpl* codec =
      dynamic_cast<Http::Http2::ServerConnectionImpl*>(codec_.get());
  ASSERT(codec != nullptr);
  shared_connection_.connection().dispatcher().post([codec]() {
    Http::Status status = codec->protocolErrorForTest();
    ASSERT(Http::getStatusCode(status) == Http::StatusCode::CodecProtocolError);
  });
}

AssertionResult FakeConnectionBase::waitForDisconnect(milliseconds timeout) {
  ENVOY_LOG(trace, "FakeConnectionBase waiting for disconnect");
  absl::MutexLock lock(&lock_);
  const auto reached = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return !shared_connection_.connectedLockHeld();
  };

  if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
    if (timeout == TestUtility::DefaultTimeout) {
      ADD_FAILURE() << "Please don't waitForDisconnect with a 5s timeout if failure is expected\n";
    }
    return AssertionFailure() << "Timed out waiting for disconnect.";
  }
  ENVOY_LOG(trace, "FakeConnectionBase done waiting for disconnect");
  return AssertionSuccess();
}

AssertionResult FakeConnectionBase::waitForHalfClose(milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  if (!time_system_.waitFor(lock_, absl::Condition(&half_closed_), timeout)) {
    return AssertionFailure() << "Timed out waiting for half close.";
  }
  return AssertionSuccess();
}

AssertionResult FakeHttpConnection::waitForNewStream(Event::Dispatcher& client_dispatcher,
                                                     FakeStreamPtr& stream,
                                                     std::chrono::milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  if (!waitForWithDispatcherRun(
          time_system_, lock_,
          [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return !new_streams_.empty(); },
          client_dispatcher, timeout)) {
    return AssertionFailure() << "Timed out waiting for new stream.";
  }
  stream = std::move(new_streams_.front());
  new_streams_.pop_front();
  return AssertionSuccess();
}

FakeUpstream::FakeUpstream(const std::string& uds_path, const FakeUpstreamConfig& config)
    : FakeUpstream(Network::Test::createRawBufferSocketFactory(),
                   Network::SocketPtr{new Network::UdsListenSocket(
                       std::make_shared<Network::Address::PipeInstance>(uds_path))},
                   config) {
  ENVOY_LOG(info, "starting fake server on unix domain socket {}", uds_path);
}

static Network::SocketPtr
makeTcpListenSocket(const Network::Address::InstanceConstSharedPtr& address) {
  return std::make_unique<Network::TcpListenSocket>(address, nullptr, true);
}

static Network::SocketPtr makeTcpListenSocket(uint32_t port, Network::Address::IpVersion version) {
  return makeTcpListenSocket(Network::Utility::parseInternetAddress(
      Network::Test::getLoopbackAddressString(version), port));
}

static Network::SocketPtr
makeUdpListenSocket(const Network::Address::InstanceConstSharedPtr& address) {
  auto socket = std::make_unique<Network::UdpListenSocket>(address, nullptr, true);
  // TODO(mattklein123): These options are set in multiple locations. We should centralize them for
  // UDP listeners.
  socket->addOptions(Network::SocketOptionFactory::buildIpPacketInfoOptions());
  socket->addOptions(Network::SocketOptionFactory::buildRxQueueOverFlowOptions());
  return socket;
}

static Network::SocketPtr
makeListenSocket(const FakeUpstreamConfig& config,
                 const Network::Address::InstanceConstSharedPtr& address) {
  return (config.udp_fake_upstream_ ? makeUdpListenSocket(address) : makeTcpListenSocket(address));
}

FakeUpstream::FakeUpstream(const Network::Address::InstanceConstSharedPtr& address,
                           const FakeUpstreamConfig& config)
    : FakeUpstream(Network::Test::createRawBufferSocketFactory(), makeListenSocket(config, address),
                   config) {
  ENVOY_LOG(info, "starting fake server on socket {}:{}. Address version is {}. UDP={}",
            address->ip()->addressAsString(), address->ip()->port(),
            Network::Test::addressVersionAsString(address->ip()->version()),
            config.udp_fake_upstream_);
}

FakeUpstream::FakeUpstream(uint32_t port, Network::Address::IpVersion version,
                           const FakeUpstreamConfig& config)
    : FakeUpstream(Network::Test::createRawBufferSocketFactory(),
                   makeTcpListenSocket(port, version), config) {
  ASSERT(!config.udp_fake_upstream_);
  ENVOY_LOG(info, "starting fake server on port {}. Address version is {}",
            localAddress()->ip()->port(), Network::Test::addressVersionAsString(version));
}

FakeUpstream::FakeUpstream(Network::TransportSocketFactoryPtr&& transport_socket_factory,
                           const Network::Address::InstanceConstSharedPtr& address,
                           const FakeUpstreamConfig& config)
    : FakeUpstream(std::move(transport_socket_factory), makeListenSocket(config, address), config) {
  ENVOY_LOG(info, "starting fake server on socket {}:{}. Address version is {}. UDP={}",
            address->ip()->addressAsString(), address->ip()->port(),
            Network::Test::addressVersionAsString(address->ip()->version()),
            config.udp_fake_upstream_);
}

FakeUpstream::FakeUpstream(Network::TransportSocketFactoryPtr&& transport_socket_factory,
                           uint32_t port, Network::Address::IpVersion version,
                           const FakeUpstreamConfig& config)
    : FakeUpstream(std::move(transport_socket_factory), makeTcpListenSocket(port, version),
                   config) {
  ASSERT(!config.udp_fake_upstream_);
  ENVOY_LOG(info, "starting fake server on port {}. Address version is {}",
            localAddress()->ip()->port(), Network::Test::addressVersionAsString(version));
}

FakeUpstream::FakeUpstream(Network::TransportSocketFactoryPtr&& transport_socket_factory,
                           Network::SocketPtr&& listen_socket, const FakeUpstreamConfig& config)
    : http_type_(config.upstream_protocol_),
      socket_(Network::SocketSharedPtr(listen_socket.release())),
      socket_factory_(std::make_shared<FakeListenSocketFactory>(socket_)),
      api_(Api::createApiForTest(stats_store_)), time_system_(config.time_system_),
      dispatcher_(api_->allocateDispatcher("fake_upstream")),
      handler_(new Server::ConnectionHandlerImpl(*dispatcher_, 0)),
      read_disable_on_new_connection_(true), enable_half_close_(config.enable_half_close_),
      listener_(*this, http_type_ == FakeHttpConnection::Type::HTTP3),
      filter_chain_(Network::Test::createEmptyFilterChain(std::move(transport_socket_factory))) {
  thread_ = api_->threadFactory().createThread([this]() -> void { threadRoutine(); });
  server_initialized_.waitReady();
}

FakeUpstream::~FakeUpstream() { cleanUp(); };

void FakeUpstream::cleanUp() {
  if (thread_.get()) {
    dispatcher_->exit();
    thread_->join();
    thread_.reset();
  }
}

bool FakeUpstream::createNetworkFilterChain(Network::Connection& connection,
                                            const std::vector<Network::FilterFactoryCb>&) {
  absl::MutexLock lock(&lock_);
  if (read_disable_on_new_connection_) {
    // Disable early close detection to avoid closing the network connection before full
    // initialization is complete.
    connection.detectEarlyCloseWhenReadDisabled(false);
    connection.readDisable(true);
  }
  auto connection_wrapper = std::make_unique<SharedConnectionWrapper>(connection);

  LinkedList::moveIntoListBack(std::move(connection_wrapper), new_connections_);

  // Normally we don't associate a logical network connection with a FakeHttpConnection  until
  // waitForHttpConnection is called, but QUIC needs to be set up as packets come in, so we do
  // not lazily create for HTTP/3
  // TODO(#14829) handle the case where waitForHttpConnection is called with
  // non-default arguments for max request headers and protocol options.
  if (http_type_ == FakeHttpConnection::Type::HTTP3) {
    quic_connections_.push_back(std::make_unique<FakeHttpConnection>(
        *this, consumeConnection(), http_type_, time_system_, Http::DEFAULT_MAX_REQUEST_HEADERS_KB,
        Http::DEFAULT_MAX_HEADERS_COUNT, envoy::config::core::v3::HttpProtocolOptions::ALLOW));
    quic_connections_.back()->initialize();
  }
  return true;
}

bool FakeUpstream::createListenerFilterChain(Network::ListenerFilterManager&) { return true; }

void FakeUpstream::createUdpListenerFilterChain(Network::UdpListenerFilterManager& udp_listener,
                                                Network::UdpReadFilterCallbacks& callbacks) {
  udp_listener.addReadFilter(std::make_unique<FakeUpstreamUdpFilter>(*this, callbacks));
}

void FakeUpstream::threadRoutine() {
  handler_->addListener(absl::nullopt, listener_);
  server_initialized_.setReady();
  dispatcher_->run(Event::Dispatcher::RunType::Block);
  handler_.reset();
  {
    absl::MutexLock lock(&lock_);
    new_connections_.clear();
    consumed_connections_.clear();
  }
}

AssertionResult FakeUpstream::waitForHttpConnection(
    Event::Dispatcher& client_dispatcher, FakeHttpConnectionPtr& connection, milliseconds timeout,
    uint32_t max_request_headers_kb, uint32_t max_request_headers_count,
    envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
        headers_with_underscores_action) {
  {
    absl::MutexLock lock(&lock_);

    // As noted in createNetworkFilterChain, HTTP3 FakeHttpConnections are not
    // lazily created, so HTTP3 needs a different wait path here.
    if (http_type_ == FakeHttpConnection::Type::HTTP3) {
      if (quic_connections_.empty() &&
          !waitForWithDispatcherRun(
              time_system_, lock_,
              [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return !quic_connections_.empty(); },
              client_dispatcher, timeout)) {
        return AssertionFailure() << "Timed out waiting for new quic connection.";
      }
      if (!quic_connections_.empty()) {
        connection = std::move(quic_connections_.front());
        quic_connections_.pop_front();
        return AssertionSuccess();
      }
    }

    if (!waitForWithDispatcherRun(
            time_system_, lock_,
            [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return !new_connections_.empty(); },
            client_dispatcher, timeout)) {
      if (timeout == TestUtility::DefaultTimeout) {
        ADD_FAILURE()
            << "Please don't waitForHttpConnection with a 5s timeout if failure is expected\n";
      }
      return AssertionFailure() << "Timed out waiting for new connection.";
    }
  }

  return runOnDispatcherThreadAndWait([&]() {
    absl::MutexLock lock(&lock_);
    connection = std::make_unique<FakeHttpConnection>(
        *this, consumeConnection(), http_type_, time_system_, max_request_headers_kb,
        max_request_headers_count, headers_with_underscores_action);
    connection->initialize();
    return AssertionSuccess();
  });
}

AssertionResult
FakeUpstream::waitForHttpConnection(Event::Dispatcher& client_dispatcher,
                                    std::vector<std::unique_ptr<FakeUpstream>>& upstreams,
                                    FakeHttpConnectionPtr& connection, milliseconds timeout) {
  if (upstreams.empty()) {
    return AssertionFailure() << "No upstreams configured.";
  }
  Event::TestTimeSystem::RealTimeBound bound(timeout);
  while (bound.withinBound()) {
    for (auto& it : upstreams) {
      FakeUpstream& upstream = *it;
      {
        absl::MutexLock lock(&upstream.lock_);
        if (!waitForWithDispatcherRun(
                upstream.time_system_, upstream.lock_,
                [&upstream]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(upstream.lock_) {
                  return !upstream.new_connections_.empty();
                },
                client_dispatcher, 5ms)) {
          continue;
        }
      }

      return upstream.runOnDispatcherThreadAndWait([&]() {
        absl::MutexLock lock(&upstream.lock_);
        connection = std::make_unique<FakeHttpConnection>(
            upstream, upstream.consumeConnection(), upstream.http_type_, upstream.timeSystem(),
            Http::DEFAULT_MAX_REQUEST_HEADERS_KB, Http::DEFAULT_MAX_HEADERS_COUNT,
            envoy::config::core::v3::HttpProtocolOptions::ALLOW);
        connection->initialize();
        return AssertionSuccess();
      });
    }
  }
  return AssertionFailure() << "Timed out waiting for HTTP connection.";
}

AssertionResult FakeUpstream::waitForRawConnection(FakeRawConnectionPtr& connection,
                                                   milliseconds timeout) {
  {
    absl::MutexLock lock(&lock_);
    const auto reached = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
      return !new_connections_.empty();
    };

    ENVOY_LOG(debug, "waiting for raw connection");
    if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
      return AssertionFailure() << "Timed out waiting for raw connection";
    }
  }

  return runOnDispatcherThreadAndWait([&]() {
    absl::MutexLock lock(&lock_);
    connection = makeRawConnection(consumeConnection(), timeSystem());
    connection->initialize();
    // Skip enableHalfClose if the connection is already disconnected.
    if (connection->connected()) {
      connection->connection().enableHalfClose(enable_half_close_);
    }
    return AssertionSuccess();
  });
}

SharedConnectionWrapper& FakeUpstream::consumeConnection() {
  ASSERT(!new_connections_.empty());
  auto* const connection_wrapper = new_connections_.front().get();
  // Skip the thread safety check if the network connection has already been freed since there's no
  // alternate way to get access to the dispatcher.
  ASSERT(!connection_wrapper->connected() ||
         connection_wrapper->connection().dispatcher().isThreadSafe());
  connection_wrapper->setParented();
  connection_wrapper->moveBetweenLists(new_connections_, consumed_connections_);
  if (read_disable_on_new_connection_ && connection_wrapper->connected()) {
    // Re-enable read and early close detection.
    auto& connection = connection_wrapper->connection();
    connection.detectEarlyCloseWhenReadDisabled(true);
    connection.readDisable(false);
  }
  return *connection_wrapper;
}

testing::AssertionResult FakeUpstream::waitForUdpDatagram(Network::UdpRecvData& data_to_fill,
                                                          std::chrono::milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  const auto reached = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return !received_datagrams_.empty();
  };

  if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
    return AssertionFailure() << "Timed out waiting for UDP datagram.";
  }

  data_to_fill = std::move(received_datagrams_.front());
  received_datagrams_.pop_front();
  return AssertionSuccess();
}

void FakeUpstream::onRecvDatagram(Network::UdpRecvData& data) {
  absl::MutexLock lock(&lock_);
  received_datagrams_.emplace_back(std::move(data));
}

AssertionResult FakeUpstream::runOnDispatcherThreadAndWait(std::function<AssertionResult()> cb,
                                                           std::chrono::milliseconds timeout) {
  auto result = std::make_shared<AssertionResult>(AssertionSuccess());
  auto done = std::make_shared<absl::Notification>();
  ASSERT(!dispatcher_->isThreadSafe());
  dispatcher_->post([&]() {
    *result = cb();
    done->Notify();
  });
  RELEASE_ASSERT(done->WaitForNotificationWithTimeout(absl::FromChrono(timeout)),
                 "Timed out waiting for cb to run on dispatcher");
  return *result;
}

void FakeUpstream::sendUdpDatagram(const std::string& buffer,
                                   const Network::Address::InstanceConstSharedPtr& peer) {
  dispatcher_->post([this, buffer, peer] {
    const auto rc = Network::Utility::writeToSocket(socket_->ioHandle(), Buffer::OwnedImpl(buffer),
                                                    nullptr, *peer);
    EXPECT_TRUE(rc.rc_ == buffer.length());
  });
}

testing::AssertionResult FakeUpstream::rawWriteConnection(uint32_t index, const std::string& data,
                                                          bool end_stream,
                                                          std::chrono::milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  auto iter = consumed_connections_.begin();
  std::advance(iter, index);
  return (*iter)->executeOnDispatcher(
      [data, end_stream](Network::Connection& connection) {
        ASSERT(connection.state() == Network::Connection::State::Open);
        Buffer::OwnedImpl buffer(data);
        connection.write(buffer, end_stream);
      },
      timeout);
}

FakeRawConnection::~FakeRawConnection() {
  // If the filter was already deleted, it means the shared_connection_ was too, so don't try to
  // access it.
  if (auto filter = read_filter_.lock(); filter != nullptr) {
    EXPECT_TRUE(shared_connection_.executeOnDispatcher(
        [filter = std::move(filter)](Network::Connection& connection) {
          connection.removeReadFilter(filter);
        }));
  }
}

void FakeRawConnection::initialize() {
  FakeConnectionBase::initialize();
  Network::ReadFilterSharedPtr filter{new ReadFilter(*this)};
  read_filter_ = filter;
  if (!shared_connection_.connected()) {
    ENVOY_LOG(warn, "FakeRawConnection::initialize: network connection is already disconnected");
    return;
  }
  ASSERT(shared_connection_.connection().dispatcher().isThreadSafe());
  shared_connection_.connection().addReadFilter(filter);
}

AssertionResult FakeRawConnection::waitForData(uint64_t num_bytes, std::string* data,
                                               milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  const auto reached = [this, num_bytes]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return data_.size() == num_bytes;
  };
  ENVOY_LOG(debug, "waiting for {} bytes of data", num_bytes);
  if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
    return AssertionFailure() << fmt::format(
               "Timed out waiting for data. Got '{}', waiting for {} bytes.", data_, num_bytes);
  }
  if (data != nullptr) {
    *data = data_;
  }
  return AssertionSuccess();
}

AssertionResult
FakeRawConnection::waitForData(const std::function<bool(const std::string&)>& data_validator,
                               std::string* data, milliseconds timeout) {
  absl::MutexLock lock(&lock_);
  const auto reached = [this, &data_validator]()
                           ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) { return data_validator(data_); };
  ENVOY_LOG(debug, "waiting for data");
  if (!time_system_.waitFor(lock_, absl::Condition(&reached), timeout)) {
    return AssertionFailure() << "Timed out waiting for data.";
  }
  if (data != nullptr) {
    *data = data_;
  }
  return AssertionSuccess();
}

AssertionResult FakeRawConnection::write(const std::string& data, bool end_stream,
                                         milliseconds timeout) {
  return shared_connection_.executeOnDispatcher(
      [data, end_stream](Network::Connection& connection) {
        Buffer::OwnedImpl to_write(data);
        connection.write(to_write, end_stream);
      },
      timeout);
}

Network::FilterStatus FakeRawConnection::ReadFilter::onData(Buffer::Instance& data,
                                                            bool end_stream) {
  absl::MutexLock lock(&parent_.lock_);
  ENVOY_LOG(debug, "got {} bytes, end_stream {}", data.length(), end_stream);
  parent_.data_.append(data.toString());
  parent_.half_closed_ = end_stream;
  data.drain(data.length());
  return Network::FilterStatus::StopIteration;
}
} // namespace Envoy
