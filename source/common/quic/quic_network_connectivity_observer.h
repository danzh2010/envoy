#pragma once

#include <memory>

#include "envoy/network/network_observer.h"

#include "source/common/common/logger.h"

namespace Envoy {
namespace Quic {

class EnvoyQuicClientSession;

// TODO(danzh) make this class a wrapper around QUICHE's own network observer.
class QuicNetworkConnectivityObserver : public Network::NetworkConnectivityObserver,
                                        protected Logger::Loggable<Logger::Id::connection> {
public:
  // session must outlive this object.
  explicit QuicNetworkConnectivityObserver(EnvoyQuicClientSession& session);
  QuicNetworkConnectivityObserver(const QuicNetworkConnectivityObserver&) = delete;
  QuicNetworkConnectivityObserver& operator=(const QuicNetworkConnectivityObserver&) = delete;

  // Network::NetworkConnectivityObserver
  void onNetworkChanged() override {
    // TODO(danzh) close the connection if it's idle, otherwise mark it as go away.
    (void)session_;
  }

private:
  EnvoyQuicClientSession& session_;
};

using QuicNetworkConnectivityObserverPtr = std::unique_ptr<QuicNetworkConnectivityObserver>;

} // namespace Quic
} // namespace Envoy
