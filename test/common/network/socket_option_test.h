#pragma once

#include "common/network/address_impl.h"
#include "common/network/socket_option_impl.h"

#include "test/mocks/api/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/test_common/logging.h"
#include "test/test_common/threadsafe_singleton_injector.h"

#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Network {
namespace {

class SocketOptionTest : public testing::Test {
public:
  SocketOptionTest() { socket_.local_address_.reset(); }

  Api::MockOsSysCalls os_sys_calls_;

  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls_{[this]() {
    std::cerr << "========= inject os_calls " << &os_sys_calls_ << "\n";
    // Before injecting OsSysCallsImpl, make sure validateIpv{4,6}Supported is called so the static
    // bool is initialized without requiring to mock ::socket and ::close.
    std::make_unique<Address::Ipv4Instance>("1.2.3.4", 5678);
    std::make_unique<Address::Ipv6Instance>("::1:2:3:4", 5678);
    return &os_sys_calls_;
  }()};

  NiceMock<MockListenSocket> socket_;

  void testSetSocketOptionSuccess(
      Socket::Option& socket_option, Network::SocketOptionName option_name, int option_val,
      const std::set<envoy::api::v2::core::SocketOption::SocketState>& when, bool is_v6) {
    for (auto state : when) {
      if (is_v6) {
        EXPECT_CALL(os_sys_calls_, getsockopt_(_, _, IPV6_V6ONLY, _, _));
      }
      if (option_name.has_value()) {
        EXPECT_CALL(os_sys_calls_, setsockopt_(_, option_name.value().first,
                                               option_name.value().second, _, sizeof(int)))
            .WillOnce(Invoke([option_val](int, int, int, const void* optval, socklen_t) -> int {
              EXPECT_EQ(option_val, *static_cast<const int*>(optval));
              return 0;
            }));
        std::cerr << "=========== call setOption()1\n";
        EXPECT_TRUE(socket_option.setOption(socket_, state));
      } else {
        EXPECT_FALSE(socket_option.setOption(socket_, state));
      }
    }

    // The set of SocketOption::SocketState for which this option should not be set.
    // Initialize to all the states, and remove states that are passed in.
    std::list<envoy::api::v2::core::SocketOption::SocketState> unset_socketstates{
        envoy::api::v2::core::SocketOption::STATE_PREBIND,
        envoy::api::v2::core::SocketOption::STATE_BOUND,
        envoy::api::v2::core::SocketOption::STATE_LISTENING,
    };
    unset_socketstates.remove_if(
        [&](envoy::api::v2::core::SocketOption::SocketState state) -> bool {
          return when.find(state) != when.end();
        });
    for (auto state : unset_socketstates) {
      std::cerr << "=========== call setOption()2\n";
      if (is_v6) {
        EXPECT_CALL(os_sys_calls_, getsockopt_(_, _, IPV6_V6ONLY, _, _));
      }
      EXPECT_CALL(os_sys_calls_, setsockopt_(_, _, _, _, _)).Times(0);
      EXPECT_TRUE(socket_option.setOption(socket_, state));
    }
  }
};

} // namespace
} // namespace Network
} // namespace Envoy
