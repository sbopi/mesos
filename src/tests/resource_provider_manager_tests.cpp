// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mesos/http.hpp>
#include <mesos/resources.hpp>

#include <mesos/state/in_memory.hpp>
#include <mesos/state/state.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>

#include <process/ssl/flags.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"

#include "resource_provider/manager.hpp"
#include "resource_provider/registrar.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

namespace http = process::http;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::state::InMemoryStorage;
using mesos::state::State;

using mesos::resource_provider::AdmitResourceProvider;
using mesos::resource_provider::Registrar;
using mesos::resource_provider::RemoveResourceProvider;

using mesos::v1::resource_provider::Call;
using mesos::v1::resource_provider::Driver;
using mesos::v1::resource_provider::Event;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::OK;
using process::http::UnsupportedMediaType;

using std::string;
using std::vector;

using testing::Eq;
using testing::SaveArg;
using testing::Values;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class ResourceProviderManagerHttpApiTest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags slaveFlags = MesosTest::CreateSlaveFlags();

    slaveFlags.authenticate_http_readwrite = false;

    constexpr SlaveInfo::Capability::Type capabilities[] = {
      SlaveInfo::Capability::MULTI_ROLE,
      SlaveInfo::Capability::HIERARCHICAL_ROLE,
      SlaveInfo::Capability::RESERVATION_REFINEMENT,
      SlaveInfo::Capability::RESOURCE_PROVIDER};

    slaveFlags.agent_features = SlaveCapabilities();
    foreach (SlaveInfo::Capability::Type type, capabilities) {
      SlaveInfo::Capability* capability =
        slaveFlags.agent_features->add_capabilities();
      capability->set_type(type);
    }

    return slaveFlags;
  }
};


// The tests are parameterized by the content type of the request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    ResourceProviderManagerHttpApiTest,
    Values(ContentType::PROTOBUF, ContentType::JSON));


TEST_F(ResourceProviderManagerHttpApiTest, NoContentType)
{
  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  ResourceProviderManager manager;

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'Content-Type' to be present",
      response);
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(ResourceProviderManagerHttpApiTest, ValidJsonButInvalidProtobuf)
{
  JSON::Object object;
  object.values["string"] = "valid_json";

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = APPLICATION_JSON;
  request.headers["Content-Type"] = APPLICATION_JSON;
  request.body = stringify(object);

  ResourceProviderManager manager;

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Failed to validate resource_provider::Call: "
      "Expecting 'type' to be present",
      response);
}


TEST_P(ResourceProviderManagerHttpApiTest, MalformedContent)
{
  const ContentType contentType = GetParam();

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = stringify(contentType);
  request.headers["Content-Type"] = stringify(contentType);
  request.body = "MALFORMED_CONTENT";

  ResourceProviderManager manager;

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  switch (contentType) {
    case ContentType::PROTOBUF:
      AWAIT_EXPECT_RESPONSE_BODY_EQ(
          "Failed to parse body into Call protobuf",
          response);
      break;
    case ContentType::JSON:
      AWAIT_EXPECT_RESPONSE_BODY_EQ(
          "Failed to parse body into JSON: "
          "syntax error at line 1 near: MALFORMED_CONTENT",
          response);
      break;
    case ContentType::RECORDIO:
      break;
  }
}


TEST_P(ResourceProviderManagerHttpApiTest, UnsupportedContentMediaType)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();

  mesos::v1::ResourceProviderInfo* info =
    subscribe->mutable_resource_provider_info();

  info->set_type("org.apache.mesos.rp.test");
  info->set_name("test");

  const ContentType contentType = GetParam();
  const string unknownMediaType = "application/unknown-media-type";

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = stringify(contentType);
  request.headers["Content-Type"] = unknownMediaType;
  request.body = serialize(contentType, call);

  ResourceProviderManager manager;

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);
}


TEST_P(ResourceProviderManagerHttpApiTest, UpdateState)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager;

  Option<UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<UUID> uuid = UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    recordio::Reader<Event> responseDecoder(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, update the total resources to the manager.
  {
    std::vector<v1::Resource> resources =
      v1::Resources::fromString("disk:4").get();
    foreach (v1::Resource& resource, resources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());
    }

    Call call;
    call.set_type(Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdateState* updateState = call.mutable_update_state();

    updateState->mutable_resources()->CopyFrom(v1::Resources(resources));
    updateState->set_resource_version_uuid(UUID::random().toBytes());

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    // The manager will send out a message informing its subscriber
    // about the newly added resources.
    Future<ResourceProviderMessage> message = manager.messages().get();

    AWAIT_READY(message);

    EXPECT_EQ(ResourceProviderMessage::Type::UPDATE_STATE, message->type);
    EXPECT_EQ(devolve(resourceProviderId.get()), message->updateState->id);
    EXPECT_EQ(devolve(resources), message->updateState->total);
  }
}


TEST_P(ResourceProviderManagerHttpApiTest, UpdateOfferOperationStatus)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager;

  Option<UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<UUID> uuid = UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    recordio::Reader<Event> responseDecoder(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, send an offer operation update to the manager.
  {
    v1::FrameworkID frameworkId;
    frameworkId.set_value("foo");

    mesos::v1::OfferOperationStatus status;
    status.set_state(mesos::v1::OfferOperationState::OFFER_OPERATION_FINISHED);

    UUID operationUUID = UUID::random();

    Call call;
    call.set_type(Call::UPDATE_OFFER_OPERATION_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdateOfferOperationStatus* updateOfferOperationStatus =
      call.mutable_update_offer_operation_status();
    updateOfferOperationStatus->mutable_framework_id()->CopyFrom(frameworkId);
    updateOfferOperationStatus->mutable_status()->CopyFrom(status);
    updateOfferOperationStatus->set_operation_uuid(operationUUID.toBytes());

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    // The manager will send out a message informing its subscriber
    // about the updated offer operation.
    Future<ResourceProviderMessage> message = manager.messages().get();

    AWAIT_READY(message);

    EXPECT_EQ(
        ResourceProviderMessage::Type::UPDATE_OFFER_OPERATION_STATUS,
        message->type);
    EXPECT_EQ(
        devolve(frameworkId),
        message->updateOfferOperationStatus->update.framework_id());
    EXPECT_EQ(
        devolve(status).state(),
        message->updateOfferOperationStatus->update.status().state());
    EXPECT_EQ(
        operationUUID.toBytes(),
        message->updateOfferOperationStatus->update.operation_uuid());
  }
}


// This test starts an agent and connects directly with its resource
// provider endpoint.
TEST_P(ResourceProviderManagerHttpApiTest, AgentEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  AWAIT_READY(__recover);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();

  mesos::v1::ResourceProviderInfo* info =
    subscribe->mutable_resource_provider_info();

  info->set_type("org.apache.mesos.rp.test");
  info->set_name("test");

  const ContentType contentType = GetParam();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
      agent.get()->pid,
      "api/v1/resource_provider",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);

  Option<http::Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  recordio::Reader<Event> responseDecoder(
      ::recordio::Decoder<Event>(
          lambda::bind(deserialize<Event>, contentType, lambda::_1)),
      reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the resource provider id is set.
  EXPECT_EQ(Event::SUBSCRIBED, event->get().type());
  EXPECT_FALSE(event->get().subscribed().provider_id().value().empty());
}


class ResourceProviderRegistrarTest : public tests::MesosTest {};


// Test that the agent resource provider registrar works as expected.
TEST_F(ResourceProviderRegistrarTest, AgentRegistrar)
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("foo");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  const slave::Flags flags = CreateSlaveFlags();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // The agent will send `UpdateSlaveMessage` after it has created its
  // meta directories. Await the message to make sure the agent
  // registrar can create its store in the meta hierarchy.
  AWAIT_READY(updateSlaveMessage);

  Try<Owned<Registrar>> registrar =
    Registrar::create(flags, slaveRegisteredMessage->slave_id());

  ASSERT_SOME(registrar);
  ASSERT_NE(nullptr, registrar->get());

  // Applying operations on a not yet recovered registrar fails.
  AWAIT_FAILED(registrar.get()->apply(Owned<Registrar::Operation>(
      new AdmitResourceProvider(resourceProviderId))));

  AWAIT_READY(registrar.get()->recover());

  AWAIT_READY(registrar.get()->apply(Owned<Registrar::Operation>(
      new AdmitResourceProvider(resourceProviderId))));

  AWAIT_READY(registrar.get()->apply(Owned<Registrar::Operation>(
      new RemoveResourceProvider(resourceProviderId))));
}


// Test that the master resource provider registrar works as expected.
TEST_F(ResourceProviderRegistrarTest, MasterRegistrar)
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("foo");

  InMemoryStorage storage;
  State state(&storage);
  master::Registrar masterRegistrar(CreateMasterFlags(), &state);

  const MasterInfo masterInfo = protobuf::createMasterInfo({});

  Try<Owned<Registrar>> registrar = Registrar::create(&masterRegistrar);

  ASSERT_SOME(registrar);
  ASSERT_NE(nullptr, registrar->get());

  // Applying operations on a not yet recovered registrar fails.
  AWAIT_FAILED(registrar.get()->apply(Owned<Registrar::Operation>(
      new AdmitResourceProvider(resourceProviderId))));

  AWAIT_READY(masterRegistrar.recover(masterInfo));

  AWAIT_READY(registrar.get()->apply(Owned<Registrar::Operation>(
      new AdmitResourceProvider(resourceProviderId))));

  AWAIT_READY(registrar.get()->apply(Owned<Registrar::Operation>(
      new RemoveResourceProvider(resourceProviderId))));
}


// Test that resource provider resources are offered to frameworks,
// frameworks can accept the offer with an operation that has a resource
// provider convert resources and that the converted resources are
// offered to frameworks as well.
TEST_P(ResourceProviderManagerHttpApiTest, ConvertResources)
{
  // Start master and agent.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Pause the clock and control it manually in order to
  // control the timing of the registration. A registration timeout
  // would trigger multiple registration attempts. As a result, multiple
  // 'UpdateSlaveMessage' would be sent, which we need to avoid to
  // ensure that the second 'UpdateSlaveMessage' is a result of the
  // resource provider registration.
  Clock::pause();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Clock::resume();

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::MockResourceProvider resourceProvider(
      resourceProviderInfo, Some(v1::Resources(disk)));

  // Start and register a resource provider.
  string scheme = "http";

#ifdef USE_SSL_SOCKET
  if (process::network::openssl::flags().enabled) {
    scheme = "https";
  }
#endif

  http::URL url(
      scheme,
      agent.get()->pid.address.ip,
      agent.get()->pid.address.port,
      agent.get()->pid.id + "/api/v1/resource_provider");

  Owned<EndpointDetector> endpointDetector(new ConstantEndpointDetector(url));

  const ContentType contentType = GetParam();

  resourceProvider.start(endpointDetector, contentType, v1::DEFAULT_CREDENTIAL);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a framework.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_)).WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Option<v1::FrameworkID> frameworkId;
  Option<mesos::v1::Offer> offer1;

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");

  {
    Future<v1::scheduler::Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    Future<v1::scheduler::Event::Offers> offers;
    EXPECT_CALL(*scheduler, offers(_, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    mesos::v1::scheduler::Call call;
    call.set_type(mesos::v1::scheduler::Call::SUBSCRIBE);
    mesos::v1::scheduler::Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    mesos.send(call);

    AWAIT_READY(subscribed);

    frameworkId = subscribed->framework_id();

    // Resource provider resources will be offered to the framework.
    AWAIT_READY(offers);
    EXPECT_FALSE(offers->offers().empty());
    offer1 = offers->offers(0);
  }

  v1::Resources resources =
    v1::Resources(offer1->resources()).filter([](const v1::Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  v1::Resource reserved = *(resources.begin());
  reserved.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.role(), DEFAULT_CREDENTIAL.principal()));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  mesos.send(
      v1::createCallAccept(
          frameworkId.get(),
          offer1.get(),
          {v1::RESERVE(reserved),
           v1::CREATE_BLOCK(reserved)}));

  // The converted resource should be offered to the framework.
  AWAIT_READY(offers);
  EXPECT_FALSE(offers->offers().empty());

  const v1::Offer& offer2 = offers->offers(0);

  Option<v1::Resource> block;
  foreach (const v1::Resource& resource, offer2.resources()) {
    if (resource.has_provider_id()) {
      block = resource;
    }
  }

  ASSERT_SOME(block);
  EXPECT_EQ(
      v1::Resource::DiskInfo::Source::BLOCK, block->disk().source().type());
  EXPECT_FALSE(block->reservations().empty());
}


// Test that resource provider can resubscribe with an agent after
// a resource provider failover as well as an agent failover.
TEST_P(ResourceProviderManagerHttpApiTest, ResubscribeResourceProvider)
{
  Clock::pause();

  // Start master and agent.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  v1::MockResourceProvider resourceProvider(
      resourceProviderInfo, Some(v1::Resources(disk)));

  // Start and register a resource provider.
  string scheme = "http";

#ifdef USE_SSL_SOCKET
  if (process::network::openssl::flags().enabled) {
    scheme = "https";
  }
#endif

  http::URL url(
      scheme,
      agent.get()->pid.address.ip,
      agent.get()->pid.address.port,
      agent.get()->pid.id + "/api/v1/resource_provider");

  Owned<EndpointDetector> endpointDetector(new ConstantEndpointDetector(url));

  const ContentType contentType = GetParam();

  resourceProvider.start(endpointDetector, contentType, v1::DEFAULT_CREDENTIAL);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources. At this point the resource provider
  // will have an ID assigned by the agent.
  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderID resourceProviderId = resourceProvider.info.id();

  Future<Event::Subscribed> subscribed1;
  EXPECT_CALL(resourceProvider, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed1));

  // Resource provider failover by opening a new connection.
  // The assigned resource provider ID will be used to resubscribe.
  resourceProvider.start(endpointDetector, contentType, v1::DEFAULT_CREDENTIAL);

  AWAIT_READY(subscribed1);
  EXPECT_EQ(resourceProviderId, subscribed1->provider_id());

  Future<Event::Subscribed> subscribed2;
  EXPECT_CALL(resourceProvider, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed2));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // The agent failover.
  agent->reset();
  agent = StartSlave(detector.get(), slaveFlags);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(__recover);

  url = http::URL(
      scheme,
      agent.get()->pid.address.ip,
      agent.get()->pid.address.port,
      agent.get()->pid.id + "/api/v1/resource_provider");

  endpointDetector.reset(new ConstantEndpointDetector(url));

  resourceProvider.start(endpointDetector, contentType, v1::DEFAULT_CREDENTIAL);

  AWAIT_READY(subscribed2);
  EXPECT_EQ(resourceProviderId, subscribed2->provider_id());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
