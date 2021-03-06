/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

--*/

#include <time.h>
#include <stdio.h>

#include <vector>
#include <map>
#include <mutex>
#include <algorithm>

#include <quic_datapath.h>
#include <msquichelper.h>

uint16_t Port = 443;
const char* ServerName = "localhost";
const char* ServerIp = nullptr;
QUIC_ADDR ServerAddress = {0};
std::vector<const char*> ALPNs(
    { "h3-25", "h3-26", "h3-27",
      "hq-25", "hq-26", "hq-27",
      "smb" });

const QUIC_API_TABLE* MsQuic;
HQUIC Registration;

extern "C" void QuicTraceRundown(void) { }

_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
ConnectionHandler(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    bool* GotConnected = (bool*)Context;
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        *GotConnected = true;
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        MsQuic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        return QUIC_STATUS_NOT_SUPPORTED;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_THREAD_CALLBACK(TestReachability, Context)
{
    QUIC_BUFFER Alpn;
    Alpn.Buffer = (uint8_t*)Context;
    Alpn.Length = (uint32_t)strlen((char*)Context);

    HQUIC Session = nullptr;
    if (QUIC_FAILED(MsQuic->SessionOpen(Registration, &Alpn, 1, nullptr, &Session))) {
        printf("SessionOpen failed.\n");
        exit(1);
    }

    HQUIC Connection = nullptr;
    bool GotConnected = false;
    if (QUIC_FAILED(MsQuic->ConnectionOpen(Session, ConnectionHandler, &GotConnected, &Connection))) {
        printf("ConnectionOpen failed.\n");
        exit(1);
    }

    if (QUIC_FAILED(MsQuic->SetParam(Connection, QUIC_PARAM_LEVEL_CONNECTION, QUIC_PARAM_CONN_REMOTE_ADDRESS, sizeof(ServerAddress), &ServerAddress))) {
        printf("SetParam QUIC_PARAM_CONN_REMOTE_ADDRESS failed.\n");
        exit(1);
    }

    uint16_t StreamCount = 100;
    if (QUIC_FAILED(MsQuic->SetParam(Connection, QUIC_PARAM_LEVEL_CONNECTION, QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT, sizeof(StreamCount), &StreamCount))) {
        printf("SetParam QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT failed.\n");
        exit(1);
    }

    uint64_t IdleTimeoutMs = 10 * 1000;
    if (QUIC_FAILED(MsQuic->SetParam(Connection, QUIC_PARAM_LEVEL_CONNECTION, QUIC_PARAM_CONN_IDLE_TIMEOUT, sizeof(IdleTimeoutMs), &IdleTimeoutMs))) {
        printf("SetParam QUIC_PARAM_CONN_IDLE_TIMEOUT failed.\n");
        exit(1);
    }

    if (QUIC_FAILED(MsQuic->ConnectionStart(Connection, AF_UNSPEC, ServerName, Port))) {
        printf("ConnectionStart failed.\n");
        exit(1);
    }

    MsQuic->SessionClose(Session);

    if (GotConnected) {
        printf("  %6s    reachable\n", (char*)Context);
    } else {
        printf("  %6s  unreachable\n", (char*)Context);
    }

    QUIC_THREAD_RETURN(0);
}

int
QUIC_MAIN_EXPORT
main(int argc, char **argv)
{
    if (argc > 1 &&
        (
            !strcmp(argv[1], "?") ||
            !strcmp(argv[1], "-?") ||
            !strcmp(argv[1], "--?") ||
            !strcmp(argv[1], "/?") ||
            !strcmp(argv[1], "help")
        )) {
        printf("Usage: quicreach.exe [-server:<name>] [-ip:<ip>] [-port:<number>]\n");
        exit(1);
    }

    TryGetValue(argc, argv, "server", &ServerName);
    TryGetValue(argc, argv, "ip", &ServerIp);
    TryGetValue(argc, argv, "port", &Port);

    QuicPlatformSystemLoad();
    QuicPlatformInitialize();

    if (ServerIp == nullptr) {
        QUIC_DATAPATH* Datapath = nullptr;
        if (QUIC_FAILED(
            QuicDataPathInitialize(
                0,
                (QUIC_DATAPATH_RECEIVE_CALLBACK_HANDLER)(1),
                (QUIC_DATAPATH_UNREACHABLE_CALLBACK_HANDLER)(1),
                &Datapath))) {
            printf("QuicDataPathInitialize failed.\n");
            exit(1);
        }
        if (QUIC_FAILED(
            QuicDataPathResolveAddress(
                Datapath,
                ServerName,
                &ServerAddress))) {
            printf("Failed to resolve IP address of '%s'.\n", ServerName);
            exit(1);
        }
        QuicDataPathUninitialize(Datapath);
    } else {
        if (!QuicAddrFromString(ServerIp, Port, &ServerAddress)) {
            printf("QuicAddrFromString failed.\n");
            exit(1);
        }
    }

    if (QUIC_FAILED(MsQuicOpen(&MsQuic))) {
        printf("MsQuicOpen failed.\n");
        exit(1);
    }

    const QUIC_REGISTRATION_CONFIG RegConfig = { "reach", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed.\n");
        exit(1);
    }

    printf("\n%s:%hu:\n\n", ServerName, Port);

    std::vector<QUIC_THREAD> Threads;
    QUIC_THREAD_CONFIG Config = { 0, 0, "reach_worker", TestReachability, nullptr };

    for (auto ALPN : ALPNs) {
        Config.Context = (void*)ALPN;
        QUIC_THREAD Thread;
        if (QUIC_FAILED(QuicThreadCreate(&Config, &Thread))) {
            printf("QuicThreadCreate failed.\n");
            exit(1);
        }
        Threads.push_back(Thread);
    }

    for (auto Thread : Threads) {
        QuicThreadWait(&Thread);
        QuicThreadDelete(&Thread);
    }

    MsQuic->RegistrationClose(Registration);

    MsQuicClose(MsQuic);

    QuicPlatformUninitialize();
    QuicPlatformSystemUnload();

    return 0;
}
