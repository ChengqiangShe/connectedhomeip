/*
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
 *      This file implements the CHIP reliable message protocol.
 *
 */

#include <inttypes.h>

#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeMgr.h>
#include <messaging/ReliableMessageContext.h>

#include <lib/core/CHIPEncoding.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/Defer.h>
#include <messaging/ErrorCategory.h>
#include <messaging/Flags.h>
#include <messaging/ReliableMessageMgr.h>
#include <protocols/Protocols.h>
#include <protocols/secure_channel/Constants.h>

namespace chip {
namespace Messaging {

ReliableMessageContext::ReliableMessageContext() : mNextAckTime(0), mPendingPeerAckMessageCounter(0) {}

bool ReliableMessageContext::AutoRequestAck() const
{
    return mFlags.Has(Flags::kFlagAutoRequestAck);
}

bool ReliableMessageContext::IsAckPending() const
{
    return mFlags.Has(Flags::kFlagAckPending);
}

bool ReliableMessageContext::HasRcvdMsgFromPeer() const
{
    return mFlags.Has(Flags::kFlagMsgRcvdFromPeer);
}

void ReliableMessageContext::SetAutoRequestAck(bool autoReqAck)
{
    mFlags.Set(Flags::kFlagAutoRequestAck, autoReqAck);
}

void ReliableMessageContext::SetMsgRcvdFromPeer(bool inMsgRcvdFromPeer)
{
    mFlags.Set(Flags::kFlagMsgRcvdFromPeer, inMsgRcvdFromPeer);
}

void ReliableMessageContext::SetAckPending(bool inAckPending)
{
    mFlags.Set(Flags::kFlagAckPending, inAckPending);
}

void ReliableMessageContext::SetDropAckDebug(bool inDropAckDebug)
{
    mFlags.Set(Flags::kFlagDropAckDebug, inDropAckDebug);
}

bool ReliableMessageContext::IsMessageNotAcked() const
{
    return mFlags.Has(Flags::kFlagMesageNotAcked);
}

void ReliableMessageContext::SetMessageNotAcked(bool messageNotAcked)
{
    mFlags.Set(Flags::kFlagMesageNotAcked, messageNotAcked);
}

bool ReliableMessageContext::ShouldDropAckDebug() const
{
    return mFlags.Has(Flags::kFlagDropAckDebug);
}

ExchangeContext * ReliableMessageContext::GetExchangeContext()
{
    return static_cast<ExchangeContext *>(this);
}

ReliableMessageMgr * ReliableMessageContext::GetReliableMessageMgr()
{
    return static_cast<ExchangeContext *>(this)->GetExchangeMgr()->GetReliableMessageMgr();
}

CHIP_ERROR ReliableMessageContext::FlushAcks()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    if (IsAckPending())
    {
        err = SendStandaloneAckMessage();

        if (err == CHIP_NO_ERROR)
        {
            ChipLogDetail(ExchangeManager,
                          "Flushed pending ack for MessageCounter:" ChipLogFormatMessageCounter
                          " on exchange " ChipLogFormatExchange,
                          mPendingPeerAckMessageCounter, ChipLogValueExchange(GetExchangeContext()));
        }
    }

    return err;
}

bool ReliableMessageContext::HasPiggybackAckPending() const
{
    return mFlags.Has(Flags::kFlagAckMessageCounterIsValid);
}

/**
 *  Process received Ack. Remove the corresponding message context from the RetransTable and execute the application
 *  callback
 *
 *  @note
 *    This message is part of the CHIP Reliable Messaging protocol.
 *
 *  @param[in]    ackMessageCounter         The acknowledged message counter of the incoming message.
 */
void ReliableMessageContext::HandleRcvdAck(uint32_t ackMessageCounter)
{
    // Msg is an Ack; Check Retrans Table and remove message context
    if (!GetReliableMessageMgr()->CheckAndRemRetransTable(this, ackMessageCounter))
    {
        // This can happen quite easily due to a packet with a piggyback ack
        // being lost and retransmitted.
        ChipLogDetail(ExchangeManager,
                      "CHIP MessageCounter:" ChipLogFormatMessageCounter " not in RetransTable on exchange " ChipLogFormatExchange,
                      ackMessageCounter, ChipLogValueExchange(GetExchangeContext()));
    }
    else
    {
        ChipLogDetail(ExchangeManager,
                      "Removed CHIP MessageCounter:" ChipLogFormatMessageCounter
                      " from RetransTable on exchange " ChipLogFormatExchange,
                      ackMessageCounter, ChipLogValueExchange(GetExchangeContext()));
    }
}

CHIP_ERROR ReliableMessageContext::HandleNeedsAck(uint32_t messageCounter, BitFlags<MessageFlagValues> messageFlags)

{
    // Skip processing ack if drop ack debug is enabled.
    if (ShouldDropAckDebug())
        return CHIP_NO_ERROR;

    CHIP_ERROR err = HandleNeedsAckInner(messageCounter, messageFlags);

    // Schedule next physical wakeup on function exit
    GetReliableMessageMgr()->StartTimer();

    return err;
}

CHIP_ERROR ReliableMessageContext::HandleNeedsAckInner(uint32_t messageCounter, BitFlags<MessageFlagValues> messageFlags)

{
    // If the message IS a duplicate there will never be a response to it, so we
    // should not wait for one and just immediately send a standalone ack.
    if (messageFlags.Has(MessageFlagValues::kDuplicateMessage))
    {
        ChipLogDetail(ExchangeManager,
                      "Forcing tx of solitary ack for duplicate MessageCounter:" ChipLogFormatMessageCounter
                      " on exchange " ChipLogFormatExchange,
                      messageCounter, ChipLogValueExchange(GetExchangeContext()));

        bool wasAckPending = IsAckPending() && mPendingPeerAckMessageCounter != messageCounter;

        bool messageCounterWasValid = HasPiggybackAckPending();

        // Temporary store currently pending ack message counter (even if there is none).
        uint32_t tempAckMessageCounter = mPendingPeerAckMessageCounter;

        SetPendingPeerAckMessageCounter(messageCounter);
        CHIP_ERROR err = SendStandaloneAckMessage();

        if (wasAckPending)
        {
            // Restore previously pending ack message counter.
            SetPendingPeerAckMessageCounter(tempAckMessageCounter);
        }
        else if (messageCounterWasValid)
        {
            // Restore the previous value, so later piggybacks will pick it up,
            // but don't set out "ack is pending" state, because we didn't use
            // to have it set.
            mPendingPeerAckMessageCounter = tempAckMessageCounter;
        }
        // Otherwise don't restore the invalid old mPendingPeerAckMessageCounter
        // value, so we preserve the invariant that once we have had an ack
        // pending we always have a valid mPendingPeerAckMessageCounter.

        return err;
    }
    // Otherwise, the message IS NOT a duplicate.
    else
    {
        if (IsAckPending())
        {
            ChipLogDetail(ExchangeManager,
                          "Pending ack queue full; forcing tx of solitary ack for MessageCounter:" ChipLogFormatMessageCounter
                          " on exchange " ChipLogFormatExchange,
                          mPendingPeerAckMessageCounter, ChipLogValueExchange(GetExchangeContext()));
            // Send the Ack for the currently pending Ack in a SecureChannel::StandaloneAck message.
            ReturnErrorOnFailure(SendStandaloneAckMessage());
        }

        // Replace the Pending ack message counter.
        SetPendingPeerAckMessageCounter(messageCounter);
        using namespace System::Clock::Literals;
        mNextAckTime = System::SystemClock().GetMonotonicTimestamp() + CHIP_CONFIG_RMP_DEFAULT_ACK_TIMEOUT;
        return CHIP_NO_ERROR;
    }
}

CHIP_ERROR ReliableMessageContext::SendStandaloneAckMessage()
{
    // Allocate a buffer for the null message
    System::PacketBufferHandle msgBuf = MessagePacketBuffer::New(0);
    if (msgBuf.IsNull())
    {
        return CHIP_ERROR_NO_MEMORY;
    }

    // Send the null message
    ChipLogDetail(ExchangeManager,
                  "Sending Standalone Ack for MessageCounter:" ChipLogFormatMessageCounter " on exchange " ChipLogFormatExchange,
                  mPendingPeerAckMessageCounter, ChipLogValueExchange(GetExchangeContext()));

    CHIP_ERROR err = GetExchangeContext()->SendMessage(Protocols::SecureChannel::MsgType::StandaloneAck, std::move(msgBuf),
                                                       BitFlags<SendMessageFlags>{ SendMessageFlags::kNoAutoRequestAck });
    if (IsSendErrorNonCritical(err))
    {
        ChipLogError(ExchangeManager,
                     "Non-crit err %" CHIP_ERROR_FORMAT " sending solitary ack for MessageCounter:" ChipLogFormatMessageCounter
                     " on exchange " ChipLogFormatExchange,
                     err.Format(), mPendingPeerAckMessageCounter, ChipLogValueExchange(GetExchangeContext()));
        return CHIP_NO_ERROR;
    }
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(ExchangeManager,
                     "Failed to send Solitary ack for MessageCounter:" ChipLogFormatMessageCounter
                     " on exchange " ChipLogFormatExchange ":%" CHIP_ERROR_FORMAT,
                     mPendingPeerAckMessageCounter, ChipLogValueExchange(GetExchangeContext()), err.Format());
    }

    return err;
}

void ReliableMessageContext::SetPendingPeerAckMessageCounter(uint32_t aPeerAckMessageCounter)
{
    mPendingPeerAckMessageCounter = aPeerAckMessageCounter;
    SetAckPending(true);
    mFlags.Set(Flags::kFlagAckMessageCounterIsValid);
}

} // namespace Messaging
} // namespace chip
