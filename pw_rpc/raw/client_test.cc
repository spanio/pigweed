// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_rpc/client.h"

#include <optional>

#include "gtest/gtest.h"
#include "pw_rpc/internal/client_call.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/raw/client_testing.h"

namespace pw::rpc {

void UnaryMethod();
void BidirectionalStreamMethod();

template <>
struct internal::MethodInfo<UnaryMethod> {
  static constexpr uint32_t kServiceId = 100;
  static constexpr uint32_t kMethodId = 200;
  static constexpr MethodType kType = MethodType::kUnary;
};

template <>
struct internal::MethodInfo<BidirectionalStreamMethod> {
  static constexpr uint32_t kServiceId = 100;
  static constexpr uint32_t kMethodId = 300;
  static constexpr MethodType kType = MethodType::kBidirectionalStreaming;
};

namespace {

template <auto kMethod, typename Call, typename Context>
Call MakeCall(Context& context) {
  return Call(context.client(),
              context.channel().id(),
              internal::MethodInfo<kMethod>::kServiceId,
              internal::MethodInfo<kMethod>::kMethodId,
              internal::MethodInfo<kMethod>::kType);
}

class TestStreamCall : public internal::StreamResponseClientCall {
 public:
  TestStreamCall(Client& client,
                 uint32_t channel_id,
                 uint32_t service_id,
                 uint32_t method_id,
                 MethodType type)
      : StreamResponseClientCall(
            client, channel_id, service_id, method_id, type),
        payload(nullptr) {
    set_on_next([this](ConstByteSpan string) {
      payload = reinterpret_cast<const char*>(string.data());
    });
    set_on_completed([this](Status status) { completed = status; });
    set_on_error([this](Status status) { error = status; });
  }

  const char* payload;
  std::optional<Status> completed;
  std::optional<Status> error;
};

class TestUnaryCall : public internal::UnaryResponseClientCall {
 public:
  TestUnaryCall(Client& client,
                uint32_t channel_id,
                uint32_t service_id,
                uint32_t method_id,
                MethodType type)
      : UnaryResponseClientCall(
            client, channel_id, service_id, method_id, type),
        payload(nullptr) {
    set_on_completed([this](ConstByteSpan string, Status status) {
      payload = reinterpret_cast<const char*>(string.data());
      completed = status;
    });
    set_on_error([this](Status status) { error = status; });
  }

  const char* payload;
  std::optional<Status> completed;
  std::optional<Status> error;
};

TEST(Client, ProcessPacket_InvokesUnaryCallbacks) {
  RawClientTestContext context;
  TestUnaryCall call = MakeCall<UnaryMethod, TestUnaryCall>(context);
  call.SendInitialRequest({});

  ASSERT_NE(call.completed, OkStatus());

  context.server().SendResponse<UnaryMethod>(
      std::as_bytes(std::span("you nary?!?")), OkStatus());

  ASSERT_NE(call.payload, nullptr);
  EXPECT_STREQ(call.payload, "you nary?!?");
  EXPECT_EQ(call.completed, OkStatus());
}

TEST(Client, ProcessPacket_InvokesStreamCallbacks) {
  RawClientTestContext context;
  auto call = MakeCall<BidirectionalStreamMethod, TestStreamCall>(context);
  call.SendInitialRequest({});

  context.server().SendServerStream<BidirectionalStreamMethod>(
      std::as_bytes(std::span("<=>")));

  ASSERT_NE(call.payload, nullptr);
  EXPECT_STREQ(call.payload, "<=>");

  context.server().SendResponse<BidirectionalStreamMethod>(Status::NotFound());

  EXPECT_EQ(call.completed, Status::NotFound());
}

TEST(Client, ProcessPacket_InvokesErrorCallback) {
  RawClientTestContext context;
  auto call = MakeCall<BidirectionalStreamMethod, TestStreamCall>(context);
  call.SendInitialRequest({});

  context.server().SendServerError<BidirectionalStreamMethod>(
      Status::Aborted());

  EXPECT_EQ(call.error, Status::Aborted());
}

TEST(Client, ProcessPacket_SendsClientErrorOnUnregisteredServerStream) {
  RawClientTestContext context;
  context.server().SendServerStream<BidirectionalStreamMethod>({});

  StatusView errors = context.output().errors<BidirectionalStreamMethod>();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(), Status::FailedPrecondition());
}

TEST(Client, ProcessPacket_NonServerStreamOnUnregisteredCall_SendsNothing) {
  RawClientTestContext context;
  context.server().SendServerError<UnaryMethod>(Status::NotFound());
  EXPECT_EQ(context.output().total_packets(), 0u);

  context.server().SendResponse<UnaryMethod>({}, Status::Unavailable());
  EXPECT_EQ(context.output().total_packets(), 0u);
}

TEST(Client, ProcessPacket_ReturnsDataLossOnBadPacket) {
  RawClientTestContext context;

  constexpr std::byte bad_packet[]{
      std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
  EXPECT_EQ(context.client().ProcessPacket(bad_packet), Status::DataLoss());
}

TEST(Client, ProcessPacket_ReturnsInvalidArgumentOnServerPacket) {
  RawClientTestContext context;

  std::byte encoded[64];
  Result<std::span<const std::byte>> result =
      internal::Packet(internal::PacketType::REQUEST, 1, 2, 3).Encode(encoded);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(context.client().ProcessPacket(*result), Status::InvalidArgument());
}

}  // namespace
}  // namespace pw::rpc
