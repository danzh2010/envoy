#include "source/common/router/retry_state_impl.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "envoy/config/route/v3/route_components.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/utility.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codes.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/runtime/runtime_features.h"

namespace Envoy {
namespace Router {

RetryStatePtr RetryStateImpl::create(const RetryPolicy& route_policy,
                                     Http::RequestHeaderMap& request_headers,
                                     const Upstream::ClusterInfo& cluster,
                                     const VirtualCluster* vcluster, Runtime::Loader& runtime,
                                     Random::RandomGenerator& random, Event::Dispatcher& dispatcher,
                                     TimeSource& time_source, Upstream::ResourcePriority priority) {
  RetryStatePtr ret;

  // We short circuit here and do not bother with an allocation if there is no chance we will retry.
  if (request_headers.EnvoyRetryOn() || request_headers.EnvoyRetryGrpcOn() ||
      route_policy.retryOn()) {
    ret.reset(new RetryStateImpl(route_policy, request_headers, cluster, vcluster, runtime, random,
                                 dispatcher, time_source, priority));
  }

  // Consume all retry related headers to avoid them being propagated to the upstream
  request_headers.removeEnvoyRetryOn();
  request_headers.removeEnvoyRetryGrpcOn();
  request_headers.removeEnvoyMaxRetries();
  request_headers.removeEnvoyHedgeOnPerTryTimeout();
  request_headers.removeEnvoyRetriableHeaderNames();
  request_headers.removeEnvoyRetriableStatusCodes();
  request_headers.removeEnvoyUpstreamRequestPerTryTimeoutMs();

  return ret;
}

RetryStateImpl::RetryStateImpl(const RetryPolicy& route_policy,
                               Http::RequestHeaderMap& request_headers,
                               const Upstream::ClusterInfo& cluster, const VirtualCluster* vcluster,
                               Runtime::Loader& runtime, Random::RandomGenerator& random,
                               Event::Dispatcher& dispatcher, TimeSource& time_source,
                               Upstream::ResourcePriority priority)
    : cluster_(cluster), vcluster_(vcluster), runtime_(runtime), random_(random),
      dispatcher_(dispatcher), time_source_(time_source), retry_on_(route_policy.retryOn()),
      retries_remaining_(route_policy.numRetries()), priority_(priority),
      retry_host_predicates_(route_policy.retryHostPredicates()),
      retry_priority_(route_policy.retryPriority()),
      retriable_status_codes_(route_policy.retriableStatusCodes()),
      retriable_headers_(route_policy.retriableHeaders()),
      reset_headers_(route_policy.resetHeaders()),
      reset_max_interval_(route_policy.resetMaxInterval()) {

  std::chrono::milliseconds base_interval(
      runtime_.snapshot().getInteger("upstream.base_retry_backoff_ms", 25));
  if (route_policy.baseInterval()) {
    base_interval = *route_policy.baseInterval();
  }

  // By default, cap the max interval to 10 times the base interval to ensure reasonable back-off
  // intervals.
  std::chrono::milliseconds max_interval = base_interval * 10;
  if (route_policy.maxInterval()) {
    max_interval = *route_policy.maxInterval();
  }

  backoff_strategy_ = std::make_unique<JitteredExponentialBackOffStrategy>(
      base_interval.count(), max_interval.count(), random_);
  host_selection_max_attempts_ = route_policy.hostSelectionMaxAttempts();

  // Merge in the headers.
  if (request_headers.EnvoyRetryOn()) {
    retry_on_ |= parseRetryOn(request_headers.getEnvoyRetryOnValue()).first;
  }
  if (request_headers.EnvoyRetryGrpcOn()) {
    retry_on_ |= parseRetryGrpcOn(request_headers.getEnvoyRetryGrpcOnValue()).first;
  }

  const auto& retriable_request_headers = route_policy.retriableRequestHeaders();
  if (!retriable_request_headers.empty()) {
    // If this route limits retries by request headers, make sure there is a match.
    bool request_header_match = false;
    for (const auto& retriable_header : retriable_request_headers) {
      if (retriable_header->matchesHeaders(request_headers)) {
        request_header_match = true;
        break;
      }
    }

    if (!request_header_match) {
      retry_on_ = 0;
    }
  }
  if (retry_on_ != 0 && request_headers.EnvoyMaxRetries()) {
    uint64_t temp;
    if (absl::SimpleAtoi(request_headers.getEnvoyMaxRetriesValue(), &temp)) {
      // The max retries header takes precedence if set.
      retries_remaining_ = temp;
    }
  }

  if (request_headers.EnvoyRetriableStatusCodes()) {
    for (const auto& code :
         StringUtil::splitToken(request_headers.getEnvoyRetriableStatusCodesValue(), ",")) {
      unsigned int out;
      if (absl::SimpleAtoi(code, &out)) {
        retriable_status_codes_.emplace_back(out);
      }
    }
  }

  if (request_headers.EnvoyRetriableHeaderNames()) {
    // Retriable headers in the configuration are specified via HeaderMatcher.
    // Giving the same flexibility via request header would require the user
    // to provide HeaderMatcher serialized into a string. To avoid this extra
    // complexity we only support name-only header matchers via request
    // header. Anything more sophisticated needs to be provided via config.
    for (const auto& header_name : StringUtil::splitToken(
             request_headers.EnvoyRetriableHeaderNames()->value().getStringView(), ",")) {
      envoy::config::route::v3::HeaderMatcher header_matcher;
      header_matcher.set_name(std::string(absl::StripAsciiWhitespace(header_name)));
      retriable_headers_.emplace_back(
          std::make_shared<Http::HeaderUtility::HeaderData>(header_matcher));
    }
  }
}

RetryStateImpl::~RetryStateImpl() { resetRetry(); }

void RetryStateImpl::enableBackoffTimer() {
  if (!retry_timer_) {
    retry_timer_ = dispatcher_.createTimer([this]() -> void { backoff_callback_(); });
  }

  if (ratelimited_backoff_strategy_ != nullptr) {
    // If we have a backoff strategy based on rate limit feedback from the response we use it.
    retry_timer_->enableTimer(
        std::chrono::milliseconds(ratelimited_backoff_strategy_->nextBackOffMs()));

    // The strategy is only valid for the response that sent the ratelimit reset header and cannot
    // be reused.
    ratelimited_backoff_strategy_.reset();

    cluster_.stats().upstream_rq_retry_backoff_ratelimited_.inc();

  } else {
    // Otherwise we use a fully jittered exponential backoff algorithm.
    retry_timer_->enableTimer(std::chrono::milliseconds(backoff_strategy_->nextBackOffMs()));

    cluster_.stats().upstream_rq_retry_backoff_exponential_.inc();
  }
}

std::pair<uint32_t, bool> RetryStateImpl::parseRetryOn(absl::string_view config) {
  uint32_t ret = 0;
  bool all_fields_valid = true;
  for (const auto& retry_on : StringUtil::splitToken(config, ",", false, true)) {
    if (retry_on == Http::Headers::get().EnvoyRetryOnValues._5xx) {
      ret |= RetryPolicy::RETRY_ON_5XX;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.GatewayError) {
      ret |= RetryPolicy::RETRY_ON_GATEWAY_ERROR;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.ConnectFailure) {
      ret |= RetryPolicy::RETRY_ON_CONNECT_FAILURE;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.EnvoyRateLimited) {
      ret |= RetryPolicy::RETRY_ON_ENVOY_RATE_LIMITED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.Retriable4xx) {
      ret |= RetryPolicy::RETRY_ON_RETRIABLE_4XX;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.RefusedStream) {
      ret |= RetryPolicy::RETRY_ON_REFUSED_STREAM;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.RetriableStatusCodes) {
      ret |= RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.RetriableHeaders) {
      ret |= RetryPolicy::RETRY_ON_RETRIABLE_HEADERS;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.Reset) {
      ret |= RetryPolicy::RETRY_ON_RESET;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.AltProtocolsPostConnectFailure) {
      ret |= RetryPolicy::RETRY_ON_ALT_PROTOCOLS_POST_CONNECT_FAILURE;
    } else {
      all_fields_valid = false;
    }
  }

  return {ret, all_fields_valid};
}

std::pair<uint32_t, bool> RetryStateImpl::parseRetryGrpcOn(absl::string_view retry_grpc_on_header) {
  uint32_t ret = 0;
  bool all_fields_valid = true;
  for (const auto& retry_on : StringUtil::splitToken(retry_grpc_on_header, ",", false, true)) {
    if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Cancelled) {
      ret |= RetryPolicy::RETRY_ON_GRPC_CANCELLED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.DeadlineExceeded) {
      ret |= RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.ResourceExhausted) {
      ret |= RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Unavailable) {
      ret |= RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Internal) {
      ret |= RetryPolicy::RETRY_ON_GRPC_INTERNAL;
    } else {
      all_fields_valid = false;
    }
  }

  return {ret, all_fields_valid};
}

absl::optional<std::chrono::milliseconds>
RetryStateImpl::parseResetInterval(const Http::ResponseHeaderMap& response_headers) const {
  for (const auto& reset_header : reset_headers_) {
    const auto& interval = reset_header->parseInterval(time_source_, response_headers);
    if (interval.has_value() && interval.value() <= reset_max_interval_) {
      return interval;
    }
  }

  return absl::nullopt;
}

void RetryStateImpl::resetRetry() {
  if (backoff_callback_) {
    cluster_.resourceManager(priority_).retries().dec();
    backoff_callback_ = nullptr;
  }
  if (next_loop_callback_ != nullptr) {
    cluster_.resourceManager(priority_).retries().dec();
    next_loop_callback_ = nullptr;
  }
}

RetryStatus RetryStateImpl::shouldRetry(RetryDecision would_retry, DoRetryCallback callback) {
  // If a callback is armed from a previous shouldRetry and we don't need to
  // retry this particular request, we can infer that we did a retry earlier
  // and it was successful.
  if ((backoff_callback_ || next_loop_callback_) && would_retry == RetryDecision::NoRetry) {
    cluster_.stats().upstream_rq_retry_success_.inc();
    if (vcluster_) {
      vcluster_->stats().upstream_rq_retry_success_.inc();
    }
  }

  resetRetry();

  if (would_retry == RetryDecision::NoRetry) {
    return RetryStatus::No;
  }

  // The request has exhausted the number of retries allotted to it by the retry policy configured
  // (or the x-envoy-max-retries header).
  if (retries_remaining_ == 0) {
    cluster_.stats().upstream_rq_retry_limit_exceeded_.inc();
    if (vcluster_) {
      vcluster_->stats().upstream_rq_retry_limit_exceeded_.inc();
    }
    return RetryStatus::NoRetryLimitExceeded;
  }

  retries_remaining_--;

  if (!cluster_.resourceManager(priority_).retries().canCreate()) {
    cluster_.stats().upstream_rq_retry_overflow_.inc();
    if (vcluster_) {
      vcluster_->stats().upstream_rq_retry_overflow_.inc();
    }
    return RetryStatus::NoOverflow;
  }

  if (!runtime_.snapshot().featureEnabled("upstream.use_retry", 100)) {
    return RetryStatus::No;
  }

  ASSERT(!backoff_callback_ && !next_loop_callback_);
  cluster_.resourceManager(priority_).retries().inc();
  cluster_.stats().upstream_rq_retry_.inc();
  if (vcluster_) {
    vcluster_->stats().upstream_rq_retry_.inc();
  }
  if (would_retry == RetryDecision::RetryWithBackoff) {
    backoff_callback_ = callback;
    enableBackoffTimer();
  } else {
    next_loop_callback_ = dispatcher_.createSchedulableCallback(callback);
    next_loop_callback_->scheduleCallbackNextIteration();
  }
  return RetryStatus::Yes;
}

RetryStatus RetryStateImpl::shouldRetryHeaders(const Http::ResponseHeaderMap& response_headers,
                                               const Http::RequestHeaderMap& original_request,
                                               DoRetryHeaderCallback callback) {
  // This may be overridden in wouldRetryFromHeaders().
  bool disable_early_data = !Runtime::runtimeFeatureEnabled(
      "envoy.reloadable_features.conn_pool_new_stream_with_early_data_and_alt_svc");
  const RetryDecision retry_decision =
      wouldRetryFromHeaders(response_headers, original_request, disable_early_data);

  // Yes, we will retry based on the headers - try to parse a rate limited reset interval from the
  // response.
  if (retry_decision == RetryDecision::RetryWithBackoff && !reset_headers_.empty()) {
    const auto backoff_interval = parseResetInterval(response_headers);
    if (backoff_interval.has_value() && (backoff_interval.value().count() > 1L)) {
      ratelimited_backoff_strategy_ = std::make_unique<JitteredLowerBoundBackOffStrategy>(
          backoff_interval.value().count(), random_);
    }
  }

  return shouldRetry(retry_decision,
                     [disable_early_data, callback]() { callback(disable_early_data); });
}

RetryStatus RetryStateImpl::shouldRetryReset(Http::StreamResetReason reset_reason,
                                             absl::optional<bool> was_using_alt_svc,
                                             DoRetryResetCallback callback) {

  // Following wouldRetryFromReset() may override the value.
  bool disable_alt_svc = false;
  const RetryDecision retry_decision =
      wouldRetryFromReset(reset_reason, was_using_alt_svc, disable_alt_svc);
  return shouldRetry(retry_decision, [disable_alt_svc, callback]() { callback(disable_alt_svc); });
}

RetryStatus RetryStateImpl::shouldHedgeRetryPerTryTimeout(DoRetryCallback callback) {
  // A hedged retry on per try timeout is always retried if there are retries
  // left. NOTE: this is a bit different than non-hedged per try timeouts which
  // are only retried if the applicable retry policy specifies either
  // RETRY_ON_5XX or RETRY_ON_GATEWAY_ERROR. This is because these types of
  // retries are associated with a stream reset which is analogous to a gateway
  // error. When hedging on per try timeout is enabled, however, there is no
  // stream reset.
  return shouldRetry(RetryState::RetryDecision::RetryWithBackoff, callback);
}

RetryState::RetryDecision
RetryStateImpl::wouldRetryFromHeaders(const Http::ResponseHeaderMap& response_headers,
                                      const Http::RequestHeaderMap& original_request,
                                      bool& disable_early_data) {
  // A response that contains the x-envoy-ratelimited header comes from an upstream envoy.
  // We retry these only when the envoy-ratelimited policy is in effect.
  if (response_headers.EnvoyRateLimited() != nullptr) {
    return (retry_on_ & RetryPolicy::RETRY_ON_ENVOY_RATE_LIMITED) ? RetryDecision::RetryWithBackoff
                                                                  : RetryDecision::NoRetry;
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_5XX) {
    if (Http::CodeUtility::is5xx(Http::Utility::getResponseStatus(response_headers))) {
      return RetryDecision::RetryWithBackoff;
    }
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_GATEWAY_ERROR) {
    if (Http::CodeUtility::isGatewayError(Http::Utility::getResponseStatus(response_headers))) {
      return RetryDecision::RetryWithBackoff;
    }
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_4XX)) {
    Http::Code code = static_cast<Http::Code>(Http::Utility::getResponseStatus(response_headers));
    if (code == Http::Code::Conflict) {
      return RetryDecision::RetryWithBackoff;
    }
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES)) {
    for (auto code : retriable_status_codes_) {
      uint32_t status_code = Http::Utility::getResponseStatus(response_headers);
      if (status_code == code) {
        if (!Runtime::runtimeFeatureEnabled(
                "envoy.reloadable_features.conn_pool_new_stream_with_early_data_and_alt_svc") ||
            static_cast<Http::Code>(code) != Http::Code::TooEarly) {
          return RetryDecision::RetryWithBackoff;
        }
        if (original_request.get(Http::Headers::get().EarlyData).empty()) {
          // Retry iff the downstream request wasn't sent as early data. Otherwise, regardless if
          // the request was sent as early date in upstream or not, don't retry. Instead, forwarding
          // the response to downstream.
          disable_early_data = true;
          return RetryDecision::RetryNoBackoff;
        }
      }
    }
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_HEADERS) {
    for (const auto& retriable_header : retriable_headers_) {
      if (retriable_header->matchesHeaders(response_headers)) {
        return RetryDecision::RetryWithBackoff;
      }
    }
  }

  if (retry_on_ &
      (RetryPolicy::RETRY_ON_GRPC_CANCELLED | RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED |
       RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED | RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE |
       RetryPolicy::RETRY_ON_GRPC_INTERNAL)) {
    absl::optional<Grpc::Status::GrpcStatus> status = Grpc::Common::getGrpcStatus(response_headers);
    if (status) {
      if ((status.value() == Grpc::Status::Canceled &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_CANCELLED)) ||
          (status.value() == Grpc::Status::DeadlineExceeded &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED)) ||
          (status.value() == Grpc::Status::ResourceExhausted &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED)) ||
          (status.value() == Grpc::Status::Unavailable &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE)) ||
          (status.value() == Grpc::Status::Internal &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_INTERNAL))) {
        return RetryDecision::RetryWithBackoff;
      }
    }
  }

  return RetryDecision::NoRetry;
}

RetryState::RetryDecision
RetryStateImpl::wouldRetryFromReset(const Http::StreamResetReason reset_reason,
                                    absl::optional<bool> was_using_alt_svc, bool& disable_alt_svc) {
  ASSERT(!disable_alt_svc);
  // First check "never retry" conditions so we can short circuit (we never
  // retry if the reset reason is overflow).
  if (reset_reason == Http::StreamResetReason::Overflow) {
    return RetryDecision::NoRetry;
  }

  const bool consider_disable_alt_svc = Runtime::runtimeFeatureEnabled(
      "envoy.reloadable_features.conn_pool_new_stream_with_early_data_and_alt_svc");
  if (consider_disable_alt_svc) {
    if (reset_reason == Http::StreamResetReason::ConnectionFailure) {
      if (was_using_alt_svc.has_value()) {
        // Already got request encoder, so this must be a 0-RTT handshake failure. Retry
        // immediately.
        // TODO(danzh) consider making the retry configurable.
        ASSERT(*was_using_alt_svc, "0-RTT was attempted on non-Quic connection and failed.");
        return RetryDecision::RetryNoBackoff;
      }
      if ((retry_on_ & RetryPolicy::RETRY_ON_CONNECT_FAILURE)) {
        // This is a pool failure.
        return RetryDecision::RetryWithBackoff;
      }
    } else if (was_using_alt_svc.value() &&
               (retry_on_ & RetryPolicy::RETRY_ON_ALT_PROTOCOLS_POST_CONNECT_FAILURE)) {
      // Retry any post-handshake failure immediately with no alt_svc if the request was sent over
      // Http/3.
      disable_alt_svc = true;
      return RetryDecision::RetryNoBackoff;
    }
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_RESET) {
    return RetryDecision::RetryWithBackoff;
  }

  if (retry_on_ & (RetryPolicy::RETRY_ON_5XX | RetryPolicy::RETRY_ON_GATEWAY_ERROR)) {
    // Currently we count an upstream reset as a "5xx" (since it will result in
    // one). With RETRY_ON_RESET we may eventually remove these policies.
    return RetryDecision::RetryWithBackoff;
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_REFUSED_STREAM) &&
      reset_reason == Http::StreamResetReason::RemoteRefusedStreamReset) {
    return RetryDecision::RetryWithBackoff;
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_CONNECT_FAILURE) &&
      reset_reason == Http::StreamResetReason::ConnectionFailure) {
    ASSERT(!consider_disable_alt_svc);
    return RetryDecision::RetryWithBackoff;
  }

  return RetryDecision::NoRetry;
}

bool RetryStateImpl::wouldRetryFromRetriableStatusCode(Http::Code status_code) const {
  if ((retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES)) {
    for (auto code : retriable_status_codes_) {
      if (status_code == static_cast<Http::Code>(code)) {
        return true;
      }
    }
  }
  return false;
}

} // namespace Router
} // namespace Envoy
