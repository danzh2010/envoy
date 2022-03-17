#include "source/common/http/alternate_protocols_cache_impl.h"

#include "source/common/common/logger.h"

#include "http3_status_tracker_impl.h"
#include "quiche/spdy/core/spdy_alt_svc_wire_format.h"
#include "re2/re2.h"

namespace Envoy {
namespace Http {
namespace {

struct RegexHolder {
  RegexHolder() : origin_regex("(.*)://(.*):(\\d+)") {}

  const re2::RE2 origin_regex;
};

using ConstRegexHolder = ConstSingleton<RegexHolder>;

} // namespace

std::string
AlternateProtocolsCacheImpl::originToString(const AlternateProtocolsCache::Origin& origin) {
  return absl::StrCat(origin.scheme_, "://", origin.hostname_, ":", origin.port_);
}

absl::optional<AlternateProtocolsCache::Origin>
AlternateProtocolsCacheImpl::stringToOrigin(const std::string& str) {
  const re2::RE2& origin_regex = ConstRegexHolder::get().origin_regex;
  std::string scheme;
  std::string hostname;
  int port = 0;
  if (re2::RE2::FullMatch(str.c_str(), origin_regex, &scheme, &hostname, &port)) {
    return AlternateProtocolsCache::Origin(scheme, hostname, port);
  }
  return {};
}

std::string AlternateProtocolsCacheImpl::originDataToStringForCache(const OriginData& origin_data) {
  if (origin_data.protocols.empty()) {
    return std::string("clear|0");
  }
  std::string value;
  for (auto& protocol : origin_data.protocols) {
    if (!value.empty()) {
      value.push_back(',');
    }
    absl::StrAppend(&value, protocol.alpn_, "=\"", protocol.hostname_, ":", protocol.port_, "\"");

    // Note this is _not_ actually the max age, but the absolute time at which
    // this entry will expire. protocolsFromString will convert back to ma.
    absl::StrAppend(
        &value, "; ma=",
        std::chrono::duration_cast<std::chrono::seconds>(protocol.expiration_.time_since_epoch())
            .count());
  }
  absl::StrAppend(&value, "|", origin_data.srtt.count());
  if (origin_data.h3_status_tracker != nullptr) {
    absl::StrAppend(&value, "|", origin_data.h3_status_tracker->statusToStringForCache());
  }
  return value;
}

absl::optional<AlternateProtocolsCacheImpl::OriginData>
AlternateProtocolsCacheImpl::originDataFromString(const Origin& origin,
                                                  absl::string_view origin_data_string,
                                                  Event::Dispatcher& dispatcher,
                                                  Http3StatusTrackerCallback& callback,
                                                  bool from_cache) {
  OriginData data;
  const std::vector<absl::string_view> parts = absl::StrSplit(origin_data_string, '|');
  if (parts.size() > 3) {
    return {};
  }
  if (parts.size() == 3) {
    data.h3_status_tracker =
        Http3StatusTrackerImpl::createFromCachedString(dispatcher, origin, parts[2], callback);
    if (data.h3_status_tracker == nullptr) {
      return {};
    }
  }
  if (parts.size() >= 2) {
    int64_t srtt;
    if (!absl::SimpleAtoi(parts[1], &srtt)) {
      return {};
    }
    data.srtt = std::chrono::microseconds(srtt);
  } else if (parts.size() == 1) {
    // Handling raw alt-svc with no endpoint info
    data.srtt = std::chrono::microseconds(0);
  }

  spdy::SpdyAltSvcWireFormat::AlternativeServiceVector altsvc_vector;
  if (!spdy::SpdyAltSvcWireFormat::ParseHeaderFieldValue(parts[0], &altsvc_vector)) {
    return {};
  }
  for (const auto& alt_svc : altsvc_vector) {
    MonotonicTime expiration;
    if (from_cache) {
      auto expire_time_from_epoch = std::chrono::seconds(alt_svc.max_age_seconds);
      auto time_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(
          dispatcher.approximateMonotonicTime().time_since_epoch());
      if (expire_time_from_epoch < time_since_epoch) {
        expiration = dispatcher.approximateMonotonicTime();
      } else {
        expiration =
            dispatcher.approximateMonotonicTime() + (expire_time_from_epoch - time_since_epoch);
      }
    } else {
      expiration =
          dispatcher.approximateMonotonicTime() + std::chrono::seconds(alt_svc.max_age_seconds);
    }
    Http::AlternateProtocolsCache::AlternateProtocol protocol(alt_svc.protocol_id, alt_svc.host,
                                                              alt_svc.port, expiration);
    data.protocols.push_back(protocol);
  }
  return data;
}

AlternateProtocolsCacheImpl::AlternateProtocolsCacheImpl(
    Event::Dispatcher& dispatcher, std::unique_ptr<KeyValueStore>&& key_value_store,
    size_t max_entries)
    : dispatcher_(dispatcher), max_entries_(max_entries > 0 ? max_entries : 1024) {
  if (key_value_store) {
    KeyValueStore::ConstIterateCb load_protocols = [this](const std::string& key,
                                                          const std::string& value) {
      absl::optional<Origin> origin = stringToOrigin(key);
      if (!origin.has_value()) {
        ENVOY_LOG(warn, fmt::format("Unable to parse cache entry with key: {}", key));
        return KeyValueStore::Iterate::Continue;
      }
      absl::optional<OriginData> origin_data =
          originDataFromString(origin.value(), value, dispatcher_, *this, true);
      if (!origin_data.has_value()) {
        ENVOY_LOG(warn,
                  fmt::format("Unable to parse cache entry with key: {} value: {}", key, value));
        return KeyValueStore::Iterate::Continue;
      }
      // We deferred transfering ownership into key_value_store_ prior, so
      // that we won't end up doing redundant updates to the store while
      // iterating.
      setAlternativesImpl(origin.value(), origin_data.value().protocols);
      setSrttImpl(origin.value(), origin_data.value().srtt);
      protocols_[origin.value()].h3_status_tracker = std::move(origin_data->h3_status_tracker);
      return KeyValueStore::Iterate::Continue;
    };
    key_value_store->iterate(load_protocols);
    key_value_store_ = std::move(key_value_store);
  }
}

AlternateProtocolsCacheImpl::~AlternateProtocolsCacheImpl() = default;

void AlternateProtocolsCacheImpl::setAlternatives(const Origin& origin,
                                                  std::vector<AlternateProtocol>& protocols) {
  setAlternativesImpl(origin, protocols);
  if (key_value_store_) {
    key_value_store_->addOrUpdate(originToString(origin),
                                  originDataToStringForCache(protocols_[origin]));
  }
}

void AlternateProtocolsCacheImpl::setSrtt(const Origin& origin, std::chrono::microseconds srtt) {
  setSrttImpl(origin, srtt);
}

void AlternateProtocolsCacheImpl::setSrttImpl(const Origin& origin,
                                              std::chrono::microseconds srtt) {
  auto entry_it = protocols_.find(origin);
  if (entry_it == protocols_.end()) {
    return;
  }
  entry_it->second.srtt = srtt;
  if (key_value_store_) {
    key_value_store_->addOrUpdate(originToString(origin),
                                  originDataToStringForCache(entry_it->second));
  }
}

std::chrono::microseconds AlternateProtocolsCacheImpl::getSrtt(const Origin& origin) const {
  auto entry_it = protocols_.find(origin);
  if (entry_it == protocols_.end()) {
    return std::chrono::microseconds(0);
  }
  return entry_it->second.srtt;
}

void AlternateProtocolsCacheImpl::setAlternativesImpl(const Origin& origin,
                                                      std::vector<AlternateProtocol>& protocols) {
  static const size_t max_protocols = 10;
  if (protocols.size() > max_protocols) {
    ENVOY_LOG_MISC(trace, "Too many alternate protocols: {}, truncating", protocols.size());
    protocols.erase(protocols.begin() + max_protocols, protocols.end());
  }
  while (protocols_.size() >= max_entries_) {
    auto iter = protocols_.begin();
    key_value_store_->remove(originToString(iter->first));
    protocols_.erase(iter);
  }
  protocols_[origin] = OriginData{protocols, std::chrono::microseconds(0), nullptr};
}

OptRef<const std::vector<AlternateProtocolsCache::AlternateProtocol>>
AlternateProtocolsCacheImpl::findAlternatives(const Origin& origin) {
  auto entry_it = protocols_.find(origin);
  if (entry_it == protocols_.end()) {
    return makeOptRefFromPtr<const std::vector<AlternateProtocol>>(nullptr);
  }
  std::vector<AlternateProtocol>& protocols = entry_it->second.protocols;

  auto original_size = protocols.size();
  const MonotonicTime now = dispatcher_.timeSource().monotonicTime();
  protocols.erase(std::remove_if(protocols.begin(), protocols.end(),
                                 [now](const AlternateProtocol& protocol) {
                                   return (now > protocol.expiration_);
                                 }),
                  protocols.end());

  if (protocols.empty()) {
    protocols_.erase(entry_it);
    if (key_value_store_) {
      key_value_store_->remove(originToString(origin));
    }
    return makeOptRefFromPtr<const std::vector<AlternateProtocol>>(nullptr);
  }
  if (key_value_store_ && original_size != protocols.size()) {
    key_value_store_->addOrUpdate(originToString(origin),
                                  originDataToStringForCache(entry_it->second));
  }

  return makeOptRef(const_cast<const std::vector<AlternateProtocol>&>(protocols));
}

size_t AlternateProtocolsCacheImpl::size() const { return protocols_.size(); }

Http3StatusTrackerSharedPtr
AlternateProtocolsCacheImpl::getHttp3StatusTracker(const Origin& origin) {
  auto entry_it = protocols_.find(origin);
  if (entry_it == protocols_.end()) {
    return nullptr;
  }
  if (entry_it->second.h3_status_tracker == nullptr) {
    entry_it->second.h3_status_tracker =
        std::make_shared<Http3StatusTrackerImpl>(dispatcher_, origin, *this);
  }
  return entry_it->second.h3_status_tracker;
}

void AlternateProtocolsCacheImpl::onHttp3StatusChanged(
    const AlternateProtocolsCache::Origin& origin) {
  auto entry_it = protocols_.find(origin);
  if (entry_it == protocols_.end()) {
    return;
  }

  if (key_value_store_) {
    key_value_store_->addOrUpdate(originToString(origin),
                                  originDataToStringForCache(entry_it->second));
  }
}

} // namespace Http
} // namespace Envoy
