#pragma once

#include <array>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>

using Socket = SOCKET;
using SocketLength = int;
static const Socket BAD_SOCKET = INVALID_SOCKET;

#else

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <signal.h>

using Socket = int;
using SocketLength = socklen_t;
static const Socket BAD_SOCKET = -1;

#endif

constexpr int32_t MAX_VARS_PER_FRAME = 16;
constexpr int32_t MAX_STACK_DEPTH = 64;

constexpr uint64_t MAX_SOURCE_BYTES = 15ULL * 1024ULL * 1024ULL;
constexpr size_t IO_BUFFER_SIZE = 64U * 1024U;

constexpr int32_t TTDB_FORMAT_VERSION = 1;
constexpr int32_t MAX_TDBG_STRING = 15 * 1024 * 1024;


struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin;
};


struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    std::string targetFuncName;
};


struct Variable
{
    std::string name;
    int32_t value;
};


struct Frame
{
    std::string func_name;
    int32_t argc;

    Variable argv[MAX_VARS_PER_FRAME];

    int32_t returnLine;

    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};


struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};


struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};


inline bool networkInitialize()
{
#ifdef _WIN32

    WSADATA data;

    return WSAStartup(MAKEWORD(2, 2), &data) == 0;

#else

    return true;

#endif
}


inline void networkCleanup()
{
#ifdef _WIN32

    WSACleanup();

#endif
}


inline void closeSocket(Socket sock)
{
    if (sock == BAD_SOCKET)
        return;

#ifdef _WIN32

    closesocket(sock);

#else

    close(sock);

#endif
}


inline void shutdownSocketWrite(Socket sock)
{
    if (sock == BAD_SOCKET)
        return;

#ifdef _WIN32

    shutdown(sock, SD_SEND);

#else

    shutdown(sock, SHUT_WR);

#endif
}


inline void shutdownSocketBoth(Socket sock)
{
    if (sock == BAD_SOCKET)
        return;

#ifdef _WIN32

    shutdown(sock, SD_BOTH);

#else

    shutdown(sock, SHUT_RDWR);

#endif
}


inline bool socketInterrupted()
{
#ifdef _WIN32

    return WSAGetLastError() == WSAEINTR;

#else

    return errno == EINTR;

#endif
}


inline std::string socketErrorText()
{
#ifdef _WIN32

    return std::string("Winsock error ") +
           std::to_string(WSAGetLastError());

#else

    return std::strerror(errno);

#endif
}


inline bool setSocketTimeouts(Socket sock, int seconds)
{
#ifdef _WIN32

    DWORD timeout = static_cast<DWORD>(seconds * 1000);

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout)) != 0)
        return false;

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout)) != 0)
        return false;

#else

    timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) != 0)
        return false;

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) != 0)
        return false;

#endif

    return true;
}


inline int receiveSome(Socket sock, void *buffer, size_t capacity)
{
    if (capacity == 0)
        return 0;

    if (capacity > static_cast<size_t>(INT_MAX))
        capacity = INT_MAX;

#ifdef _WIN32

    return recv(sock,
                static_cast<char *>(buffer),
                static_cast<int>(capacity),
                0);

#else

    ssize_t received = recv(sock, buffer, capacity, 0);

    if (received > INT_MAX)
        return INT_MAX;

    return static_cast<int>(received);

#endif
}


inline bool sendAllBytes(Socket sock, const void *buffer, size_t length)
{
    const unsigned char *bytes =
        static_cast<const unsigned char *>(buffer);

    size_t sentTotal = 0;

    while (sentTotal < length)
    {
        size_t remaining = length - sentTotal;

        if (remaining > static_cast<size_t>(INT_MAX))
            remaining = INT_MAX;

#ifdef _WIN32

        int sent = send(
            sock,
            reinterpret_cast<const char *>(bytes + sentTotal),
            static_cast<int>(remaining),
            0
        );

#else

#ifdef MSG_NOSIGNAL

        int sent = static_cast<int>(
            send(sock, bytes + sentTotal, remaining, MSG_NOSIGNAL)
        );

#else

        int sent = static_cast<int>(
            send(sock, bytes + sentTotal, remaining, 0)
        );

#endif

#endif

        if (sent < 0)
        {
            if (socketInterrupted())
                continue;

            return false;
        }

        if (sent == 0)
            return false;

        sentTotal += static_cast<size_t>(sent);
    }

    return true;
}


inline Socket connectHostSocket(const char *host, int port)
{
    if (host == nullptr || port < 1 || port > 65535)
        return BAD_SOCKET;

    addrinfo hints;

    std::memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portText[16];

    std::snprintf(portText, sizeof(portText), "%d", port);

    addrinfo *results = nullptr;

    if (getaddrinfo(host, portText, &hints, &results) != 0)
        return BAD_SOCKET;

    Socket connected = BAD_SOCKET;

    for (addrinfo *item = results;
         item != nullptr;
         item = item->ai_next)
    {
        Socket candidate = socket(
            item->ai_family,
            item->ai_socktype,
            item->ai_protocol
        );

        if (candidate == BAD_SOCKET)
            continue;

        if (connect(
                candidate,
                item->ai_addr,
                static_cast<SocketLength>(item->ai_addrlen)
            ) == 0)
        {
            connected = candidate;
            break;
        }

        closeSocket(candidate);
    }

    freeaddrinfo(results);

    return connected;
}


