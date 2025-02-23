/*
 * Copyright (C) 2019-2021 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#ifdef DATASERVICE_WRITE_HAS_OPENSSL
#include <openssl/sha.h>
#endif

#include <olp/authentication/TokenProvider.h>
#include <olp/core/cache/DefaultCache.h>
#include <olp/core/client/ApiError.h>
#include <olp/core/client/HRN.h>
#include <olp/core/client/HttpResponse.h>
#include <olp/core/client/OlpClient.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/core/client/OlpClientSettingsFactory.h>
#include <olp/dataservice/write/StreamLayerClient.h>
#include <olp/dataservice/write/model/PublishDataRequest.h>
#include <olp/dataservice/write/model/PublishSdiiRequest.h>
#include <testutils/CustomParameters.hpp>
#include "Utils.h"

const std::string kEndpoint = "endpoint";
const std::string kAppid = "dataservice_write_test_appid";
const std::string kSecret = "dataservice_write_test_secret";
const std::string kCatalog = "dataservice_write_test_catalog";
const std::string kLayer = "layer";

namespace {

namespace write = olp::dataservice::write;
namespace model = olp::dataservice::write::model;

const std::string kBillingTag = "OlpCppSdkTest";

#ifdef DATASERVICE_WRITE_HAS_OPENSSL
// https://stackoverflow.com/a/10632725/275524
std::string sha256(const std::string str) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256_CTX sha256;
  SHA256_Init(&sha256);
  SHA256_Update(&sha256, str.c_str(), str.size());
  SHA256_Final(hash, &sha256);
  std::stringstream ss;
  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
  }
  return ss.str();
}
#endif

std::string GenerateRandomUUID() {
  static boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
}

void PublishDataSuccessAssertions(
    const olp::client::ApiResponse<model::ResponseOkSingle,
                                   olp::client::ApiError>& result) {
  EXPECT_SUCCESS(result);
  EXPECT_FALSE(result.GetResult().GetTraceID().empty());
}

template <typename T>
void PublishFailureAssertions(
    const olp::client::ApiResponse<T, olp::client::ApiError>& result) {
  EXPECT_FALSE(result.IsSuccessful());
  EXPECT_NE(result.GetError().GetHttpStatusCode(),
            olp::http::HttpStatusCode::OK);
}

class DataserviceWriteStreamLayerClientCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(client_ = CreateStreamLayerClient());
    data_ = GenerateData();
  }

  void TearDown() override {
    data_.reset();
    client_.reset();
    if (disk_cache_) {
      disk_cache_->Close();
    }

    // verify that no other thread contains network or task scheduler instance:
    auto network = network_;
    auto scheduler = task_scheduler_;
    network_.reset();
    task_scheduler_.reset();

    ASSERT_EQ(network.use_count(), 1);
    ASSERT_EQ(scheduler.use_count(), 1);
  }

  std::string GetTestCatalog() {
    return CustomParameters::getArgument(kCatalog);
  }

  std::string GetTestLayer() { return CustomParameters::getArgument(kLayer); }

  void QueueMultipleEvents(int num_events) {
    for (int i = 0; i < num_events; i++) {
      data_->push_back(' ');
      data_->push_back(i);
      auto error = client_->Queue(
          model::PublishDataRequest().WithData(data_).WithLayerId(
              GetTestLayer()));
      ASSERT_FALSE(error) << error.get();
    }
  }

  virtual std::shared_ptr<write::StreamLayerClient> CreateStreamLayerClient() {
    network_ = olp::client::OlpClientSettingsFactory::
        CreateDefaultNetworkRequestHandler();
    task_scheduler_ =
        olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1u);

    const auto app_id = CustomParameters::getArgument(kAppid);
    const auto secret = CustomParameters::getArgument(kSecret);

    olp::authentication::Settings authentication_settings({app_id, secret});
    authentication_settings.token_endpoint_url =
        CustomParameters::getArgument(kEndpoint);
    authentication_settings.network_request_handler = network_;

    olp::authentication::TokenProviderDefault provider(authentication_settings);

    olp::client::AuthenticationSettings auth_client_settings;
    auth_client_settings.provider = provider;

    olp::client::OlpClientSettings settings;
    settings.authentication_settings = auth_client_settings;
    settings.network_request_handler = network_;
    settings.task_scheduler = task_scheduler_;

    disk_cache_ = std::make_shared<olp::cache::DefaultCache>();
    EXPECT_EQ(disk_cache_->Open(),
              olp::cache::DefaultCache::StorageOpenResult::Success);
    settings.cache = disk_cache_;

    return std::make_shared<write::StreamLayerClient>(
        olp::client::HRN{GetTestCatalog()}, stream_client_settings_, settings);
  }

 private:
  std::shared_ptr<std::vector<unsigned char>> GenerateData() {
    std::string test_suite_name(testing::UnitTest::GetInstance()
                                    ->current_test_info()
                                    ->test_suite_name());
    std::string test_name(
        testing::UnitTest::GetInstance()->current_test_info()->name());
    std::string data_string(test_suite_name + " " + test_name + " Payload");
    return std::make_shared<std::vector<unsigned char>>(data_string.begin(),
                                                        data_string.end());
  }

 protected:
  std::shared_ptr<write::StreamLayerClient> client_;
  std::shared_ptr<std::vector<unsigned char>> data_;
  std::shared_ptr<olp::http::Network> network_;
  std::shared_ptr<olp::thread::TaskScheduler> task_scheduler_;
  std::shared_ptr<olp::cache::DefaultCache> disk_cache_;
  write::StreamLayerClientSettings stream_client_settings_;
};

TEST_F(DataserviceWriteStreamLayerClientCacheTest, Queue) {
  auto error = client_->Queue(
      model::PublishDataRequest().WithData(data_).WithLayerId(GetTestLayer()));

  ASSERT_FALSE(error) << error.get();
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, QueueNullData) {
  auto error =
      client_->Queue(model::PublishDataRequest().WithData(nullptr).WithLayerId(
          GetTestLayer()));

  ASSERT_TRUE(error);
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, QueueExtraRequestParams) {
  const auto uuid = GenerateRandomUUID();

  auto error = client_->Queue(model::PublishDataRequest()
                                  .WithData(data_)
                                  .WithLayerId(GetTestLayer())
                                  .WithTraceId(uuid)
                                  .WithBillingTag(kBillingTag));

  ASSERT_FALSE(error) << error.get();
}

#ifdef DATASERVICE_WRITE_HAS_OPENSSL
TEST_F(DataserviceWriteStreamLayerClientCacheTest, QueueWithChecksum) {
  const std::string data_string(data_->begin(), data_->end());
  auto checksum = sha256(data_string);

  auto error = client_->Queue(model::PublishDataRequest()
                                  .WithData(data_)
                                  .WithLayerId(GetTestLayer())
                                  .WithChecksum(checksum));

  ASSERT_FALSE(error) << error.get();
}
#endif

TEST_F(DataserviceWriteStreamLayerClientCacheTest, FlushDataSingle) {
  auto error = client_->Queue(
      model::PublishDataRequest().WithData(data_).WithLayerId(GetTestLayer()));

  ASSERT_FALSE(error) << error.get();

  auto response = client_->Flush(model::FlushRequest()).GetFuture().get();

  ASSERT_FALSE(response.empty());
  ASSERT_NO_FATAL_FAILURE(PublishDataSuccessAssertions(response[0]));
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, FlushDataMultiple) {
  ASSERT_NO_FATAL_FAILURE(QueueMultipleEvents(5));

  auto response = client_->Flush(model::FlushRequest()).GetFuture().get();

  ASSERT_EQ(5, response.size());
  for (auto& single_response : response) {
    ASSERT_NO_FATAL_FAILURE(PublishDataSuccessAssertions(single_response));
  }
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, FlushDataSingleAsync) {
  auto error = client_->Queue(
      model::PublishDataRequest().WithData(data_).WithLayerId(GetTestLayer()));

  ASSERT_FALSE(error) << error.get();

  std::promise<write::StreamLayerClient::FlushResponse> response_promise;
  bool call_is_async = true;
  auto cancel_token =
      client_->Flush(model::FlushRequest(),
                     [&](write::StreamLayerClient::FlushResponse response) {
                       call_is_async = false;
                       response_promise.set_value(response);
                     });

  EXPECT_TRUE(call_is_async);
  auto response_future = response_promise.get_future();
  auto status = response_future.wait_for(std::chrono::seconds(30));
  if (status != std::future_status::ready) {
    cancel_token.Cancel();
  }
  auto response = response_future.get();

  ASSERT_FALSE(response.empty());
  ASSERT_NO_FATAL_FAILURE(PublishDataSuccessAssertions(response[0]));
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, FlushDataMultipleAsync) {
  ASSERT_NO_FATAL_FAILURE(QueueMultipleEvents(5));

  std::promise<write::StreamLayerClient::FlushResponse> response_promise;
  bool call_is_async = true;
  auto cancel_token =
      client_->Flush(model::FlushRequest(),
                     [&](write::StreamLayerClient::FlushResponse response) {
                       call_is_async = false;
                       response_promise.set_value(response);
                     });

  EXPECT_TRUE(call_is_async);
  auto response_future = response_promise.get_future();
  auto status = response_future.wait_for(std::chrono::seconds(30));
  if (status != std::future_status::ready) {
    cancel_token.Cancel();
  }
  auto response = response_future.get();

  ASSERT_EQ(5, response.size());
  for (auto& single_response : response) {
    ASSERT_NO_FATAL_FAILURE(PublishDataSuccessAssertions(single_response));
  }
}

TEST_F(DataserviceWriteStreamLayerClientCacheTest, FlushDataCancel) {
  auto error = client_->Queue(
      model::PublishDataRequest().WithData(data_).WithLayerId(GetTestLayer()));

  ASSERT_FALSE(error) << error.get();

  auto cancel_future = client_->Flush(model::FlushRequest());

  std::thread([cancel_future]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel_future.GetCancellationToken().Cancel();
  })
      .detach();

  auto response = cancel_future.GetFuture().get();

  ASSERT_EQ(1, response.size());
  if (response[0].IsSuccessful()) {
    SUCCEED();
    return;
  }

  ASSERT_NO_FATAL_FAILURE(PublishFailureAssertions(response[0]));
}

}  // namespace
