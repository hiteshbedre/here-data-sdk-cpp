/*
 * Copyright (C) 2019-2020 HERE Europe B.V.
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
#include <matchers/NetworkUrlMatchers.h>
#include <mocks/CacheMock.h>
#include <mocks/NetworkMock.h>

#include <olp/core/cache/CacheSettings.h>
#include <olp/core/cache/DefaultCache.h>
#include <olp/core/cache/KeyValueCache.h>
#include <olp/core/client/OlpClientSettingsFactory.h>
#include <olp/core/utils/Dir.h>
#include <olp/dataservice/read/VersionedLayerClient.h>
#include "ApiDefaultResponses.h"
#include "ReadDefaultResponses.h"
#include "UrlGenerators.h"
#include "VersionedLayerClientImpl.h"
#include "repositories/QuadTreeIndex.h"
// clang-format off
#include "generated/serializer/ApiSerializer.h"
#include "generated/serializer/VersionResponseSerializer.h"
#include "generated/serializer/PartitionsSerializer.h"
#include "generated/serializer/JsonSerializer.h"
// clang-format on

namespace {
namespace read = olp::dataservice::read;
namespace model = olp::dataservice::read::model;
using ::testing::_;
using ::testing::Mock;

const std::string kCatalog =
    "hrn:here:data::olp-here-test:hereos-internal-test-v2";
const std::string kLayerId = "testlayer";
const auto kHrn = olp::client::HRN::FromString(kCatalog);
const auto kPartitionId = "269";
const auto kCatalogVersion = 108;
const auto kTimeout = std::chrono::seconds(5);
constexpr auto kBlobDataHandle = R"(4eed6ed1-0d32-43b9-ae79-043cb4256432)";
constexpr auto kHereTile = "23618364";
constexpr auto kOtherHereTile = "1476147";
constexpr auto kOtherHereTile2 = "5904591";
constexpr auto kUrlLookup =
    R"(https://api-lookup.data.api.platform.here.com/lookup/v1/resources/hrn:here:data::olp-here-test:hereos-internal-test-v2/apis)";

template <class T>
std::string serialize(std::vector<T> data) {
  std::string str = "[";
  for (const auto& el : data) {
    str.append(olp::serializer::serialize(el));
    str.append(",");
  }
  str[str.length() - 1] = ']';
  return str;
}

TEST(VersionedLayerClientTest, CanBeMoved) {
  read::VersionedLayerClient client_a(olp::client::HRN(), "", boost::none, {});
  read::VersionedLayerClient client_b(std::move(client_a));
  read::VersionedLayerClient client_c(olp::client::HRN(), "", boost::none, {});
  client_c = std::move(client_b);
}

TEST(VersionedLayerClientTest, GetData) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  std::shared_ptr<CacheMock> cache_mock = std::make_shared<CacheMock>();
  olp::client::OlpClientSettings settings;
  settings.network_request_handler = network_mock;
  settings.cache = cache_mock;

  read::VersionedLayerClient client(kHrn, kLayerId, boost::none, settings);
  {
    SCOPED_TRACE("Get Data with PartitionId and DataHandle");
    std::promise<read::DataResponse> promise;
    std::future<read::DataResponse> future = promise.get_future();

    auto token = client.GetData(
        read::DataRequest()
            .WithPartitionId(kPartitionId)
            .WithDataHandle(kBlobDataHandle),
        [&](read::DataResponse response) { promise.set_value(response); });

    EXPECT_EQ(future.wait_for(kTimeout), std::future_status::ready);

    const auto& response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::PreconditionFailed);
  }
  Mock::VerifyAndClearExpectations(network_mock.get());
}

TEST(VersionedLayerClientTest, RemoveFromCachePartition) {
  olp::client::OlpClientSettings settings;
  std::shared_ptr<CacheMock> cache_mock = std::make_shared<CacheMock>();
  settings.cache = cache_mock;

  // successfull mock cache calls
  auto found_cache_response = [](const std::string& /*key*/,
                                 const olp::cache::Decoder& /*encoder*/) {
    auto partition = model::Partition();
    partition.SetPartition(kPartitionId);
    partition.SetDataHandle(kBlobDataHandle);
    return partition;
  };
  auto partition_cache_remove = [&](const std::string& prefix) {
    std::string expected_prefix =
        kHrn.ToCatalogHRNString() + "::" + kLayerId + "::" + kPartitionId +
        "::" + std::to_string(kCatalogVersion) + "::partition";
    EXPECT_EQ(prefix, expected_prefix);
    return true;
  };
  auto data_cache_remove = [&](const std::string& prefix) {
    std::string expected_prefix = kHrn.ToCatalogHRNString() + "::" + kLayerId +
                                  "::" + kBlobDataHandle + "::Data";
    EXPECT_EQ(prefix, expected_prefix);
    return true;
  };

  read::VersionedLayerClient client(kHrn, kLayerId, kCatalogVersion, settings);
  {
    SCOPED_TRACE("Successfull remove partition from cache");

    EXPECT_CALL(*cache_mock, Get(_, _)).WillOnce(found_cache_response);
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce(partition_cache_remove)
        .WillOnce(data_cache_remove);
    ASSERT_TRUE(client.RemoveFromCache(kPartitionId));
  }
  {
    SCOPED_TRACE("Remove not existing partition from cache");
    EXPECT_CALL(*cache_mock, Get(_, _))
        .WillOnce([](const std::string&, const olp::cache::Decoder&) {
          return boost::any();
        });
    ASSERT_TRUE(client.RemoveFromCache(kPartitionId));
  }
  {
    SCOPED_TRACE("Partition cache failure");
    EXPECT_CALL(*cache_mock, Get(_, _)).WillOnce(found_cache_response);
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce([](const std::string&) { return false; });
    ASSERT_FALSE(client.RemoveFromCache(kPartitionId));
  }
  {
    SCOPED_TRACE("Data cache failure");
    EXPECT_CALL(*cache_mock, Get(_, _)).WillOnce(found_cache_response);
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce(partition_cache_remove)
        .WillOnce([](const std::string&) { return false; });
    ASSERT_FALSE(client.RemoveFromCache(kPartitionId));
  }
}

TEST(VersionedLayerClientTest, RemoveFromCacheTileKey) {
  olp::client::OlpClientSettings settings;
  std::shared_ptr<CacheMock> cache_mock = std::make_shared<CacheMock>();
  settings.cache = cache_mock;

  auto depth = 4;
  auto tile_key = olp::geo::TileKey::FromHereTile(kHereTile);
  auto root = tile_key.ChangedLevelBy(-depth);

  auto stream = std::stringstream(
      mockserver::ReadDefaultResponses::GenerateQuadTreeResponse(
          root, depth, {9, 10, 11, 12}));
  read::QuadTreeIndex quad_tree(root, depth, stream);
  auto buffer = quad_tree.GetRawData();

  auto quad_cache_key = [&depth](const olp::geo::TileKey& key) {
    return kHrn.ToCatalogHRNString() + "::" + kLayerId +
           "::" + key.ToHereTile() + "::" + std::to_string(kCatalogVersion) +
           "::" + std::to_string(depth) + "::quadtree";
  };

  auto data_cache_remove = [&](const std::string& prefix) {
    std::string expected_prefix =
        kHrn.ToCatalogHRNString() + "::" + kLayerId +
        "::" + mockserver::ReadDefaultResponses::GenerateDataHandle(kHereTile) +
        "::Data";
    EXPECT_EQ(prefix, expected_prefix);
    return true;
  };

  read::VersionedLayerClient client(kHrn, kLayerId, kCatalogVersion, settings);
  {
    SCOPED_TRACE("Successfull remove tile from cache");

    EXPECT_CALL(*cache_mock, Get(_))
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-1)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-2)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-3)));
          return nullptr;
        })
        .WillOnce(
            [&tile_key, &quad_cache_key, &buffer](const std::string& key) {
              EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-4)));
              return buffer;
            });
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce(data_cache_remove);
    EXPECT_CALL(*cache_mock, Contains(_))
        .WillRepeatedly([&](const std::string&) { return true; });
    ASSERT_TRUE(client.RemoveFromCache(tile_key));
  }

  {
    SCOPED_TRACE("Remove not existing tile from cache");
    EXPECT_CALL(*cache_mock, Get(_))
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-1)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-2)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-3)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-4)));
          return nullptr;
        });
    ASSERT_TRUE(client.RemoveFromCache(tile_key));
  }
  {
    SCOPED_TRACE("Data cache failure");
    EXPECT_CALL(*cache_mock, Get(_))
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-1)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-2)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-3)));
          return nullptr;
        })
        .WillOnce(
            [&tile_key, &quad_cache_key, &buffer](const std::string& key) {
              EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-4)));
              return buffer;
            });
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce([](const std::string&) { return false; });
    ASSERT_FALSE(client.RemoveFromCache(tile_key));
  }
  {
    SCOPED_TRACE("Successfull remove tile and quad tree from cache");
    EXPECT_CALL(*cache_mock, Get(_))
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-1)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-2)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-3)));
          return nullptr;
        })
        .WillOnce(
            [&tile_key, &quad_cache_key, &buffer](const std::string& key) {
              EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-4)));
              return buffer;
            });
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce(data_cache_remove)
        .WillOnce([&](const std::string&) { return true; });
    EXPECT_CALL(*cache_mock, Contains(_))
        .WillRepeatedly([&](const std::string&) { return false; });
    ASSERT_TRUE(client.RemoveFromCache(tile_key));
  }
  {
    SCOPED_TRACE("Successfull remove tile but removing quad tree fails");
    EXPECT_CALL(*cache_mock, Get(_))
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-1)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-2)));
          return nullptr;
        })
        .WillOnce([&tile_key, &quad_cache_key](const std::string& key) {
          EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-3)));
          return nullptr;
        })
        .WillOnce(
            [&tile_key, &quad_cache_key, &buffer](const std::string& key) {
              EXPECT_EQ(key, quad_cache_key(tile_key.ChangedLevelBy(-4)));
              return buffer;
            });
    EXPECT_CALL(*cache_mock, RemoveKeysWithPrefix(_))
        .WillOnce(data_cache_remove)
        .WillOnce([&](const std::string&) { return false; });
    EXPECT_CALL(*cache_mock, Contains(_))
        .WillRepeatedly([&](const std::string&) { return false; });
    ASSERT_FALSE(client.RemoveFromCache(tile_key));
  }
}

TEST(VersionedLayerClientTest, ProtectThanRelease) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  olp::cache::CacheSettings cache_settings;
  cache_settings.disk_path_mutable =
      olp::utils::Dir::TempDirectory() + "/unittest";
  auto cache =
      std::make_shared<olp::cache::DefaultCache>(std::move(cache_settings));
  cache->Open();
  cache->Clear();
  olp::client::OlpClientSettings settings;
  settings.cache = cache;
  settings.default_cache_expiration = std::chrono::seconds(2);
  settings.network_request_handler = network_mock;
  auto version = 4u;
  auto api_response =
      mockserver::ApiDefaultResponses::GenerateResourceApisResponse(kCatalog);
  auto quad_path = mock::GeneratePath(
      api_response, "query",
      mock::GenerateGetQuadKeyPath("92259", kLayerId, version, 4));
  ASSERT_FALSE(quad_path.empty());
  auto tile_key = olp::geo::TileKey::FromHereTile(kHereTile);
  auto responce_quad =
      mockserver::ReadDefaultResponses::GenerateQuadTreeResponse(
          tile_key.ChangedLevelBy(-4), 4, {9, 10, 11, 12});
  auto tile_path = mock::GeneratePath(
      api_response, "blob",
      mock::GenerateGetDataPath(
          kLayerId,
          mockserver::ReadDefaultResponses::GenerateDataHandle(kHereTile)));
  ASSERT_FALSE(tile_path.empty());
  auto tile2_path = mock::GeneratePath(
      api_response, "blob",
      mock::GenerateGetDataPath(
          kLayerId, mockserver::ReadDefaultResponses::GenerateDataHandle(
                        kOtherHereTile2)));
  ASSERT_FALSE(tile2_path.empty());
  auto other_tile_path = mock::GeneratePath(
      api_response, "blob",
      mock::GenerateGetDataPath(
          kLayerId, mockserver::ReadDefaultResponses::GenerateDataHandle(
                        kOtherHereTile)));
  ASSERT_FALSE(other_tile_path.empty());

  read::VersionedLayerClientImpl client(kHrn, kLayerId, boost::none, settings);
  {
    SCOPED_TRACE("Cache tile key");

    EXPECT_CALL(*network_mock, Send(IsGetRequest(kUrlLookup), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     serialize(api_response)));
    auto version_path = mock::GeneratePath(
        api_response, "metadata", mock::GenerateGetLatestVersionPath());
    ASSERT_FALSE(version_path.empty());
    EXPECT_CALL(*network_mock, Send(IsGetRequest(version_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(
                mockserver::ReadDefaultResponses::GenerateVersionResponse(
                    version))));

    EXPECT_CALL(*network_mock, Send(IsGetRequest(quad_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     responce_quad));
    EXPECT_CALL(*network_mock, Send(IsGetRequest(tile_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "data"));

    auto future =
        client.GetData(read::TileRequest().WithTileKey(tile_key)).GetFuture();

    const auto& response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
  }

  {
    SCOPED_TRACE("Cache tile other key");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);

    EXPECT_CALL(*network_mock, Send(IsGetRequest(other_tile_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "data"));

    auto future =
        client.GetData(read::TileRequest().WithTileKey(other_tile_key))
            .GetFuture();

    const auto& response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
  }
  {
    SCOPED_TRACE("Protect");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);
    auto response = client.Protect({tile_key, other_tile_key});
    ASSERT_TRUE(response);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ASSERT_TRUE(client.IsCached(tile_key));
    ASSERT_TRUE(client.IsCached(other_tile_key));
  }

  {
    SCOPED_TRACE("Protect tile which not in cache but has known data handle");
    auto tile_key2 = olp::geo::TileKey::FromHereTile(kOtherHereTile2);
    auto response = client.Protect({tile_key2});
    ASSERT_TRUE(response);
    ASSERT_FALSE(client.IsCached(tile_key2));

    // now get protected tile
    EXPECT_CALL(*network_mock, Send(IsGetRequest(tile2_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "data"));

    auto data_future =
        client.GetData(read::TileRequest().WithTileKey(tile_key2)).GetFuture();

    const auto& data_response = data_future.get();
    ASSERT_TRUE(data_response.IsSuccessful());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    // tile stays in cache, as it was protected before
    ASSERT_TRUE(client.IsCached(tile_key2));
  }
  {
    SCOPED_TRACE("Protect tile which not in cache");
    auto some_tile_key = olp::geo::TileKey::FromHereTile("6904592");

    auto response = client.Protect({some_tile_key});
    ASSERT_FALSE(response);
  }
  {
    SCOPED_TRACE("Release tiles without releasing quad tree");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);
    auto other_tile_key2 = olp::geo::TileKey::FromHereTile(kOtherHereTile2);
    auto response = client.Release({tile_key, other_tile_key2});
    ASSERT_TRUE(response);
    ASSERT_FALSE(client.IsCached(tile_key));
    // other_tile_key is still protected, quad tree should ce in cache
    ASSERT_TRUE(client.IsCached(other_tile_key));
  }
  {
    SCOPED_TRACE("Release last protected tile with quad tree");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);
    // release last protected tile for quad
    // 2 keys should be released(tile and quad)
    auto response = client.Release({other_tile_key});
    ASSERT_TRUE(response);
    ASSERT_FALSE(client.IsCached(other_tile_key));
  }
  {
    SCOPED_TRACE("Release not protected tile");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);
    // release last protected tile for quad
    // 2 keys should be released(tile and quad)
    auto response = client.Release({other_tile_key});
    ASSERT_FALSE(response);
  }
  {
    SCOPED_TRACE("Protect and release keys within one quad");
    auto other_tile_key = olp::geo::TileKey::FromHereTile(kOtherHereTile);

    EXPECT_CALL(*network_mock, Send(IsGetRequest(quad_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     responce_quad));
    EXPECT_CALL(*network_mock, Send(IsGetRequest(tile_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "data"));

    EXPECT_CALL(*network_mock, Send(IsGetRequest(other_tile_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "data"));
    auto future =
        client.GetData(read::TileRequest().WithTileKey(tile_key)).GetFuture();

    const auto& response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
    future = client.GetData(read::TileRequest().WithTileKey(other_tile_key))
                 .GetFuture();
    const auto& response_other = future.get();
    ASSERT_TRUE(response_other.IsSuccessful());

    auto protect_response = client.Protect({tile_key, other_tile_key});
    ASSERT_TRUE(protect_response);
    ASSERT_TRUE(client.IsCached(tile_key));
    ASSERT_TRUE(client.IsCached(other_tile_key));

    auto release_response = client.Release({tile_key, other_tile_key});
    ASSERT_TRUE(release_response);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ASSERT_FALSE(client.IsCached(tile_key));
    ASSERT_FALSE(client.IsCached(other_tile_key));
  }
  ASSERT_TRUE(cache->Clear());
  Mock::VerifyAndClearExpectations(network_mock.get());
}

TEST(VersionedLayerClientTest, PrefetchPartitionsSplitted) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  olp::client::OlpClientSettings settings;
  settings.network_request_handler = network_mock;
  auto version = 4u;

  auto partitions_count = 200u;
  std::vector<std::string> partitions1;
  std::vector<std::string> partitions2;

  for (auto i = 0u; i < partitions_count / 2; i++) {
    partitions1.emplace_back(std::to_string(i));
  }
  for (auto i = partitions_count / 2; i < partitions_count; i++) {
    partitions2.emplace_back(std::to_string(i));
  }
  std::vector<std::string> partitions = partitions1;
  partitions.insert(partitions.end(), partitions2.begin(), partitions2.end());

  read::VersionedLayerClientImpl client(kHrn, kLayerId, boost::none, settings);
  {
    SCOPED_TRACE("Prefetch multiple partitions");

    auto api_response =
        mockserver::ApiDefaultResponses::GenerateResourceApisResponse(kCatalog);
    auto partitions_response1 =
        mockserver::ReadDefaultResponses::GeneratePartitionsResponse(
            partitions_count / 2);
    auto partitions_response2 =
        mockserver::ReadDefaultResponses::GeneratePartitionsResponse(
            partitions_count / 2, partitions_count / 2);

    EXPECT_CALL(*network_mock, Send(IsGetRequest(kUrlLookup), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     serialize(api_response)));

    auto version_path = mock::GeneratePath(
        api_response, "metadata", mock::GenerateGetLatestVersionPath());
    ASSERT_FALSE(version_path.empty());

    EXPECT_CALL(*network_mock, Send(IsGetRequest(version_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(
                mockserver::ReadDefaultResponses::GenerateVersionResponse(
                    version))));

    auto partitions_path1 = mock::GeneratePath(
        api_response, "query",
        mock::GenerateGetPartitionsPath(kLayerId, partitions1, version));
    ASSERT_FALSE(partitions_path1.empty());
    auto partitions_path2 = mock::GeneratePath(
        api_response, "query",
        mock::GenerateGetPartitionsPath(kLayerId, partitions2, version));
    ASSERT_FALSE(partitions_path2.empty());

    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path1), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(partitions_response1)));
    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path2), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(partitions_response2)));

    for (const auto& partition : partitions_response1.GetPartitions()) {
      auto partition_path = mock::GeneratePath(
          api_response, "blob",
          mock::GenerateGetDataPath(kLayerId, partition.GetDataHandle()));
      ASSERT_FALSE(partition_path.empty());

      EXPECT_CALL(*network_mock, Send(IsGetRequest(partition_path), _, _, _, _))
          .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                           olp::http::HttpStatusCode::OK),
                                       "data"));
    }

    for (const auto& partition : partitions_response2.GetPartitions()) {
      auto partition_path = mock::GeneratePath(
          api_response, "blob",
          mock::GenerateGetDataPath(kLayerId, partition.GetDataHandle()));
      ASSERT_FALSE(partition_path.empty());

      EXPECT_CALL(*network_mock, Send(IsGetRequest(partition_path), _, _, _, _))
          .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                           olp::http::HttpStatusCode::OK),
                                       "data"));
    }

    const auto request =
        olp::dataservice::read::PrefetchPartitionsRequest().WithPartitionIds(
            partitions);

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
    const auto result = response.MoveResult();

    ASSERT_EQ(result.GetPartitions().size(), partitions_count);

    for (const auto& partition : result.GetPartitions()) {
      ASSERT_TRUE(client.IsCached(partition));
    }
  }
  {
    SCOPED_TRACE("Prefetch cached partitions");
    const auto request =
        olp::dataservice::read::PrefetchPartitionsRequest().WithPartitionIds(
            partitions);
    auto future = client.PrefetchPartitions(request, nullptr).GetFuture();
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
    const auto result = response.MoveResult();

    ASSERT_EQ(result.GetPartitions().size(), partitions_count);

    for (const auto& partition : result.GetPartitions()) {
      ASSERT_TRUE(client.IsCached(partition));
    }
  }
  Mock::VerifyAndClearExpectations(network_mock.get());
}

TEST(VersionedLayerClientTest, PrefetchPartitionsSomeFail) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  olp::client::OlpClientSettings settings;
  settings.network_request_handler = network_mock;
  auto version = 4u;

  auto partitions_count = 5u;
  std::vector<std::string> partitions;
  partitions.reserve(partitions_count);
  for (auto i = 0u; i < partitions_count; i++) {
    partitions.emplace_back(std::to_string(i));
  }
  auto api_response =
      mockserver::ApiDefaultResponses::GenerateResourceApisResponse(kCatalog);
  auto partitions_response =
      mockserver::ReadDefaultResponses::GeneratePartitionsResponse(
          partitions_count);
  const auto request =
      olp::dataservice::read::PrefetchPartitionsRequest().WithPartitionIds(
          partitions);
  read::VersionedLayerClientImpl client(kHrn, kLayerId, boost::none, settings);
  auto partitions_path = mock::GeneratePath(
      api_response, "query",
      mock::GenerateGetPartitionsPath(kLayerId, partitions, version));
  ASSERT_FALSE(partitions_path.empty());
  {
    SCOPED_TRACE("Prefetch partitions, some fails");

    EXPECT_CALL(*network_mock, Send(IsGetRequest(kUrlLookup), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     serialize(api_response)));

    auto version_path = mock::GeneratePath(
        api_response, "metadata", mock::GenerateGetLatestVersionPath());
    ASSERT_FALSE(version_path.empty());

    EXPECT_CALL(*network_mock, Send(IsGetRequest(version_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(
                mockserver::ReadDefaultResponses::GenerateVersionResponse(
                    version))));

    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse()
                .WithBytesDownloaded(10ull)
                .WithBytesUploaded(5ull)
                .WithStatus(olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(partitions_response)));
    for (auto i = 0u; i < partitions_response.GetPartitions().size(); i++) {
      const auto& partition = partitions_response.GetPartitions().at(i);
      auto partition_path = mock::GeneratePath(
          api_response, "blob",
          mock::GenerateGetDataPath(kLayerId, partition.GetDataHandle()));
      ASSERT_FALSE(partition_path.empty());

      EXPECT_CALL(*network_mock, Send(IsGetRequest(partition_path), _, _, _, _))
          .WillOnce(ReturnHttpResponse(
              olp::http::NetworkResponse()
                  .WithBytesDownloaded(2ull)
                  .WithBytesUploaded(1ull)
                  .WithStatus((i == 0 ? olp::http::HttpStatusCode::OK
                                      : olp::http::HttpStatusCode::NOT_FOUND)),
              "data"));
    }

    olp::dataservice::read::PrefetchPartitionsStatus statistic;
    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        [&](olp::dataservice::read::PrefetchPartitionsStatus status) {
          statistic = std::move(status);
        });
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_TRUE(response.IsSuccessful());
    ASSERT_EQ(statistic.bytes_transferred, 15 + 5 * 3);
    ASSERT_EQ(statistic.total_partitions_to_prefetch, partitions_count);
    ASSERT_EQ(statistic.prefetched_partitions, partitions_count);
    const auto result = response.MoveResult();
    // only 1 partition downloaded
    ASSERT_EQ(result.GetPartitions().size(), 1u);
    for (const auto& partition : result.GetPartitions()) {
      ASSERT_TRUE(client.IsCached(partition));
      ASSERT_TRUE(client.RemoveFromCache(partition));
    }
  }
  {
    SCOPED_TRACE("Prefetch partitions, all fails");

    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(partitions_response)));

    for (auto i = 0u; i < partitions_response.GetPartitions().size(); i++) {
      const auto& partition = partitions_response.GetPartitions().at(i);
      auto partition_path = mock::GeneratePath(
          api_response, "blob",
          mock::GenerateGetDataPath(kLayerId, partition.GetDataHandle()));
      ASSERT_FALSE(partition_path.empty());

      EXPECT_CALL(*network_mock, Send(IsGetRequest(partition_path), _, _, _, _))
          .WillOnce(
              ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                     olp::http::HttpStatusCode::NOT_FOUND),
                                 "data"));
    }

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Unknown);
    ASSERT_EQ("No partitions were prefetched.",
              response.GetError().GetMessage());
  }
  Mock::VerifyAndClearExpectations(network_mock.get());
}

TEST(VersionedLayerClientTest, PrefetchPartitionsFail) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  olp::client::OlpClientSettings settings;
  settings.network_request_handler = network_mock;
  auto version = 4u;

  auto partitions_count = 2u;
  std::vector<std::string> partitions;
  partitions.reserve(partitions_count);
  for (auto i = 0u; i < partitions_count; i++) {
    partitions.emplace_back(std::to_string(i));
  }
  auto api_response =
      mockserver::ApiDefaultResponses::GenerateResourceApisResponse(kCatalog);
  const auto request =
      olp::dataservice::read::PrefetchPartitionsRequest().WithPartitionIds(
          partitions);
  read::VersionedLayerClientImpl client(kHrn, kLayerId, boost::none, settings);
  auto partitions_path = mock::GeneratePath(
      api_response, "query",
      mock::GenerateGetPartitionsPath(kLayerId, partitions, version));
  ASSERT_FALSE(partitions_path.empty());
  {
    SCOPED_TRACE("Prefetch partitions, empty request");

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        olp::dataservice::read::PrefetchPartitionsRequest(),
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::InvalidArgument);
  }
  {
    SCOPED_TRACE("Get version fails");
    EXPECT_CALL(*network_mock, Send(IsGetRequest(kUrlLookup), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     serialize(api_response)));

    auto version_path = mock::GeneratePath(
        api_response, "metadata", mock::GenerateGetLatestVersionPath());
    ASSERT_FALSE(version_path.empty());

    EXPECT_CALL(*network_mock, Send(IsGetRequest(version_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::BAD_REQUEST),
            olp::serializer::serialize(
                mockserver::ReadDefaultResponses::GenerateVersionResponse(
                    version))));

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::BadRequest);
  }
  {
    SCOPED_TRACE("Get data handles fails");

    auto version_path = mock::GeneratePath(
        api_response, "metadata", mock::GenerateGetLatestVersionPath());
    ASSERT_FALSE(version_path.empty());
    EXPECT_CALL(*network_mock, Send(IsGetRequest(version_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(
                mockserver::ReadDefaultResponses::GenerateVersionResponse(
                    version))));

    auto partitions_response =
        mockserver::ReadDefaultResponses::GeneratePartitionsResponse(
            partitions_count);
    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::BAD_REQUEST),
            olp::serializer::serialize(partitions_response)));

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful())
        << response.GetError().GetMessage().c_str();
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::BadRequest);
  }
  {
    SCOPED_TRACE("Invalid json");

    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(
                                         olp::http::HttpStatusCode::OK),
                                     "invalid json"));

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Unknown);
    ASSERT_EQ("Fail parsing response.", response.GetError().GetMessage());
  }
  {
    SCOPED_TRACE("Empty data handles");

    auto partitions_response =
        mockserver::ReadDefaultResponses::GeneratePartitionsResponse(
            partitions_count);
    auto& mutable_partitions = partitions_response.GetMutablePartitions();
    // force empty data handles
    for (auto& partition : mutable_partitions) {
      partition.SetDataHandle("");
    }

    EXPECT_CALL(*network_mock, Send(IsGetRequest(partitions_path), _, _, _, _))
        .WillOnce(ReturnHttpResponse(
            olp::http::NetworkResponse().WithStatus(
                olp::http::HttpStatusCode::OK),
            olp::serializer::serialize(partitions_response)));

    std::promise<olp::dataservice::read::PrefetchPartitionsResponse> promise;
    auto future = promise.get_future();
    auto token = client.PrefetchPartitions(
        request,
        [&promise](
            olp::dataservice::read::PrefetchPartitionsResponse response) {
          promise.set_value(std::move(response));
        },
        nullptr);
    ASSERT_NE(future.wait_for(std::chrono::seconds(kTimeout)),
              std::future_status::timeout);

    auto response = future.get();
    ASSERT_FALSE(response.IsSuccessful());
    EXPECT_EQ(response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Unknown);
    ASSERT_EQ("No partitions were prefetched.",
              response.GetError().GetMessage());
  }
  Mock::VerifyAndClearExpectations(network_mock.get());
}

TEST(VersionedLayerClientTest, PrefetchPartitionsCancel) {
  std::shared_ptr<NetworkMock> network_mock = std::make_shared<NetworkMock>();
  olp::client::OlpClientSettings settings;
  settings.network_request_handler = network_mock;
  settings.task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);
  auto partitions_count = 2u;
  std::vector<std::string> partitions;
  partitions.reserve(partitions_count);
  for (auto i = 0u; i < partitions_count; i++) {
    partitions.emplace_back(std::to_string(i));
  }
  const auto request =
      olp::dataservice::read::PrefetchPartitionsRequest().WithPartitionIds(
          partitions);
  read::VersionedLayerClientImpl client(kHrn, kLayerId, boost::none, settings);
  {
    SCOPED_TRACE("Cancel request");
    std::promise<void> block_promise;
    auto block_future = block_promise.get_future();
    settings.task_scheduler->ScheduleTask(
        [&block_future]() { block_future.get(); });
    auto cancellable = client.PrefetchPartitions(request, nullptr);

    // cancel the request and unblock queue
    cancellable.GetCancellationToken().Cancel();
    block_promise.set_value();
    auto future = cancellable.GetFuture();

    ASSERT_EQ(future.wait_for(kTimeout), std::future_status::ready);

    auto data_response = future.get();

    EXPECT_FALSE(data_response.IsSuccessful());
    EXPECT_EQ(data_response.GetError().GetErrorCode(),
              olp::client::ErrorCode::Cancelled);
  }
  Mock::VerifyAndClearExpectations(network_mock.get());
}

}  // namespace
