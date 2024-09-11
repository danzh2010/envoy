#pragma once

#include <memory>

#include "envoy/common/pure.h"
#include "envoy/event/dispatcher.h"

namespace Envoy {

namespace Network {

// Used by ClientConnection to observe underlying network change events from the platform.
class NetworkConnectivityObserver {
public:
  virtual ~NetworkConnectivityObserver() = default;

  /**
   * Called when the device switches to a different network, i.e. from Wifi to Cellular.
   */
  virtual void onNetworkChanged() PURE;
};

// A registry of network connectivity observers, usually one for each worker thread.
class NetworkObserverRegistry {
public:
  virtual ~NetworkObserverRegistry() = default;

  /**
   * Register an observer for network change events.
   * @param observer that is interested in network connectivity changes.
   */
  virtual void registerObserver(NetworkConnectivityObserver& observer) PURE;

  /**
   * Unregister an observer for network change events.
   * @param observer that is no longer interested in network connectivity changes. No-op if not
   * registered before.
   */
  virtual void unregisterObserver(NetworkConnectivityObserver& observer) PURE;
};

// A factory of NetworkObserverRegistry, usually owned by main thread.
class NetworkObserverRegistryFactory {
public:
  virtual ~NetworkObserverRegistryFactory() = default;

  /**
   * Create a registry instance.
   * @param dispatcher that any network change events should be dispatched on.
   */
  virtual std::unique_ptr<NetworkObserverRegistry>
  createNetworkObserverRegistry(Event::Dispatcher& dispatcher) PURE;
};

} // namespace Network

} // namespace Envoy
