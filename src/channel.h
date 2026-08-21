/******************************************************************************\
 * Copyright (c) 2004-2026
 *
 * Author(s):
 *  Volker Fischer
 *
 * As of Jamulus 3.12.1dev (commit eb172d47): All new source code contributions must be licensed
 * under AGPL 3.0 or any later version.
 *
 * Existing code: Code contributed before 3.12.1dev (commit eb172d47) was licensed under GPL 2.0+.
 * This code will be licensed under GPL 3.0 (or any later version) from
 * 3.12.1dev (commit eb172d47).  When distributed as part of Jamulus, the AGPL 3.0 terms govern
 * the combined work, including network use provisions.
 *
 ******************************************************************************
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
\******************************************************************************/

#pragma once

#include <QThread>
#include <QDateTime>
#include <QFile>
#include <atomic>
#if QT_VERSION >= QT_VERSION_CHECK( 5, 6, 0 )
#    include <QVersionNumber>
#endif
#include "global.h"
#include "buffer.h"
#include "util.h"
#include "protocol.h"
#include "socket.h"

/* Definitions ****************************************************************/
// set the time-out for the input buffer until the state changes from
// connected to not connected (the actual time depends on the way the error
// correction is implemented)
#define CON_TIME_OUT_SEC_MAX 30 // seconds

// number of frames for audio fade-in, 48 kHz, x samples: 3 sec / (x samples / 48 kHz)
#define FADE_IN_NUM_FRAMES                2250
#define FADE_IN_NUM_FRAMES_DBLE_FRAMESIZE 1125

enum EPutDataStat
{
    PS_GEN_ERROR,
    PS_AUDIO_OK,
    PS_AUDIO_ERR,
    PS_AUDIO_INVALID,
    PS_PROT_OK,
    PS_PROT_OK_MESS_NOT_EVALUATED,
    PS_PROT_ERR,
    PS_NEW_CONNECTION
};

/* Classes ********************************************************************/
class CChannel : public QObject
{
    Q_OBJECT

public:
    // we have to make "server" the default since I do not see a chance to
    // use constructor initialization in the server for a vector of channels
    CChannel ( const bool bNIsServer = true );

    void PutProtocolData ( const int iRecCounter, const int iRecID, const CVector<uint8_t>& vecbyMesBodyData, const CHostAddress& RecHostAddr );

    EPutDataStat PutAudioData ( const CVector<uint8_t>& vecbyData, const int iNumBytes, const CHostAddress& RecHostAddr );

    EGetDataStat GetData ( CVector<uint8_t>& vecbyData, const int iNumBytes );

    void PrepAndSendPacket ( CHighPrioSocket* pSocket, const CVector<uint8_t>& vecbyNPacket, const int iNPacketLen );

    void ResetTimeOutCounter() { iConTimeOut = iConTimeOutStartVal; }
    bool IsConnected() const { return iConTimeOut > 0; }
    bool IsIdentified() const { return bIsIdentified; }
    void Disconnect();

    void SetEnable ( const bool bNEnStat );
    bool IsEnabled() { return bIsEnabled; }

    void                SetAddress ( const CHostAddress& NAddr ) { InetAddr = NAddr; }
    const CHostAddress& GetAddress() const { return InetAddr; }

    void ResetInfo()
    {
        bIsIdentified = false;
        ChannelInfo   = CChannelCoreInfo();
    } // reset does not emit a message
    QString           GetName();
    void              SetChanInfo ( const CChannelCoreInfo& NChanInf );
    CChannelCoreInfo& GetChanInfo() { return ChannelInfo; }

    void SetRemoteInfo ( const CChannelCoreInfo ChInfo ) { Protocol.CreateChanInfoMes ( ChInfo ); }

    void CreateReqChanInfoMes() { Protocol.CreateReqChanInfoMes(); }
    void CreateVersionAndOSMes() { Protocol.CreateVersionAndOSMes(); }
    void CreateMuteStateHasChangedMes ( const int iChanID, const bool bIsMuted ) { Protocol.CreateMuteStateHasChangedMes ( iChanID, bIsMuted ); }

    void  SetGain ( const int iChanID, const float fNewGain );
    float GetGain ( const int iChanID );
    float GetFadeInGain() { return static_cast<float> ( iFadeInCnt ) / iFadeInCntMax; }

    void  SetPan ( const int iChanID, const float fNewPan );
    float GetPan ( const int iChanID );

    void SetRemoteChanGain ( const int iId, const float fGain ) { Protocol.CreateChanGainMes ( iId, fGain ); }

    void SetRemoteChanPan ( const int iId, const float fPan ) { Protocol.CreateChanPanMes ( iId, fPan ); }

    bool SetSockBufNumFrames ( const int iNewNumFrames, const bool bPreserve = false );
    int  GetSockBufNumFrames() const { return iCurSockBufNumFrames; }

    void UpdateSocketBufferSize();

    int GetUploadRateKbps();

    // set/get network out buffer size and size factor
    void SetAudioStreamProperties ( const EAudComprType eNewAudComprType,
                                    const int           iNewNetwFrameSize,
                                    const int           iNewNetwFrameSizeFact,
                                    const int           iNewNumAudioChannels );

    void SetDoAutoSockBufSize ( const bool bValue ) { bDoAutoSockBufSize = bValue; }

    bool GetDoAutoSockBufSize() const { return bDoAutoSockBufSize; }

    int GetNetwFrameSizeFact() const { return iNetwFrameSizeFact; }
    int GetCeltNumCodedBytes() const { return iCeltNumCodedBytes; }

    void GetBufErrorRates ( CVector<double>& vecErrRates, double& dLimit, double& dMaxUpLimit )
    {
        SockBuf.GetErrorRates ( vecErrRates, dLimit, dMaxUpLimit );
    }

    // §105h: raw auto decision before IIR filtering (SockBuf is protected, so this forwards).
    int GetBufPreFilterDecision() { return SockBuf.GetPreFilterDecision(); }

    EAudComprType GetAudioCompressionType() { return eAudioCompressionType; }
    // concealment-rate measurement (PLAN-ADAPTIVE-PLC.md): -1 = no complete window yet
    int GetMeasuredConcealPct() const { return iMeasuredConcealPct.load ( std::memory_order_relaxed ); }
    int GetConcealWindowLen() const { return iConcealWindowLen; }
    // wire loss over the same window (OPEN-TEST-PLANS.md §65). Concealment alone
    // cannot tell a lossy link from a client that stopped sending or one whose
    // packets merely arrive late -- all three underrun the buffer. These two
    // separate them: iMeasuredSeqRecv == 0 is a channel that sent nothing, and
    // a low loss percentage under high concealment is lateness, not loss.
    // -1 = no complete window yet, or nothing received in the window.
    int GetMeasuredSeqLossPct() const { return iMeasuredSeqLossPct.load ( std::memory_order_relaxed ); }
    int GetMeasuredSeqRecv() const { return iMeasuredSeqRecv.load ( std::memory_order_relaxed ); }

    // Telemetry v2 (§105c) -- cumulative, never reset. Difference two samples for an exact total
    // over any interval; no window semantics, nothing lost between reports.
    uint32_t GetCumConcealFails() const { return iCumConcealFails.load ( std::memory_order_relaxed ); }
    uint32_t GetCumConcealBlocks() const { return iCumConcealBlocks.load ( std::memory_order_relaxed ); }
    uint32_t GetCumSeqLost() const { return iCumSeqLost.load ( std::memory_order_relaxed ); }
    uint32_t GetCumSeqSpan() const { return iCumSeqSpan.load ( std::memory_order_relaxed ); }
    uint32_t GetCumSeqReorder() const { return iCumSeqReorder.load ( std::memory_order_relaxed ); }
    uint32_t GetCumRuns() const { return iCumRuns.load ( std::memory_order_relaxed ); }
    uint32_t GetCumRunSum() const { return iCumRunSum.load ( std::memory_order_relaxed ); }
    uint32_t GetCumRunsGE32() const { return iCumRunsGE32.load ( std::memory_order_relaxed ); }
    uint32_t GetCumRunMax() const { return iCumRunMax.load ( std::memory_order_relaxed ); }
    uint32_t GetCumDragBack() const { return iCumDragBack.load ( std::memory_order_relaxed ); }
    uint32_t GetCumDragFwd() const { return iCumDragFwd.load ( std::memory_order_relaxed ); }

    // Telemetry v2 group B: ARRIVAL-GAP HISTOGRAM, 8 buckets, in units of the nominal frame
    // period. This is what lets a depth curve be rebuilt for a real player -- §85 produced one
    // for a single radio in this house and it is the most informative artefact the rig made.
    //
    // Read the buckets as the shape §84c analysed: bucket 0 is a back-to-back arrival (the
    // release half of a stall-and-drain), bucket 1 is on time, and 5..7 are stalls of growing
    // size. NOTE this is an INTER-ARRIVAL gap distribution, not depthcurve.py's "lateness above
    // the median arrival" -- they answer related questions and are not the same number.
    uint32_t GetArrivalBucket ( const int i ) const { return aiArrivalHist[i].load ( std::memory_order_relaxed ); }
    // group C: was anyone actually playing? Concealment during silence is not a defect anyone hears
    uint32_t GetCumAudibleWindows() const { return iCumAudibleWindows.load ( std::memory_order_relaxed ); }
    uint32_t GetCumLevelWindows() const { return iCumLevelWindows.load ( std::memory_order_relaxed ); }
    uint32_t GetPeakLevel() const { return iPeakLevel.load ( std::memory_order_relaxed ); }
    void     NoteLevel ( const uint16_t iLevel )
    {
        iCumLevelWindows.fetch_add ( 1, std::memory_order_relaxed );
        if ( iLevel > 0 )
        {
            iCumAudibleWindows.fetch_add ( 1, std::memory_order_relaxed );
        }
        uint32_t iPrev = iPeakLevel.load ( std::memory_order_relaxed );
        while ( iLevel > iPrev && !iPeakLevel.compare_exchange_weak ( iPrev, iLevel, std::memory_order_relaxed ) )
        {
        }
    }
    // group E: did the client change audio format mid-session?
    uint32_t GetFormatChanges() const { return iFormatChanges.load ( std::memory_order_relaxed ); }

    // Session serial for this channel SLOT, bumped by ResetTelemetryV2 on every new occupant.
    // Slot indices are reused, so ch= alone splices players; (ch, sess) segments them exactly,
    // including a reuse inside one emit interval where fresh-from-zero deltas alone are ambiguous.
    uint32_t GetTelemSession() const { return iTelemSession.load ( std::memory_order_relaxed ); }

    // Called from CServer::InitChannel for EVERY new occupant of this slot -- without it the
    // cumulative counters continue from the previous occupant and the "cumulative since the
    // channel connected" contract does not hold. Runs on the socket thread before the occupant's
    // first Put, but takes MutexSocketBuf anyway so the netbuf reset cannot race a straggling
    // Get/Put touching the old occupant's state.
    void ResetTelemetryV2()
    {
        iTelemSession.fetch_add ( 1, std::memory_order_relaxed );
        iCumConcealFails.store ( 0, std::memory_order_relaxed );
        iCumConcealBlocks.store ( 0, std::memory_order_relaxed );
        iCumSeqLost.store ( 0, std::memory_order_relaxed );
        iCumSeqSpan.store ( 0, std::memory_order_relaxed );
        iCumSeqReorder.store ( 0, std::memory_order_relaxed );
        iCumRuns.store ( 0, std::memory_order_relaxed );
        iCumRunSum.store ( 0, std::memory_order_relaxed );
        iCumRunsGE32.store ( 0, std::memory_order_relaxed );
        iCumRunMax.store ( 0, std::memory_order_relaxed );
        iCumDragBack.store ( 0, std::memory_order_relaxed );
        iCumDragFwd.store ( 0, std::memory_order_relaxed );
        for ( int i = 0; i < iNumArrivalBuckets; i++ )
        {
            aiArrivalHist[i].store ( 0, std::memory_order_relaxed );
        }
        iCumAudibleWindows.store ( 0, std::memory_order_relaxed );
        iCumLevelWindows.store ( 0, std::memory_order_relaxed );
        iPeakLevel.store ( 0, std::memory_order_relaxed );
        iFormatChanges.store ( 0, std::memory_order_relaxed );
        iPrevCodedBytesForTelem = 0;
        iLastArrivalNs          = 0;
        ArrivalTimer.invalidate(); // first gap of the new session must not span the vacancy

        MutexSocketBuf.lock();
        SockBuf.ResetTelemetryForNewConnection();
        MutexSocketBuf.unlock();
    }

    static constexpr int iNumArrivalBuckets = 8;
    bool     GetAutoSockBufSize() const { return bDoAutoSockBufSize; }
    // TEST-ONLY (plc-ab-tester): cumulative-since-connect counterparts
    uint32_t GetConcealFailsCum() const { return iConcealFailsCum.load ( std::memory_order_relaxed ); }
    uint32_t GetConcealBlocksCum() const { return iConcealBlocksCum.load ( std::memory_order_relaxed ); }
    void     ResetConcealCumCounters()
    {
        iConcealFailsCum.store ( 0, std::memory_order_relaxed );
        iConcealBlocksCum.store ( 0, std::memory_order_relaxed );
    }
    void CreatePlcAbTelemetryMes ( const CPlcAbTelemetry& Tlm ) // TEST-ONLY (plc-ab-tester)
    {
        if ( ProtocolIsEnabled() )
        {
            Protocol.CreatePlcAbTelemetryMes ( Tlm );
        }
    }
    int           GetNumAudioChannels() const { return iNumAudioChannels; }

    // network protocol interface
    void CreateJitBufMes ( const int iJitBufSize )
    {
        if ( ProtocolIsEnabled() )
        {
            Protocol.CreateJitBufMes ( iJitBufSize );
        }
    }
    void CreateClientIDMes ( const int iChanID ) { Protocol.CreateClientIDMes ( iChanID ); }
    void CreateRawAudioSupportedMes() { Protocol.CreateRawAudioSupportedMes(); }
    void CreateReqNetwTranspPropsMes() { Protocol.CreateReqNetwTranspPropsMes(); }
    void CreateReqSplitMessSupportMes() { Protocol.CreateReqSplitMessSupportMes(); }
    void CreateReqJitBufMes() { Protocol.CreateReqJitBufMes(); }
    void CreateReqConnClientsList() { Protocol.CreateReqConnClientsList(); }
    void CreateChatTextMes ( const QString& strChatText ) { Protocol.CreateChatTextMes ( strChatText ); }
    void CreateLicReqMes ( const ELicenceType eLicenceType ) { Protocol.CreateLicenceRequiredMes ( eLicenceType ); }

    //### TODO: BEGIN ###//
    // needed for compatibility to old servers >= 3.4.6 and <= 3.5.12
    void CreateReqChannelLevelListMes() { Protocol.CreateReqChannelLevelListMes(); }
    //### TODO: END ###//

    void CreateConClientListMes ( const CVector<CChannelInfo>& vecChanInfo ) { Protocol.CreateConClientListMes ( vecChanInfo ); }

    void CreateRecorderStateMes ( const ERecorderState eRecorderState ) { Protocol.CreateRecorderStateMes ( eRecorderState ); }

    CNetworkTransportProps GetNetworkTransportPropsFromCurrentSettings();

    double UpdateAndGetLevelForMeterdB ( const CVector<short>& vecsAudio, const int iInSize, const bool bIsStereoIn );

protected:
    bool ProtocolIsEnabled();

    void ResetNetworkTransportProperties()
    {
        // set it to a state were no decoding is ever possible (since we want
        // only to decode data when a network transport property message is
        // received with the correct values)
        eAudioCompressionType = CT_NONE;
        iNetwFrameSizeFact    = FRAME_SIZE_FACTOR_PREFERRED;
        iNetwFrameSize        = CELT_MINIMUM_NUM_BYTES;
        iCeltNumCodedBytes    = CELT_MINIMUM_NUM_BYTES;
        iNumAudioChannels     = 1; // mono
        bUseSequenceNumber    = false;

        // discard the concealment measurement: this channel object is reused for
        // the next client on this slot (PLAN-ADAPTIVE-PLC.md Step 1c)
        iConcealWindowCount = 0;
        iConcealFailCount   = 0;
        iMeasuredConcealPct.store ( -1, std::memory_order_relaxed );
        iMeasuredSeqLossPct.store ( -1, std::memory_order_relaxed );
        iMeasuredSeqRecv.store ( 0, std::memory_order_relaxed );
        SockBuf.ResetSeqStats();
        ResetConcealCumCounters();
    }

    // connection parameters
    CHostAddress InetAddr;

    // channel info
    CChannelCoreInfo ChannelInfo;

    // mixer and effect settings
    CVector<float> vecfGains;
    CVector<float> vecfPannings;

    // network jitter-buffer
    CNetBufWithStats SockBuf;
    int              iCurSockBufNumFrames;
    bool             bDoAutoSockBufSize;

    // telemetry v2 cumulative counters -- see the getters above
    std::atomic<uint32_t> iCumConcealFails { 0 };
    std::atomic<uint32_t> iCumConcealBlocks { 0 };
    std::atomic<uint32_t> iCumSeqLost { 0 };
    std::atomic<uint32_t> iCumSeqSpan { 0 };
    std::atomic<uint32_t> iCumSeqReorder { 0 };
    std::atomic<uint32_t> iCumRuns { 0 };
    std::atomic<uint32_t> iCumRunSum { 0 };
    std::atomic<uint32_t> iCumRunsGE32 { 0 };
    std::atomic<uint32_t> iCumRunMax { 0 };
    std::atomic<uint32_t> iCumDragBack { 0 };
    std::atomic<uint32_t> iCumDragFwd { 0 };
    std::atomic<uint32_t> aiArrivalHist[8] {};
    std::atomic<uint32_t> iCumAudibleWindows { 0 };
    std::atomic<uint32_t> iCumLevelWindows { 0 };
    std::atomic<uint32_t> iPeakLevel { 0 };
    std::atomic<uint32_t> iFormatChanges { 0 };
    std::atomic<uint32_t> iTelemSession { 0 }; // bumped per new occupant; see ResetTelemetryV2

    qint64                iLastArrivalNs = 0; // socket thread only
    int                   iPrevCodedBytesForTelem = 0;
    QElapsedTimer         ArrivalTimer;
    bool             bUseSequenceNumber;
    uint8_t          iSendSequenceNumber;

    // network output conversion buffer
    CConvBuf<uint8_t> ConvBuf;

    // network protocol
    CProtocol Protocol;

    std::atomic<int> iConTimeOut;
    int              iConTimeOutStartVal;
    int              iFadeInCnt;
    int              iFadeInCntMax;

    std::atomic<bool> bIsEnabled;
    bool              bIsServer;
    bool              bIsIdentified;

    int iNetwFrameSizeFact;
    int iNetwFrameSize;
    int iCeltNumCodedBytes;
    int iAudioFrameSizeSamples;

    EAudComprType eAudioCompressionType;
    int           iNumAudioChannels;

    // concealment-rate measurement (PLAN-ADAPTIVE-PLC.md)
    int              iConcealWindowLen   = CONCEAL_WINDOW_BLOCKS; // per frame size
    int              iConcealWindowCount = 0;                     // blocks seen in the current window
    int              iConcealFailCount   = 0;                     // of those, blocks the buffer could not supply
    std::atomic<int> iMeasuredConcealPct { -1 };                  // -1 = no complete window yet
    std::atomic<int> iMeasuredSeqLossPct { -1 };                  // §65: wire loss over the same window
    std::atomic<int> iMeasuredSeqRecv { 0 };                      // §65: packets that actually arrived in it
    // TEST-ONLY (plc-ab-tester): cumulative counterparts, read off-thread by telemetry
    std::atomic<uint32_t> iConcealFailsCum { 0 };
    std::atomic<uint32_t> iConcealBlocksCum { 0 };

    QMutex Mutex;
    QMutex MutexSocketBuf;
    QMutex MutexConvBuf;

    CStereoSignalLevelMeter SignalLevelMeter;

public slots:
    void OnSendProtMessage ( CVector<uint8_t> vecMessage );
    void OnJittBufSizeChange ( int iNewJitBufSize );
    void OnChangeChanGain ( int iChanID, float fNewGain );
    void OnChangeChanPan ( int iChanID, float fNewPan );
    void OnChangeChanInfo ( CChannelCoreInfo ChanInfo );
    void OnNetTranspPropsReceived ( CNetworkTransportProps NetworkTransportProps );
    void OnReqNetTranspProps();
    void OnReqSplitMessSupport();
    void OnSplitMessSupported() { Protocol.SetSplitMessageSupported ( true ); }

    void OnVersionAndOSReceived ( COSUtil::EOpSystemType eOSType, QString strVersion );

    void OnParseMessageBody ( CVector<uint8_t> vecbyMesBodyData, int iRecCounter, int iRecID )
    {
        // note that the return value is ignored here
        Protocol.ParseMessageBody ( vecbyMesBodyData, iRecCounter, iRecID );
    }

    void OnProtocolMessageReceived ( int iRecCounter, int iRecID, CVector<uint8_t> vecbyMesBodyData, CHostAddress RecHostAddr )
    {
        PutProtocolData ( iRecCounter, iRecID, vecbyMesBodyData, RecHostAddr );
    }

    void OnProtocolCLMessageReceived ( int iRecID, CVector<uint8_t> vecbyMesBodyData, CHostAddress RecHostAddr )
    {
        emit DetectedCLMessage ( vecbyMesBodyData, iRecID, RecHostAddr );
    }

    void OnNewConnection() { emit NewConnection(); }

signals:
    void MessReadyForSending ( CVector<uint8_t> vecMessage );
    void NewConnection();
    void ReqJittBufSize();
    void JittBufSizeChanged ( int iNewJitBufSize );
    void ServerAutoSockBufSizeChange ( int iNNumFra );
    void ReqConnClientsList();
    void ConClientListMesReceived ( CVector<CChannelInfo> vecChanInfo );
    void ChanInfoHasChanged();
    void ClientIDReceived ( int iChanID );
    void RawAudioSupported();
    void MuteStateHasChanged ( int iChanID, bool bIsMuted );
    void MuteStateHasChangedReceived ( int iChanID, bool bIsMuted );
    void ReqChanInfo();
    void ChatTextReceived ( QString strChatText );
    void PlcAbTelemetryReceived ( QString strFields ); // TEST-ONLY (plc-ab-tester)
    void ReqNetTranspProps();
    void LicenceRequired ( ELicenceType eLicenceType );
    void VersionAndOSReceived ( COSUtil::EOpSystemType eOSType, QString strVersion );
    void RecorderStateReceived ( ERecorderState eRecorderState );
    void Disconnected();

    void DetectedCLMessage ( CVector<uint8_t> vecbyMesBodyData, int iRecID, CHostAddress RecHostAddr );

    void ParseMessageBody ( CVector<uint8_t> vecbyMesBodyData, int iRecCounter, int iRecID );
};
