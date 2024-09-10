#pragma once

#include <memory>

#include "envoy/event/dispatcher.h"
#include "envoy/network/network_observer.h"

namespace Envoy {
namespace Network {

// A registry of network connectivity observers.
class NetworkObserverRegistryImpl : public NetworkObserverRegistry {
public:
  virtual ~NetworkObserverRegistryImpl() = default;

  void registerObserver(NetworkConnectivityObserver& observer) override {
    observers_.insert(&observer);
  }

  void unregisterObserver(NetworkConnectivityObserver& observer) override {
    observers_.erase(&observer);
  }

protected:
  const absl::flat_hash_set<NetworkConnectivityObserver*>& registeredObservers() const {
    return observers_;
  }

private:
  absl::flat_hash_set<NetworkConnectivityObserver*> observers_;
};

class NetworkObserverRegistryFactoryImpl : public NetworkObserverRegistryFactory {
public:
  std::unique_ptr<NetworkObserverRegistry>
  createNetworkObserverRegistry(Event::Dispatcher& /*dispatcher*/) override {
    return std::make_unique<NetworkObserverRegistryImpl>();
  }
};

} // namespace Network
} // namespace Envoy
