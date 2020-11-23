/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using executor::RemoteCommandRequest;

const int kMaxRetries = 3;
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

class EstablishCursorsTest : public ShardingTestFixture {
public:
    EstablishCursorsTest() : _nss("testdb.testcoll") {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

    /**
     * Mock a response for a killOperations command.
     */
    void expectKillOperations(size_t expected) {
        for (size_t i = 0; i < expected; i++) {
            onCommand([this](const RemoteCommandRequest& request) {
                ASSERT_EQ("admin", request.dbname) << request;
                ASSERT_TRUE(request.cmdObj.hasField("_killOperations")) << request;
                return BSON("ok" << 1);
            });
        }
    }

protected:
    const NamespaceString _nss;
};

TEST_F(EstablishCursorsTest, NoRemotes) {
    std::vector<std::pair<ShardId, BSONObj>> remotes;
    auto cursors = establishCursors(operationContext(),
                                    executor(),
                                    _nss,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    remotes,
                                    false);  // allowPartialResults


    ASSERT_EQUALS(remotes.size(), cursors.size());
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // Remote responds with non-retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });
    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteInterruptedWhileCommandInFlight) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj},
    };

    // Hang before sending the command but after resolving the host to send it to.
    auto fp = globalFailPointRegistry().find("hangBeforeSchedulingRemoteCommand");
    invariant(fp);
    auto startCount =
        fp->setMode(FailPoint::alwaysOn, 0, BSON("hostAndPort" << kTestShardHosts[0].toString()));

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::CursorKilled>);
    });

    // Verify that the failpoint is hit.
    fp->waitForTimesEntered(startCount + 1);

    // Now interrupt the opCtx which the cursor is running under.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->getServiceContext()->killOperation(
            lk, operationContext(), ErrorCodes::CursorKilled);
    }

    // Disable the failpoint to enable the ARS to continue. Once interrupted, it will then trigger a
    // killOperations for the two remotes.
    fp->setMode(FailPoint::off);

    // The OperationContext was marked as killed before the request was scheduled, however the exact
    // timing of when the interrupt condition is detected is not deterministic in this test.
    // However, since the failpoint is in a position where the remote hostAndPort is resolved, we
    // are guaranteed to get a killOperation for it but we may first see the original remote
    // request.
    auto killOpSeen = false;
    onCommand([&](const RemoteCommandRequest& request) {
        if (request.dbname == "admin" && request.cmdObj.hasField("_killOperations")) {
            killOpSeen = true;
            return BSON("ok" << 1);
        }

        // Otherwise expect the original request and mock the response.
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        CursorResponse cursorResponse(_nss, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    if (!killOpSeen) {
        expectKillOperations(1);
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        // A non-retryable error is not ignored even though allowPartialResults is true.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       true),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // Remote responds with non-retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });
    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetryableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds with retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Remote responds with success.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetryableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds with retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // We still retry up to the max retries, even if allowPartialResults is true.
    // Remote responds with success.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetryableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::HostUnreachable>);
    });

    // Remote repeatedly responds with retryable errors.
    for (int i = 0; i < kMaxRetries + 1; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    // Expect a killOperations for the remote which was not reachable.
    expectKillOperations(1);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetryableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults

        // Failure to establish a cursor due to maxing out retryable errors on one remote (in this
        // case, the only remote) was ignored, since allowPartialResults is true. The cursor entry
        // is marked as 'partialResultReturned:true', with a CursorId of 0 and no HostAndPort.
        ASSERT_EQ(cursors.size(), 1);
        ASSERT(cursors.front().getHostAndPort().empty());
        ASSERT_EQ(cursors.front().getShardId(), kTestShardIds[0]);
        ASSERT(cursors.front().getCursorResponse().getPartialResultsReturned());
        ASSERT_EQ(cursors.front().getCursorResponse().getCursorId(), CursorId{0});
    });

    // Remote repeatedly responds with retryable errors.
    for (int i = 0; i < kMaxRetries + 1; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesRespondWithSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // All remotes respond with success.
    for (auto it = remotes.begin(); it != remotes.end(); ++it) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
            CursorResponse cursorResponse(_nss, CursorId(123), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithNonError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithNonErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        // Failure is reported even though allowPartialResults was true.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       true),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithRetryableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with success on retry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithRetryableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // We still retry up to the max retries, even if allowPartialResults is true.
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });
    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with success on retry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetryableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::HostUnreachable>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote maxes out remaining retries.
    for (int i = 0; i < kMaxRetries; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetryableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // Failure to establish a cursor due to maxing out retryable errors on one remote was
        // ignored, since allowPartialResults is true. The cursor entry for that shard is marked
        // 'partialResultReturned:true', with a CursorId of 0 and no HostAndPort.
        ASSERT_EQ(remotes.size(), cursors.size());
        for (auto&& cursor : cursors) {
            const bool isMaxedOutShard = (cursor.getShardId() == kTestShardIds[1]);
            ASSERT_EQ(cursor.getHostAndPort().empty(), isMaxedOutShard);
            ASSERT_EQ(cursor.getCursorResponse().getPartialResultsReturned(), isMaxedOutShard);
            ASSERT(isMaxedOutShard ? cursor.getCursorResponse().getCursorId() == CursorId{0}
                                   : cursor.getCursorResponse().getCursorId() > CursorId{0});
        }
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retryable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote maxes out remaining retries.
    for (int i = 0; i < kMaxRetries; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, InterruptedWithDanglingRemoteRequest) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj},
        {kTestShardIds[1], cmdObj},
    };

    // Hang in ARS before it sends the request to remotes[1].
    auto fpSend = globalFailPointRegistry().find("hangBeforeSchedulingRemoteCommand");
    invariant(fpSend);
    auto timesHitSend = fpSend->setMode(
        FailPoint::alwaysOn, 0, BSON("hostAndPort" << kTestShardHosts[1].toString()));

    // Also hang in ARS::next when there is exactly 1 remote that hasn't replied yet.
    // This failpoint is important to ensure establishCursors' check for _interruptStatus.isOK()
    // happens after this unittest does opCtx->killOperation().
    auto fpNext = globalFailPointRegistry().find("hangBeforePollResponse");
    invariant(fpNext);
    auto timesHitNext = fpNext->setMode(FailPoint::alwaysOn, 0, BSON("remotesLeft" << 1));

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::CursorKilled>);
    });

    // First remote responds.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        CursorResponse cursorResponse(_nss, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Wait for ars._remotes[1] to try to send its request. We want to test the case where the
    // opCtx is killed after this happens.
    fpSend->waitForTimesEntered(timesHitSend + 1);

    // Mark the OperationContext as killed.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->getServiceContext()->killOperation(
            lk, operationContext(), ErrorCodes::CursorKilled);
    }

    // Allow ars._remotes[1] to send its request.
    fpSend->setMode(FailPoint::off);

    // Wait for establishCursors to call ars.next.
    fpNext->waitForTimesEntered(timesHitNext + 1);

    // Disable the ARS::next failpoint to allow establishCursors to handle that response.
    // Now ARS::next should check that the opCtx has been marked killed, and return a
    // failing response to establishCursors, which should clean up by sending kill commands.
    fpNext->setMode(FailPoint::off);

    // Because we paused the ARS using hangBeforePollResponse, we know the ARS will detect the
    // killed opCtx before sending any more requests. So we know only _killOperations will be sent.
    expectKillOperations(2);

    future.default_timed_get();
}

}  // namespace

}  // namespace mongo
