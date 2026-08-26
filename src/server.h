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

#include <QObject>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <functional>
#ifdef USE_OPUS_SHARED_LIB
#    include "opus/opus_custom.h"
#else
#    include "opus_custom.h"
#endif
#include "global.h"
#include "buffer.h"
#include "signalhandler.h"
#include "socket.h"
#include "channel.h"
#include "util.h"
#include "serverlogging.h"
#include "serverlist.h"
#include "recorder/jamcontroller.h"

#include "threadpool.h"
#include "centraldefense.h"

/* Definitions ****************************************************************/
// no valid channel number
#define INVALID_CHANNEL_ID ( MAX_NUM_CHANNELS + 1 )

/* Classes ********************************************************************/
class CentralDefense;
class ChatReporter;

template<unsigned int slotId>
class CServerSlots : public CServerSlots<slotId - 1>
{
public:
    void OnSendProtMessCh ( CVector<uint8_t> mess ) { SendProtMessage ( slotId - 1, mess ); }
    void OnReqConnClientsListCh() { CreateAndSendChanListForThisChan ( slotId - 1 ); }

    void OnChatTextReceivedCh ( QString strChatText ) { CreateAndSendChatTextForAllConChannels ( slotId - 1, strChatText ); }

    // TEST-ONLY (plc-ab-tester)
    void OnPlcAbTelemetryReceivedCh ( QString strFields ) { LogPlcAbTelemetry ( slotId - 1, strFields ); }

    // TEST-ONLY (client-telemetry, Step 1)
    void OnClientTelemetryReceivedCh ( QString strFields ) { LogClientTelemetry ( slotId - 1, strFields ); }

    void OnMuteStateHasChangedCh ( int iChanID, bool bIsMuted ) { CreateOtherMuteStateChanged ( slotId - 1, iChanID, bIsMuted ); }

    void OnServerAutoSockBufSizeChangeCh ( int iNNumFra ) { CreateAndSendJitBufMessage ( slotId - 1, iNNumFra ); }

protected:
    virtual void SendProtMessage ( int iChID, CVector<uint8_t> vecMessage ) = 0;

    virtual void CreateAndSendChanListForThisChan ( const int iCurChanID ) = 0;

    virtual void CreateAndSendChatTextForAllConChannels ( const int iCurChanID, const QString& strChatText ) = 0;

    virtual void LogPlcAbTelemetry ( const int iCurChanID, const QString& strFields ) = 0; // TEST-ONLY (plc-ab-tester)
    virtual void LogClientTelemetry ( const int iCurChanID, const QString& strFields ) = 0; // TEST-ONLY (client-telemetry, Step 1)

    virtual void CreateOtherMuteStateChanged ( const int iCurChanID, const int iOtherChanID, const bool bIsMuted ) = 0;

    virtual void CreateAndSendJitBufMessage ( const int iCurChanID, const int iNNumFra ) = 0;
};

template<>
class CServerSlots<0>
{};

class CServer : public QObject, public CServerSlots<MAX_NUM_CHANNELS>
{
    Q_OBJECT

public:
    CServer ( const int          iNewMaxNumChan,
              const QString&     strLoggingFileName,
              const QString&     strServerBindIP,
              const quint16      iPortNumber,
              const quint16      iQosNumber,
              const QString&     strHTMLStatusFileName,
              const QString&     strDirectoryAddress,
              const QString&     strServerListFileName,
              const QString&     strServerInfo,
              const QString&     strServerListFilter,
              const QString&     strServerPublicIP,
              const QString&     strNewWelcomeMessage,
              const QString&     strRecordingDirName,
              const bool         bNDisconnectAllClientsOnQuit,
              const bool         bNUseDoubleSystemFrameSize,
              const bool         bNDisableRaw,
              const bool         bNRecordRawOnly,
              const bool         bNUseMultithreading,
              const bool         bDisableRecording,
              const bool         bNDelayPan,
              const bool         bNEnableIPv6,
              const ELicenceType eNLicenceType );

    virtual ~CServer();

    void Start();
    void Stop();
    bool IsRunning() { return HighPrecisionTimer.isActive(); }

    bool PutAudioData ( const CVector<uint8_t>& vecbyRecBuf, const int iNumBytesRead, const CHostAddress& HostAdr, int& iCurChanID );

    int GetNumberOfConnectedClients();

    void GetConCliParam ( CVector<CHostAddress>&     vecHostAddresses,
                          CVector<QString>&          vecsName,
                          CVector<int>&              veciJitBufNumFrames,
                          CVector<int>&              veciNetwFrameSizeFact,
                          CVector<CChannelCoreInfo>& vecChanInfo );

    void CreateCLServerListReqVerAndOSMes ( const CHostAddress& InetAddr ) { ConnLessProtocol.CreateCLReqVersionAndOSMes ( InetAddr ); }

    // IPv6 Enabled
    bool IsIPv6Enabled() { return bEnableIPv6; }

    // GUI settings ------------------------------------------------------------
    int GetClientNumAudioChannels ( const int iChanNum ) { return vecChannels[iChanNum].GetNumAudioChannels(); }

    EAudComprType GetClientAudioCompressionType ( const int iChanNum ) { return vecChannels[iChanNum].GetAudioCompressionType(); }

    void           SetDirectoryType ( const EDirectoryType eNCSAT ) { ServerListManager.SetDirectoryType ( eNCSAT ); }
    EDirectoryType GetDirectoryType() { return ServerListManager.GetDirectoryType(); }
    bool           IsDirectory() { return ServerListManager.IsDirectory(); }
    ESvrRegStatus  GetSvrRegStatus() { return ServerListManager.GetSvrRegStatus(); }

    void             SetServerName ( const QString& strNewName ) { ServerListManager.SetServerName ( strNewName ); }
    QString          GetServerName() { return ServerListManager.GetServerName(); }
    void             SetServerCity ( const QString& strNewCity ) { ServerListManager.SetServerCity ( strNewCity ); }
    QString          GetServerCity() { return ServerListManager.GetServerCity(); }
    void             SetServerCountry ( const QLocale::Country eNewCountry ) { ServerListManager.SetServerCountry ( eNewCountry ); }
    QLocale::Country GetServerCountry() { return ServerListManager.GetServerCountry(); }

    bool    GetRecorderInitialised() { return JamController.GetRecorderInitialised(); }
    void    SetEnableRecording ( bool bNewEnableRecording );
    bool    GetDisableRecording() { return bDisableRecording; }
    QString GetRecorderErrMsg() { return JamController.GetRecorderErrMsg(); }
    bool    GetRecordingEnabled() { return JamController.GetRecordingEnabled(); }
    void    RequestNewRecording() { JamController.RequestNewRecording(); }
    void    SetRecordingDir ( QString newRecordingDir )
    {
        JamController.SetRecordingDir ( newRecordingDir, iServerFrameSizeSamples, bDisableRecording );
    }
    QString GetRecordingDir() { return JamController.GetRecordingDir(); }

    void SetExternalRecordingBanner ( bool bActive );
    bool GetExternalRecordingBanner() { return m_bExternalRecordingBanner; }

    void    SetWelcomeMessage ( const QString& strNWelcMess );
    QString GetWelcomeMessage() { return strWelcomeMessage; }

    void    SetDirectoryAddress ( const QString& sNDirectoryAddress ) { ServerListManager.SetDirectoryAddress ( sNDirectoryAddress ); }
    QString GetDirectoryAddress() { return ServerListManager.GetDirectoryAddress(); }

    QString GetServerListFileName() { return ServerListManager.GetServerListFileName(); }
    bool    SetServerListFileName ( QString strFilename ) { return ServerListManager.SetServerListFileName ( strFilename ); }

    void SetAutoRunMinimized ( const bool NAuRuMin ) { bAutoRunMinimized = NAuRuMin; }
    bool GetAutoRunMinimized() { return bAutoRunMinimized; }

    void SetEnableDelayPanning ( bool bDelayPanningOn ) { bDelayPan = bDelayPanningOn; }
    bool IsDelayPanningEnabled() { return bDelayPan; }

    CServerLogging* GetLogging() { return &Logging; }
    bool CentralDefenseAllows(const QHostAddress& addr) { return !m_centralDefense || m_centralDefense->shouldAllow(addr); }

    void SendChatToChannel ( const int iChanNum, const QString& strMsg );
    void BroadcastChatMessage ( const QString& strMsg );
    void SetChatReporterWelcomeCallback ( std::function<void(int, const QString&)> cb );
    void SetChatReporterRpcDispatch ( std::function<QString(const QJsonObject&)> cb );

protected:
    // access functions for actual channels
    bool IsConnected ( const int iChanNum ) { return vecChannels[iChanNum].IsConnected(); }

    int                   FindChannel ( const CHostAddress& CheckAddr, const bool bAllowNew = false );
    void                  InitChannel ( const int iNewChanID, const CHostAddress& InetAddr );
    void                  FreeChannel ( const int iCurChanID );
    void                  DumpChannels ( const QString& title );
    CVector<CChannelInfo> CreateChannelList();

    virtual void CreateAndSendChanListForAllConChannels();
    virtual void CreateAndSendChanListForThisChan ( const int iCurChanID );

    virtual void CreateAndSendChatTextForAllConChannels ( const int iCurChanID, const QString& strChatText );

    virtual void LogPlcAbTelemetry ( const int iCurChanID, const QString& strFields ); // TEST-ONLY (plc-ab-tester)
    virtual void LogClientTelemetry ( const int iCurChanID, const QString& strFields ); // TEST-ONLY (client-telemetry, Step 1)
    void BroadcastServerMessage ( const QString& text );

    virtual void CreateOtherMuteStateChanged ( const int iCurChanID, const int iOtherChanID, const bool bIsMuted );

    virtual void CreateAndSendJitBufMessage ( const int iCurChanID, const int iNNumFra );

    virtual void SendProtMessage ( int iChID, CVector<uint8_t> vecMessage );

    template<unsigned int slotId>
    inline void connectChannelSignalsToServerSlots();

    void WriteHTMLChannelList();
    void WriteHTMLServerQuit();

    static void DecodeReceiveDataBlocks ( CServer* pServer, const int iStartChanCnt, const int iStopChanCnt, const int iNumClients );

    static void MixEncodeTransmitDataBlocks ( CServer* pServer, const int iStartChanCnt, const int iStopChanCnt, const int iNumClients );

    void DecodeReceiveData ( const int iChanCnt, const int iNumClients );

    void MixEncodeTransmitData ( const int iChanCnt, const int iNumClients );

    virtual void customEvent ( QEvent* pEvent );

    void CreateAndSendRecorderStateForAllConChannels();

    // if server mode is normal or double system frame size
    bool bUseDoubleSystemFrameSize;
    int  iServerFrameSizeSamples;

    // variables needed for multithreading support
    bool                       bUseMultithreading;
    int                        iMaxNumThreads;
    CVector<std::future<void>> Futures;

    bool CreateLevelsForAllConChannels ( const int iNumClients );

    // do not use the vector class since CChannel does not have appropriate
    // copy constructor/operator
    CChannel vecChannels[MAX_NUM_CHANNELS];
    int      iMaxNumChannels;
    quint16  m_iPort;

    int    iCurNumChannels;
    int    vecChannelOrder[MAX_NUM_CHANNELS];
    QMutex MutexChanOrder;

    CProtocol         ConnLessProtocol;
    QMutex            Mutex;
    QMutex            MutexWelcomeMessage;
    std::atomic<bool> bChannelIsNowDisconnected;

    // audio encoder/decoder
    OpusCustomMode*    Opus64Mode[MAX_NUM_CHANNELS];
    OpusCustomEncoder* Opus64EncoderMono[MAX_NUM_CHANNELS];
    OpusCustomDecoder* Opus64DecoderMono[MAX_NUM_CHANNELS];
    OpusCustomEncoder* Opus64EncoderStereo[MAX_NUM_CHANNELS];
    OpusCustomDecoder* Opus64DecoderStereo[MAX_NUM_CHANNELS];
    OpusCustomMode*    OpusMode[MAX_NUM_CHANNELS];
    OpusCustomEncoder* OpusEncoderMono[MAX_NUM_CHANNELS];
    OpusCustomDecoder* OpusDecoderMono[MAX_NUM_CHANNELS];
    OpusCustomEncoder* OpusEncoderStereo[MAX_NUM_CHANNELS];
    OpusCustomDecoder* OpusDecoderStereo[MAX_NUM_CHANNELS];
    CConvBuf<int16_t>  DoubleFrameSizeConvBufIn[MAX_NUM_CHANNELS];
    CConvBuf<int16_t>  DoubleFrameSizeConvBufOut[MAX_NUM_CHANNELS];

    // needed for disabling raw audio transmission
    bool bDisableRaw;

    CVector<QString> vstrChatColors;
    CVector<int>     vecChanIDsCurConChan;

    CVector<CVector<float>>   vecvecfGains;
    CVector<CVector<float>>   vecvecfPannings;
    CVector<CVector<int16_t>> vecvecsData;
    CVector<CVector<int16_t>> vecvecsData2;
    CVector<int>              vecNumAudioChannels;
    CVector<int>              vecNumFrameSizeConvBlocks;
    CVector<int>              vecUseDoubleSysFraSizeConvBuf;
    CVector<EAudComprType>    vecAudioComprType;

    // Per-channel raw(PCM)-audio flag, indexed by channel ID. The server already derives this
    // on every decoded block (by payload size) and used to discard it; telemetry v2's raw=
    // field reads it. Kept as int, not bool: CVector is used with plain value types throughout.
    CVector<int> vecChanIsRawAudio;

    // --recordrawonly: when true, only raw(PCM) channels are handed to the recorder.
    bool bRecordRawOnly;

    // What THIS channel should be told the recorder is doing. Under --recordrawonly an OPUS
    // channel is never written, so it must never be shown the recording banner either -- the
    // banner is a statement about that user, not about the server. Returns the server-wide
    // state for every other configuration, so default behaviour is untouched.
    ERecorderState RecorderStateForChannel ( const int iChID );
    void           SendRecorderStateToChannel ( const int iChID );
    CVector<CVector<int16_t>> vecvecsSendData;
    CVector<CVector<float>>   vecvecfIntermediateProcBuf;
    CVector<CVector<uint8_t>> vecvecbyCodedData;

    // Channel levels
    CVector<uint16_t> vecChannelLevels;

    // actual working objects
    CHighPrioSocket Socket;

    // logging
    CServerLogging Logging;

    // channel level update frame interval counter
    int iFrameCount;

    // HTML file server status
    bool    bWriteStatusHTMLFile;
    QString strServerHTMLFileListName;

    CHighPrecisionTimer HighPrecisionTimer;

    // Main-thread poller that drives telemetry v2 (was PLAN-ADAPTIVE-PLC.md Step 1d's
    // concealment-rate logger; that log line was removed 2026-08-20 -- see WriteTelemetryV2).
    // Kept off the audio thread entirely: GetData() only writes atomics.
    QTimer ConcealTelemetryTimer;

    // Telemetry v2 (OPEN-TEST-PLANS.md §105c): a per-channel record every 30 s to a dedicated
    // file, with a disk cap this server enforces itself. RULE B: a quotient/window sample (e.g.
    // w=, TELEMETRY-PLAN.md Phase 0) may be added BESIDE an intact raw cumulative count, never
    // IN PLACE OF one -- §105b failed precisely because a rounded per-window rate replaced the
    // raw counters and could not be reconstructed after the fact.
    void     WriteTelemetryV2();
    bool     TelemetryV2SpaceOk();
    int      iTelemV2TickCount   = 0;
    qint64   iTelemV2CapBytes    = 0; // decided once at startup from actual free space
    bool     bTelemV2Suspended   = false;
    // TELEMETRY-PLAN.md Phase 1: count of WriteTelemetryV2() calls that returned without writing
    // a record (suspended, or the file failed to open) — emitted as skip= in s2.
    quint64  iTelemV2SkippedPasses = 0;
    // §105h auto-jitter diagnostic. Read ONCE at startup from JAMULUS_TELEMETRY_AUTODIAG so
    // the audio path never touches the environment; default OFF, so a normal fleet build
    // emits the unchanged record.
    bool     bTelemV2AutoDiag    = false;
    int      iTelemV2HighWater   = 0;
    // srv= process instance id: this server process's start time, epoch seconds, stamped once.
    // sess= (channel.h iTelemSession) is a per-CChannel member starting at 0, so every restart
    // replays sess=1,2,3... on the same slots and two different sessions collide on one
    // (host, port, ch, sess) key -- measured 2026-08-24 on 14 of 215 keys in the pulled corpus,
    // where it shows up as a cumulative counter going BACKWARDS. Nothing else in the record
    // identified the process (the startup lines go to the journal, not the log), so consumers
    // had to infer restarts by watching for counter resets. With srv= the key is unique by
    // construction, and restarts become directly countable. A start time rather than a random
    // id so the value is also readable: it gives process uptime for free.
    qint64   iTelemV2ServerId    = 0;
    QElapsedTimer TickTimer;
    qint64   iLastTickNs         = 0;
    quint64  iTelemV2Ticks       = 0;
    quint64  iTelemV2TicksLate1ms = 0;
    qint64   iTelemV2TickMaxLateUs = 0;
    QString  strTelemV2Path;
    QTimer              TimerCapacityLog;

    // server list
    CServerListManager ServerListManager;

    // jam recorder
    recorder::CJamController JamController;
    bool                     bDisableRecording;

    // GUI settings
    bool bAutoRunMinimized;

    // for delay panning
    bool bDelayPan;

    // enable IPv6
    bool bEnableIPv6;

    // messaging
    QString      strWelcomeMessage;
    ELicenceType eLicenceType;
    bool         bDisconnectAllClientsOnQuit;

    CSignalHandler* pSignalHandler;

    std::unique_ptr<CThreadPool> pThreadPool;

    CentralDefense* m_centralDefense = nullptr;
    ChatReporter*   m_chatReporter   = nullptr;
    bool            m_vecChanInfoReported[MAX_NUM_CHANNELS] = {};

    bool m_bExternalRecordingBanner = false;

signals:
    void Started();
    void Stopped();
    void ClientDisconnected ( const int iChID );
    void SvrRegStatusChanged();
    void AudioFrame ( const int              iChID,
                      const QString          stChName,
                      const CHostAddress     RecHostAddr,
                      const int              iNumAudChan,
                      const CVector<int16_t> vecsData );

    void CLVersionAndOSReceived ( CHostAddress InetAddr, COSUtil::EOpSystemType eOSType, QString strVersion );

    // pass through from jam controller
    void RestartRecorder();
    void StopRecorder();
    void RecordingSessionStarted ( QString sessionDir );
    void EndRecorderThread();

public slots:
    void OnTimer();

    void OnConcealTelemetryTimer();
    void OnTimerCapacityLog();

    void OnNewConnection ( int iChID, int iTotChans, CHostAddress RecHostAddr );

    void OnServerFull ( CHostAddress RecHostAddr );

    void OnSendCLProtMessage ( CHostAddress InetAddr, CVector<uint8_t> vecMessage );

    void OnProtocolCLMessageReceived ( int iRecID, CVector<uint8_t> vecbyMesBodyData, CHostAddress RecHostAddr );

    void OnProtocolMessageReceived ( int iRecCounter, int iRecID, CVector<uint8_t> vecbyMesBodyData, CHostAddress RecHostAddr );

    void OnCLPingReceived ( CHostAddress InetAddr, int iMs ) { ConnLessProtocol.CreateCLPingMes ( InetAddr, iMs ); }

    void OnCLPingWithNumClientsReceived ( CHostAddress InetAddr, int iMs, int )
    {
        ConnLessProtocol.CreateCLPingWithNumClientsMes ( InetAddr, iMs, GetNumberOfConnectedClients() );
    }

    void OnCLSendEmptyMes ( CHostAddress TargetInetAddr )
    {
        // only send empty message if not a directory
        if ( !ServerListManager.IsDirectory() )
        {
            ConnLessProtocol.CreateCLEmptyMes ( TargetInetAddr );
        }
    }

    void OnCLReqServerList ( CHostAddress InetAddr ) { ServerListManager.RetrieveAll ( InetAddr ); }

    void OnCLReqVersionAndOS ( CHostAddress InetAddr ) { ConnLessProtocol.CreateCLVersionAndOSMes ( InetAddr ); }

    void OnCLReqConnClientsList ( CHostAddress InetAddr ) { ConnLessProtocol.CreateCLConnClientsListMes ( InetAddr, CreateChannelList() ); }

    void OnCLReqChannelLevelList ( CHostAddress InetAddr ) { ConnLessProtocol.CreateCLChannelLevelListMes ( InetAddr, vecChannelLevels, GetNumberOfConnectedClients() ); }

    void OnCLRegisterServerReceived ( CHostAddress InetAddr, CHostAddress LInetAddr, CServerCoreInfo ServerInfo )
    {
        ServerListManager.Append ( InetAddr, LInetAddr, ServerInfo );
    }

    void OnCLRegisterServerExReceived ( CHostAddress    InetAddr,
                                        CHostAddress    LInetAddr,
                                        CServerCoreInfo ServerInfo,
                                        COSUtil::EOpSystemType,
                                        QString strVersion )
    {
        ServerListManager.Append ( InetAddr, LInetAddr, ServerInfo, strVersion );
    }

    void OnCLRegisterServerResp ( CHostAddress /* unused */, ESvrRegResult eResult ) { ServerListManager.StoreRegistrationResult ( eResult ); }

    void OnCLUnregisterServerReceived ( CHostAddress InetAddr ) { ServerListManager.Remove ( InetAddr ); }

    void OnCLDisconnection ( CHostAddress InetAddr );

    void OnAboutToQuit();

    void OnHandledSignal ( int sigNum );
};

Q_DECLARE_METATYPE ( CVector<int16_t> )
