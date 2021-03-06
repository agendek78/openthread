/*
 *    Copyright (c) 2016, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 *    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements a SPI interface to the OpenThread stack.
 */

#ifdef OPENTHREAD_CONFIG_FILE
#include OPENTHREAD_CONFIG_FILE
#else
#include <openthread-config.h>
#endif

#include "openthread/ncp.h"
#include "openthread/platform/spi-slave.h"

#include <common/code_utils.hpp>
#include <common/new.hpp>
#include <common/debug.hpp>
#include <net/ip6.hpp>
#include <ncp/ncp_spi.hpp>
#include <core/openthread-core-config.h>
#include <openthread-instance.h>

#define SPI_RESET_FLAG          0x80
#define SPI_CRC_FLAG            0x40
#define SPI_PATTERN_VALUE       0x02
#define SPI_PATTERN_MASK        0x03

#if OPENTHREAD_ENABLE_NCP_SPI

namespace ot {

static otDEFINE_ALIGNED_VAR(sNcpRaw, sizeof(NcpSpi), uint64_t);

extern "C" void otNcpInit(otInstance *aInstance)
{
    NcpSpi *ncpSpi = NULL;

    ncpSpi = new(&sNcpRaw) NcpSpi(aInstance);

    if (ncpSpi == NULL || ncpSpi != NcpBase::GetNcpInstance())
    {
        assert(false);
    }
}

static void spi_header_set_flag_byte(uint8_t *header, uint8_t value)
{
    header[0] = value;
}

static void spi_header_set_accept_len(uint8_t *header, uint16_t len)
{
    header[1] = ((len >> 0) & 0xFF);
    header[2] = ((len >> 8) & 0xFF);
}

static void spi_header_set_data_len(uint8_t *header, uint16_t len)
{
    header[3] = ((len >> 0) & 0xFF);
    header[4] = ((len >> 8) & 0xFF);
}

static uint8_t spi_header_get_flag_byte(const uint8_t *header)
{
    return header[0];
}

static uint16_t spi_header_get_accept_len(const uint8_t *header)
{
    return ( header[1] + static_cast<uint16_t>(header[2] << 8) );
}

static uint16_t spi_header_get_data_len(const uint8_t *header)
{
    return ( header[3] + static_cast<uint16_t>(header[4] << 8) );
}

NcpSpi::NcpSpi(otInstance *aInstance):
    NcpBase(aInstance),
    mHandleRxFrameTask(aInstance->mIp6.mTaskletScheduler, &NcpSpi::HandleRxFrame, this),
    mPrepareTxFrameTask(aInstance->mIp6.mTaskletScheduler, &NcpSpi::PrepareTxFrame, this)
{
    memset(mEmptySendFrame, 0, kSpiHeaderLength);
    memset(mSendFrame, 0, kSpiHeaderLength);

    mTxState = kTxStateIdle;
    mHandlingRxFrame = false;

    mTxFrameBuffer.SetCallbacks(NULL, TxFrameBufferHasData, this);

    spi_header_set_flag_byte(mSendFrame, SPI_RESET_FLAG|SPI_PATTERN_VALUE);
    spi_header_set_flag_byte(mEmptySendFrame, SPI_RESET_FLAG|SPI_PATTERN_VALUE);
    spi_header_set_accept_len(mSendFrame, sizeof(mReceiveFrame) - kSpiHeaderLength);
    otPlatSpiSlaveEnable(&NcpSpi::SpiTransactionComplete, (void*)this);

    // We signal an interrupt on this first transaction to
    // make sure that the host processor knows that our
    // reset flag was set.
    otPlatSpiSlavePrepareTransaction(mEmptySendFrame, kSpiHeaderLength, mEmptyReceiveFrame, kSpiHeaderLength, true);
}

void NcpSpi::SpiTransactionComplete(void *aContext, uint8_t *anOutputBuf, uint16_t aOutputBufLen, uint8_t *aInputBuf,
                                    uint16_t aInputBufLen, uint16_t aTransactionLength)
{
    static_cast<NcpSpi*>(aContext)->SpiTransactionComplete(anOutputBuf, aOutputBufLen, aInputBuf, aInputBufLen,
                                                           aTransactionLength);
}

void NcpSpi::SpiTransactionComplete(uint8_t *aMISOBuf, uint16_t aMISOBufLen, uint8_t *aMOSIBuf, uint16_t aMOSIBufLen,
                                    uint16_t aTransactionLength)
{
    // This may be executed from an interrupt context.
    // Must return as quickly as possible.

    uint16_t rx_data_len(0);
    uint16_t rx_accept_len(0);
    uint16_t tx_data_len(0);
    uint16_t tx_accept_len(0);

    // TODO: Check `PATTERN` bits of `HDR` and ignore frame if not set.
    //       Holding off on implementing this so as to not cause immediate
    //       compatibility problems, even though it is required by the spec.

    if (aTransactionLength >= kSpiHeaderLength)
    {
        if (aMISOBufLen >= kSpiHeaderLength)
        {
            rx_accept_len = spi_header_get_accept_len(aMISOBuf);
            tx_data_len = spi_header_get_data_len(aMISOBuf);
            (void)spi_header_get_flag_byte(aMISOBuf);
        }

        if (aMOSIBufLen >= kSpiHeaderLength)
        {
            rx_data_len = spi_header_get_data_len(aMOSIBuf);
            tx_accept_len = spi_header_get_accept_len(aMOSIBuf);
        }

        if ( !mHandlingRxFrame
          && (rx_data_len > 0)
          && (rx_data_len <= (aTransactionLength - kSpiHeaderLength))
          && (rx_data_len <= rx_accept_len)
        ) {
            mHandlingRxFrame = true;
            mHandleRxFrameTask.Post();
        }

        if ( (mTxState == kTxStateSending)
          && (tx_data_len > 0)
          && (tx_data_len <= (aTransactionLength - kSpiHeaderLength))
          && (tx_data_len <= tx_accept_len)
        ) {
            // Our transmission was successful.
            mTxState = kTxStateHandlingSendDone;
            mPrepareTxFrameTask.Post();
        }
    }

    if ( (aTransactionLength >= 1)
      && (aMISOBufLen >= 1)
    ) {
        // Clear the reset flag.
        spi_header_set_flag_byte(mSendFrame, SPI_PATTERN_VALUE);
        spi_header_set_flag_byte(mEmptySendFrame, SPI_PATTERN_VALUE);
    }

    if (mTxState == kTxStateSending)
    {
        aMISOBuf = mSendFrame;
        aMISOBufLen = mSendFrameLen;
    }
    else
    {
        aMISOBuf = mEmptySendFrame;
        aMISOBufLen = kSpiHeaderLength;
    }

    if (mHandlingRxFrame)
    {
        aMOSIBuf = mEmptyReceiveFrame;
        aMOSIBufLen = kSpiHeaderLength;
        spi_header_set_accept_len(aMISOBuf, 0);
    }
    else
    {
        aMOSIBuf = mReceiveFrame;
        aMOSIBufLen = sizeof(mReceiveFrame);
        spi_header_set_accept_len(aMISOBuf, sizeof(mReceiveFrame) - kSpiHeaderLength);
    }

    otPlatSpiSlavePrepareTransaction(aMISOBuf, aMISOBufLen, aMOSIBuf, aMOSIBufLen, (mTxState == kTxStateSending));
}

void NcpSpi::TxFrameBufferHasData(void *aContext, NcpFrameBuffer *aNcpFrameBuffer)
{
    (void)aNcpFrameBuffer;

    static_cast<NcpSpi *>(aContext)->TxFrameBufferHasData();
}

void NcpSpi::TxFrameBufferHasData(void)
{
    mPrepareTxFrameTask.Post();
}

ThreadError NcpSpi::PrepareNextSpiSendFrame(void)
{
    ThreadError errorCode = kThreadError_None;
    uint16_t frameLength;
    uint16_t readLength;

    VerifyOrExit(!mTxFrameBuffer.IsEmpty());

    SuccessOrExit(errorCode = mTxFrameBuffer.OutFrameBegin());

    frameLength = mTxFrameBuffer.OutFrameGetLength();
    VerifyOrExit(frameLength <= sizeof(mSendFrame) - kSpiHeaderLength, errorCode = kThreadError_NoBufs);

    spi_header_set_data_len(mSendFrame, frameLength);

    // Half-duplex to avoid race condition.
    spi_header_set_accept_len(mSendFrame, 0);

    readLength = mTxFrameBuffer.OutFrameRead(frameLength, mSendFrame + kSpiHeaderLength);
    VerifyOrExit(readLength == frameLength, errorCode = kThreadError_Failed);

    mSendFrameLen = frameLength + kSpiHeaderLength;

    mTxState = kTxStateSending;

    errorCode = otPlatSpiSlavePrepareTransaction(mSendFrame, mSendFrameLen, mEmptyReceiveFrame,
                                                 sizeof(mEmptyReceiveFrame), true);

    if (errorCode == kThreadError_Busy)
    {
        // Being busy is OK. We will get the transaction
        // set up properly when the current transaction
        // is completed.
        errorCode = kThreadError_None;
    }

    if (errorCode != kThreadError_None)
    {
        mTxState = kTxStateIdle;
        mPrepareTxFrameTask.Post();
        ExitNow();
    }

    // Remove the frame from tx buffer and inform the base
    // class that space is now available for a new frame.
    mTxFrameBuffer.OutFrameRemove();
    super_t::HandleSpaceAvailableInTxBuffer();

exit:
    return errorCode;
}

void NcpSpi::PrepareTxFrame(void *aContext)
{
    static_cast<NcpSpi*>(aContext)->PrepareTxFrame();
}

void NcpSpi::PrepareTxFrame(void)
{
    switch (mTxState)
    {
    case kTxStateHandlingSendDone:
        mTxState = kTxStateIdle;

        // Fall-through to next case to prepare the next frame (if any).

    case kTxStateIdle:
        PrepareNextSpiSendFrame();
        break;

    case kTxStateSending:
        // The next frame in queue (if any) will be prepared when the
        // current frame is successfully sent and this task is posted
        // again from the `SpiTransactionComplete()` callback.
        break;
    }
}

void NcpSpi::HandleRxFrame(void *aContext)
{
    static_cast<NcpSpi*>(aContext)->HandleRxFrame();
}

void NcpSpi::HandleRxFrame(void)
{
    uint16_t rx_data_len( spi_header_get_data_len(mReceiveFrame) );
    super_t::HandleReceive(mReceiveFrame + kSpiHeaderLength, rx_data_len);
    mHandlingRxFrame = false;
}

}  // namespace ot

#endif // OPENTHREAD_ENABLE_NCP_SPI
