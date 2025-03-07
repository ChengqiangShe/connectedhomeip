/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements unit tests for the ReliableMessageProtocol
 *      implementation.
 */

#include "TestMessagingLayer.h"

#include <lib/core/CHIPCore.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/UnitTestRegistration.h>
#include <lib/support/UnitTestUtils.h>
#include <messaging/ReliableMessageContext.h>
#include <messaging/ReliableMessageMgr.h>
#include <protocols/Protocols.h>
#include <protocols/echo/Echo.h>
#include <transport/SessionManager.h>
#include <transport/TransportMgr.h>

#include <nlbyteorder.h>
#include <nlunit-test.h>

#include <errno.h>

#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeMgr.h>
#include <messaging/Flags.h>
#include <messaging/tests/MessagingContext.h>
#include <transport/raw/tests/NetworkTestHelpers.h>

namespace {

using namespace chip;
using namespace chip::Inet;
using namespace chip::Transport;
using namespace chip::Messaging;
using namespace chip::Protocols;
using namespace chip::System::Clock::Literals;

using TestContext = Test::LoopbackMessagingContext<>;

TestContext sContext;

const char PAYLOAD[] = "Hello!";

auto & gLoopback = sContext.GetLoopback();

class MockAppDelegate : public ExchangeDelegate
{
public:
    CHIP_ERROR OnMessageReceived(ExchangeContext * ec, const PayloadHeader & payloadHeader,
                                 System::PacketBufferHandle && buffer) override
    {
        IsOnMessageReceivedCalled = true;
        if (payloadHeader.IsAckMsg())
        {
            mReceivedPiggybackAck = true;
        }
        if (mDropAckResponse)
        {
            auto * rc = ec->GetReliableMessageContext();
            if (rc->HasPiggybackAckPending())
            {
                (void) rc->TakePendingPeerAckMessageCounter();
            }
        }

        if (mExchange != ec)
        {
            CloseExchangeIfNeeded();
        }

        if (!mRetainExchange)
        {
            ec = nullptr;
        }
        else
        {
            ec->WillSendMessage();
        }
        mExchange = ec;

        if (mTestSuite != nullptr)
        {
            NL_TEST_ASSERT(mTestSuite, buffer->TotalLength() == sizeof(PAYLOAD));
            NL_TEST_ASSERT(mTestSuite, memcmp(buffer->Start(), PAYLOAD, buffer->TotalLength()) == 0);
        }
        return CHIP_NO_ERROR;
    }

    void OnResponseTimeout(ExchangeContext * ec) override {}

    void CloseExchangeIfNeeded()
    {
        if (mExchange != nullptr)
        {
            mExchange->Close();
            mExchange = nullptr;
        }
    }

    bool IsOnMessageReceivedCalled = false;
    bool mReceivedPiggybackAck     = false;
    bool mDropAckResponse          = false;
    bool mRetainExchange           = false;
    ExchangeContext * mExchange    = nullptr;
    nlTestSuite * mTestSuite       = nullptr;
};

class MockSessionEstablishmentExchangeDispatch : public Messaging::ApplicationExchangeDispatch
{
public:
    bool IsReliableTransmissionAllowed() const override { return mRetainMessageOnSend; }

    bool MessagePermitted(uint16_t protocol, uint8_t type) override { return true; }

    bool IsEncryptionRequired() const override { return mRequireEncryption; }

    bool mRetainMessageOnSend = true;

    bool mRequireEncryption = false;
};

class MockSessionEstablishmentDelegate : public ExchangeDelegate
{
public:
    CHIP_ERROR OnMessageReceived(ExchangeContext * ec, const PayloadHeader & payloadHeader,
                                 System::PacketBufferHandle && buffer) override
    {
        IsOnMessageReceivedCalled = true;
        if (mTestSuite != nullptr)
        {
            NL_TEST_ASSERT(mTestSuite, buffer->TotalLength() == sizeof(PAYLOAD));
            NL_TEST_ASSERT(mTestSuite, memcmp(buffer->Start(), PAYLOAD, buffer->TotalLength()) == 0);
        }
        return CHIP_NO_ERROR;
    }

    void OnResponseTimeout(ExchangeContext * ec) override {}

    virtual ExchangeMessageDispatch & GetMessageDispatch() override { return mMessageDispatch; }

    bool IsOnMessageReceivedCalled = false;
    MockSessionEstablishmentExchangeDispatch mMessageDispatch;
    nlTestSuite * mTestSuite = nullptr;
};

void CheckAddClearRetrans(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    MockAppDelegate mockAppDelegate;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockAppDelegate);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm     = ctx.GetExchangeManager().GetReliableMessageMgr();
    ReliableMessageContext * rc = exchange->GetReliableMessageContext();
    NL_TEST_ASSERT(inSuite, rm != nullptr);
    NL_TEST_ASSERT(inSuite, rc != nullptr);

    ReliableMessageMgr::RetransTableEntry * entry;

    rm->AddToRetransTable(rc, &entry);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);
    rm->ClearRetransTable(*entry);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    exchange->Close();
}

void CheckResendApplicationMessage(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockSender;
    // TODO: temporarily create a SessionHandle from node id, will be fix in PR 3602
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's drop the initial message
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 2;
    gLoopback.mDroppedMessageCount = 0;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // Ensure the exchange stays open after we send (unlike the CheckCloseExchangeAndResendApplicationMessage case), by claiming to
    // expect a response.
    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendMessageFlags::kExpectResponse);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was dropped, and was added to retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was dropped, and is still there in the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 0);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 2);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // sleep another 65 ms to trigger second re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was NOT dropped, and the retransmit table is empty, as we should have gotten an ack
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount >= 3);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 2);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    exchange->Close();
}

void CheckCloseExchangeAndResendApplicationMessage(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockSender;
    // TODO: temporarily create a SessionHandle from node id, will be fixed in PR 3602
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's drop the initial message
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 2;
    gLoopback.mDroppedMessageCount = 0;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was dropped, and was added to retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was dropped, and is still there in the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 0);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 2);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // sleep another 65 ms to trigger second re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was NOT dropped, and the retransmit table is empty, as we should have gotten an ack
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount >= 3);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 2);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckFailedMessageRetainOnSend(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockSessionEstablishmentDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    mockSender.mMessageDispatch.mRetainMessageOnSend = false;

    // Let's drop the initial message
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 1;
    gLoopback.mDroppedMessageCount = 0;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was dropped
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit table is empty, as we did not provide a message to retain
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckUnencryptedMessageReceiveFailure(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    MockSessionEstablishmentDelegate mockReceiver;
    CHIP_ERROR err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    // Expect the received messages to be encrypted
    mockReceiver.mMessageDispatch.mRequireEncryption = true;

    MockSessionEstablishmentDelegate mockSender;
    ExchangeContext * exchange = ctx.NewUnauthenticatedExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;

    // We are sending a malicious packet, doesn't expect an ack
    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kNoAutoRequestAck));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Test that the message was actually sent (and not dropped)
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);
    // Test that the message was dropped by the receiver
    NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckResendApplicationMessageWithPeerExchange(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's drop the initial message
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 1;
    gLoopback.mDroppedMessageCount = 0;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was dropped, and was added to retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 0);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);
    NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was not dropped, and is no longer in the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount >= 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    mockReceiver.mTestSuite = nullptr;

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
}

void CheckDuplicateMessageClosedExchange(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_RMP_DEFAULT_INITIAL_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_RMP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's not drop the message. Expectation is that it is received by the peer, but the ack is dropped
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;

    // Drop the ack, and also close the peer exchange
    mockReceiver.mDropAckResponse = true;
    mockReceiver.mRetainExchange  = false;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent
    // The ack was dropped, and message was added to the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // Let's not drop the duplicate message
    mockReceiver.mDropAckResponse = false;

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was sent and the ack was sent
    // and retransmit table was cleared
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 3);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckResendSessionEstablishmentMessageWithPeerExchange(nlTestSuite * inSuite, void * inContext)
{
    // Making this static to reduce stack usage, as some platforms have limits on stack size.
    static Test::MessagingContext ctx;

    TestContext & inctx = *static_cast<TestContext *>(inContext);

    CHIP_ERROR err = ctx.InitFromExisting(inctx);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    MockSessionEstablishmentDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockSessionEstablishmentDelegate mockSender;
    ExchangeContext * exchange = ctx.NewUnauthenticatedExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's drop the initial message
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 1;
    gLoopback.mDroppedMessageCount = 0;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    inctx.DrainAndServiceIO();

    // Ensure the message was dropped, and was added to retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 0);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);
    NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    inctx.DrainAndServiceIO();

    // Ensure the retransmit message was not dropped, and is no longer in the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount >= 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    mockReceiver.mTestSuite = nullptr;

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    ctx.ShutdownAndRestoreExisting(inctx);
}

void CheckDuplicateMessage(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_RMP_DEFAULT_INITIAL_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_RMP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // Let's not drop the message. Expectation is that it is received by the peer, but the ack is dropped
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;

    // Drop the ack, and keep the exchange around to receive the duplicate message
    mockReceiver.mDropAckResponse = true;
    mockReceiver.mRetainExchange  = true;

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent
    // The ack was dropped, and message was added to the retransmit table
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    // Let's not drop the duplicate message
    mockReceiver.mDropAckResponse = false;
    mockReceiver.mRetainExchange  = false;

    // sleep 65 ms to trigger first re-transmit
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // Ensure the retransmit message was sent and the ack was sent
    // and retransmit table was cleared
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 3);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    mockReceiver.CloseExchangeIfNeeded();
}

void CheckReceiveAfterStandaloneAck(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // We send a message, have it get received by the peer, then an ack is
    // returned, then a reply is returned.  We need to keep the receiver
    // exchange alive until it does the message send (so we can send the
    // response from the receiver and so the initial sender exchange can get
    // it).
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;
    mockReceiver.mRetainExchange   = true;

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    // And that we have not seen an ack yet.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    ReliableMessageContext * receiverRc = mockReceiver.mExchange->GetReliableMessageContext();
    NL_TEST_ASSERT(inSuite, receiverRc->IsAckPending());

    // Send the standalone ack.
    receiverRc->SendStandaloneAckMessage();
    ctx.DrainAndServiceIO();

    // Ensure the ack was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have not gotten any app-level responses so far.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);

    // And that we have now gotten our ack.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // Now send a message from the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response and its ack was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 4);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have received that response.
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckPiggybackAfterPiggyback(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // We send a message, have it get received by the peer, have the peer return
    // a piggybacked ack.  Then we send a second message this time _not_
    // requesting an ack, get a response, and see whether an ack was
    // piggybacked.  We need to keep both exchanges alive for that (so we can
    // send the response from the receiver and so the initial sender exchange
    // can get it).
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;
    mockReceiver.mRetainExchange   = true;
    mockSender.mRetainExchange     = true;

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    // And that we have not seen an ack yet.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    ReliableMessageContext * receiverRc = mockReceiver.mExchange->GetReliableMessageContext();
    NL_TEST_ASSERT(inSuite, receiverRc->IsAckPending());

    // Ensure that we have not gotten any app-level responses or acks so far.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, !mockSender.mReceivedPiggybackAck);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // Now send a message from the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err =
        mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer),
                                            SendFlags(SendMessageFlags::kExpectResponse).Set(SendMessageFlags::kNoAutoRequestAck));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have received that response and it had a piggyback ack.
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, mockSender.mReceivedPiggybackAck);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // Reset various state so we can measure things again.
    mockReceiver.IsOnMessageReceivedCalled = false;
    mockSender.IsOnMessageReceivedCalled   = false;
    mockSender.mReceivedPiggybackAck       = false;

    // Now send a new message to the other side, but don't ask for an ack.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer),
                                SendFlags(SendMessageFlags::kExpectResponse).Set(SendMessageFlags::kNoAutoRequestAck));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 3);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    // And that we are not expecting an ack.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // Send the final response.  At this point we don't need to keep the
    // exchanges alive anymore.
    mockReceiver.mRetainExchange = false;
    mockSender.mRetainExchange   = false;

    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response and its ack was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 5);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have received that response and it had a piggyback ack.
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, mockSender.mReceivedPiggybackAck);

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckSendUnsolicitedStandaloneAckMessage(nlTestSuite * inSuite, void * inContext)
{
    /**
     * Tests sending a standalone ack message that is:
     * 1) Unsolicited.
     * 2) Requests an ack.
     *
     * This is not a thing that would normally happen, but a malicious entity
     * could absolutely do this.
     */
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData("", 0);
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // We send a message, have it get received by the peer, expect an ack from
    // the peer.
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;

    // Purposefully sending a standalone ack that requests an ack!
    err = exchange->SendMessage(SecureChannel::MsgType::StandaloneAck, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    // Needs a manual close, because SendMessage does not close for standalone acks.
    exchange->Close();
    ctx.DrainAndServiceIO();

    // Ensure the message and its ack were sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that nothing is waiting for acks.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckSendStandaloneAckMessage(nlTestSuite * inSuite, void * inContext)
{
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    MockAppDelegate mockAppDelegate;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockAppDelegate);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    ReliableMessageMgr * rm     = ctx.GetExchangeManager().GetReliableMessageMgr();
    ReliableMessageContext * rc = exchange->GetReliableMessageContext();
    NL_TEST_ASSERT(inSuite, rm != nullptr);
    NL_TEST_ASSERT(inSuite, rc != nullptr);

    NL_TEST_ASSERT(inSuite, rc->SendStandaloneAckMessage() == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Need manual close because standalone acks don't close exchanges.
    exchange->Close();
}

void CheckMessageAfterClosed(nlTestSuite * inSuite, void * inContext)
{
    /**
     * This test performs the following sequence of actions, where all messages
     * are sent with MRP enabled:
     *
     * 1) Initiator sends message to responder.
     * 2) Responder responds to the message (piggybacking an ack) and closes
     *    the exchange.
     * 3) Initiator sends a response to the response on the same exchange, again
     *    piggybacking an ack.
     *
     * This is basically the "command, response, status response" flow, with the
     * responder closing the exchange after it sends the response.
     */

    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;
    // We need to keep both exchanges alive for the thing we are testing here.
    mockReceiver.mRetainExchange = true;
    mockSender.mRetainExchange   = true;

    NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, !mockReceiver.mReceivedPiggybackAck);

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, !mockReceiver.mReceivedPiggybackAck);

    // And that we have not seen an ack yet.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    ReliableMessageContext * receiverRc = mockReceiver.mExchange->GetReliableMessageContext();
    NL_TEST_ASSERT(inSuite, receiverRc->IsAckPending());

    // Ensure that we have not gotten any app-level responses or acks so far.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, !mockSender.mReceivedPiggybackAck);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // Now send a message from the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have received that response and it had a piggyback ack.
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, mockSender.mReceivedPiggybackAck);
    // And that we are now waiting for an ack for the response.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // Reset various state so we can measure things again.
    mockReceiver.IsOnMessageReceivedCalled = false;
    mockReceiver.mReceivedPiggybackAck     = false;
    mockSender.IsOnMessageReceivedCalled   = false;
    mockSender.mReceivedPiggybackAck       = false;

    // Now send a second message to the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent (and the ack for it was also sent).
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 4);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was not received (because the exchange is closed on the
    // receiver).
    NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);

    // And that we are not expecting an ack; acks should have been flushed
    // immediately on the receiver, due to the exchange being closed.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    err = ctx.GetExchangeManager().UnregisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckLostResponseWithPiggyback(nlTestSuite * inSuite, void * inContext)
{
    /**
     * This tests the following scenario:
     * 1) A reliable message is sent from initiator to responder.
     * 2) The responder sends a response with a piggybacked ack, which is lost.
     * 3) Initiator resends the message.
     * 4) Responder responds to the resent message with a standalone ack.
     * 5) The responder retransmits the application-level response.
     * 4) The initiator should receive the application-level response.
     */
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // Make sure that we resend our message before the other side does.
    exchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                              {
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                  64_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                              });

    // We send a message, the other side sends an application-level response
    // (which is lost), then we do a retransmit that is acked, then the other
    // side does a retransmit.  We need to keep the receiver exchange alive (so
    // we can send the response from the receiver), but don't need anything
    // special for the sender exchange, because it will be waiting for the
    // application-level response.
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;
    mockReceiver.mRetainExchange   = true;

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    // And that we have not gotten any app-level responses or acks so far.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    ReliableMessageContext * receiverRc = mockReceiver.mExchange->GetReliableMessageContext();
    // Should have pending ack here.
    NL_TEST_ASSERT(inSuite, receiverRc->IsAckPending());
    // Make sure receiver resends after sender does, and there's enough of a gap
    // that we are very unlikely to actually trigger the resends on the receiver
    // when we trigger the resends on the sender.
    mockReceiver.mExchange->GetSessionHandle().SetMRPConfig(&ctx.GetSecureSessionManager(),
                                                            {
                                                                256_ms32, // CHIP_CONFIG_MRP_DEFAULT_IDLE_RETRY_INTERVAL
                                                                256_ms32, // CHIP_CONFIG_MRP_DEFAULT_ACTIVE_RETRY_INTERVAL
                                                            });

    // Now send a message from the other side, but drop it.
    gLoopback.mNumMessagesToDrop = 1;

    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    // Stop keeping receiver exchange alive.
    mockReceiver.mRetainExchange = true;

    err = mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response was sent but dropped.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mNumMessagesToDrop == 0);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);

    // Ensure that we have not received that response.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, !mockSender.mReceivedPiggybackAck);
    // We now have our un-acked message still waiting to retransmit and the
    // message that the other side sent is waiting for an ack.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 2);

    // Reset various state so we can measure things again.
    mockReceiver.IsOnMessageReceivedCalled = false;
    mockReceiver.mReceivedPiggybackAck     = false;
    mockSender.IsOnMessageReceivedCalled   = false;
    mockSender.mReceivedPiggybackAck       = false;

    // sleep 65 ms to trigger re-transmit from sender
    chip::test_utils::SleepMillis(65);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // We resent our first message, which did not make it to the app-level
    // listener on the receiver (because it's a duplicate) but did trigger a
    // standalone ack.
    //
    // Now the annoying part is that depending on how long we _actually_ slept
    // we might have also triggered the retransmit from the other side, even
    // though we did not want to.  Handle both cases here.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 4 || gLoopback.mSentMessageCount == 6);
    if (gLoopback.mSentMessageCount == 4)
    {
        // Just triggered the retransmit from the sender.
        NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
        NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
        NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);
        NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);
    }
    else
    {
        // Also triggered the retransmit from the receiver.
        NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
        NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);
        NL_TEST_ASSERT(inSuite, !mockReceiver.IsOnMessageReceivedCalled);
        NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
    }

    // sleep 65*3 ms to trigger re-transmit from receiver
    chip::test_utils::SleepMillis(65 * 3);
    ReliableMessageMgr::Timeout(&ctx.GetSystemLayer(), rm);
    ctx.DrainAndServiceIO();

    // And now we've definitely resent our response message, which should show
    // up as an app-level message and trigger a standalone ack.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 6);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 1);
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);

    // Should be all done now.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

void CheckLostStandaloneAck(nlTestSuite * inSuite, void * inContext)
{
    /**
     * This tests the following scenario:
     * 1) A reliable message is sent from initiator to responder.
     * 2) The responder sends a standalone ack, which is lost.
     * 3) The responder sends an application-level response.
     * 4) The initiator sends a reliable response to the app-level response.
     *
     * This should succeed, with all application-level messages being delivered
     * and no crashes.
     */
    TestContext & ctx = *reinterpret_cast<TestContext *>(inContext);

    chip::System::PacketBufferHandle buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    CHIP_ERROR err = CHIP_NO_ERROR;

    MockAppDelegate mockReceiver;
    err = ctx.GetExchangeManager().RegisterUnsolicitedMessageHandlerForType(Echo::MsgType::EchoRequest, &mockReceiver);
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);

    mockReceiver.mTestSuite = inSuite;

    MockAppDelegate mockSender;
    ExchangeContext * exchange = ctx.NewExchangeToAlice(&mockSender);
    NL_TEST_ASSERT(inSuite, exchange != nullptr);

    mockSender.mTestSuite = inSuite;

    ReliableMessageMgr * rm = ctx.GetExchangeManager().GetReliableMessageMgr();
    NL_TEST_ASSERT(inSuite, rm != nullptr);

    // Ensure the retransmit table is empty right now
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);

    // We send a message, the other side sends a standalone ack first (which is
    // lost), then an application response, then we respond to that response.
    // We need to keep both exchanges alive for that (so we can send the
    // response from the receiver and so the initial sender exchange can send a
    // response to that).
    gLoopback.mSentMessageCount    = 0;
    gLoopback.mNumMessagesToDrop   = 0;
    gLoopback.mDroppedMessageCount = 0;
    mockReceiver.mRetainExchange   = true;
    mockSender.mRetainExchange     = true;

    // And ensure the ack heading back our way is dropped.
    mockReceiver.mDropAckResponse = true;

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer), SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 1);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);

    // And that we have not gotten any app-level responses or acks so far.
    NL_TEST_ASSERT(inSuite, !mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    ReliableMessageContext * receiverRc = mockReceiver.mExchange->GetReliableMessageContext();
    // Ack should have been dropped.
    NL_TEST_ASSERT(inSuite, !receiverRc->IsAckPending());

    // Don't drop any more acks.
    mockReceiver.mDropAckResponse = false;

    // Now send a message from the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = mockReceiver.mExchange->SendMessage(Echo::MsgType::EchoResponse, std::move(buffer),
                                              SendFlags(SendMessageFlags::kExpectResponse));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the response was sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 2);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // Ensure that we have received that response and had a piggyback ack.
    NL_TEST_ASSERT(inSuite, mockSender.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, mockSender.mReceivedPiggybackAck);
    // We now have just the received message waiting for an ack.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 1);

    // Reset various state so we can measure things again.
    mockReceiver.IsOnMessageReceivedCalled = false;
    mockReceiver.mReceivedPiggybackAck     = false;
    mockSender.IsOnMessageReceivedCalled   = false;
    mockSender.mReceivedPiggybackAck       = false;

    // Stop retaining the recipient exchange.
    mockReceiver.mRetainExchange = false;

    // Now send a new message to the other side.
    buffer = chip::MessagePacketBuffer::NewWithData(PAYLOAD, sizeof(PAYLOAD));
    NL_TEST_ASSERT(inSuite, !buffer.IsNull());

    err = exchange->SendMessage(Echo::MsgType::EchoRequest, std::move(buffer));
    NL_TEST_ASSERT(inSuite, err == CHIP_NO_ERROR);
    ctx.DrainAndServiceIO();

    // Ensure the message and the standalone ack to it were sent.
    NL_TEST_ASSERT(inSuite, gLoopback.mSentMessageCount == 4);
    NL_TEST_ASSERT(inSuite, gLoopback.mDroppedMessageCount == 0);

    // And that it was received.
    NL_TEST_ASSERT(inSuite, mockReceiver.IsOnMessageReceivedCalled);
    NL_TEST_ASSERT(inSuite, mockReceiver.mReceivedPiggybackAck);

    // And that receiver has no ack pending.
    NL_TEST_ASSERT(inSuite, !receiverRc->IsAckPending());

    // And that there are no un-acked messages left.
    NL_TEST_ASSERT(inSuite, rm->TestGetCountRetransTable() == 0);
}

/**
 * TODO: A test that we should have but can't write with the existing
 * infrastructure we have:
 *
 * 1. A sends message 1 to B
 * 2. B is slow to respond, A does a resend and the resend is delayed in the network.
 * 3. B responds with message 2, which acks message 1.
 * 4. A sends message 3 to B
 * 5. B sends standalone ack to message 3, which is lost
 * 6. The duplicate message from step 3 is delivered and triggers a standalone ack.
 * 7. B responds with message 4, which should carry a piggyback ack for message 3
 *    (this is the part that needs testing!)
 * 8. A sends message 5 to B.
 */

// Test Suite

/**
 *  Test Suite that lists all the test functions.
 */
// clang-format off
const nlTest sTests[] =
{
    NL_TEST_DEF("Test ReliableMessageMgr::CheckAddClearRetrans", CheckAddClearRetrans),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckResendApplicationMessage", CheckResendApplicationMessage),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckCloseExchangeAndResendApplicationMessage", CheckCloseExchangeAndResendApplicationMessage),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckFailedMessageRetainOnSend", CheckFailedMessageRetainOnSend),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckResendApplicationMessageWithPeerExchange", CheckResendApplicationMessageWithPeerExchange),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckResendSessionEstablishmentMessageWithPeerExchange", CheckResendSessionEstablishmentMessageWithPeerExchange),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckDuplicateMessage", CheckDuplicateMessage),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckDuplicateMessageClosedExchange", CheckDuplicateMessageClosedExchange),
    NL_TEST_DEF("Test that a reply after a standalone ack comes through correctly", CheckReceiveAfterStandaloneAck),
    NL_TEST_DEF("Test that a reply to a non-MRP message piggybacks an ack if there were MRP things happening on the context before", CheckPiggybackAfterPiggyback),
    NL_TEST_DEF("Test sending an unsolicited ack-soliciting 'standalone ack' message", CheckSendUnsolicitedStandaloneAckMessage),
    NL_TEST_DEF("Test ReliableMessageMgr::CheckSendStandaloneAckMessage", CheckSendStandaloneAckMessage),
    NL_TEST_DEF("Test command, response, default response, with receiver closing exchange after sending response", CheckMessageAfterClosed),
    NL_TEST_DEF("Test that unencrypted message is dropped if exchange requires encryption", CheckUnencryptedMessageReceiveFailure),
    NL_TEST_DEF("Test that dropping an application-level message with a piggyback ack works ok once both sides retransmit", CheckLostResponseWithPiggyback),
    NL_TEST_DEF("Test that an application-level response-to-response after a lost standalone ack to the initial message works", CheckLostStandaloneAck),

    NL_TEST_SENTINEL()
};

nlTestSuite sSuite =
{
    "Test-CHIP-ReliableMessageProtocol",
    &sTests[0],
    TestContext::InitializeAsync,
    TestContext::Finalize
};
// clang-format on

} // namespace

/**
 *  Main
 */
int TestReliableMessageProtocol()
{
    // Run test suit against one context
    nlTestRunner(&sSuite, &sContext);

    return (nlTestRunnerStats(&sSuite));
}

CHIP_REGISTER_TEST_SUITE(TestReliableMessageProtocol)
