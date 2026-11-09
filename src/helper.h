#pragma once

// Linux only (every library the original helper.h used is kept, except the Windows-only ones).
// Things used by more than one of server.cpp, load_balancer.cpp and client.cpp live here:
// constants, the .tdbg structs, socket helpers, CRC-32 and the checked send/receive helpers.

#include <array>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <signal.h>

// ---- Constants ----
constexpr int32_t MAX_VARS_PER_FRAME = 16;
constexpr int32_t MAX_STACK_DEPTH = 64;

constexpr uint64_t MAX_SOURCE_BYTES = 15ULL * 1024ULL * 1024ULL;
constexpr size_t IO_BUFFER_SIZE = 64U * 1024U;

constexpr int32_t TTDB_FORMAT_VERSION = 1;
constexpr int32_t MAX_TDBG_STRING = 15 * 1024 * 1024;

// Timeouts in seconds.
constexpr int SOCKET_TIMEOUT_SEC = 5;    // idle time allowed on LB<->client and LB<->server uploads
constexpr int RESPONSE_TIMEOUT_SEC = 60; // idle time the LB waits for the server while it executes
constexpr int CLIENT_TIMEOUT_SEC = 40;   // client gives up if nothing arrives (longer than the LB's 30 s queue limit)

// ---- Structs used by both server and client (the .tdbg file is written and read with these) ----
struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Small helpers ----

// Strict port parser: digits only, 1..65535.
inline bool parsePort(const char *text, int &out)
{
    if (text == nullptr || *text == '\0')
        return false;

    char *end = nullptr;

    errno = 0;

    long value = std::strtol(text, &end, 10);

    if (errno != 0 || *end != '\0' || value < 1 || value > 65535)
        return false;

    out = static_cast<int>(value);

    return true;
}

// 64-bit file positions (the .tdbg and resolve.bin can be large).
inline int64_t fileTell64(FILE *file)
{
    return static_cast<int64_t>(ftello(file));
}

inline bool fileSeek64(FILE *file, int64_t offset, int origin)
{
    return fseeko(file, static_cast<off_t>(offset), origin) == 0;
}

// ---- Socket helpers ----
inline void closeSocket(int sock)
{
    if (sock >= 0)
        close(sock);
}

// "I am done sending." The other side's recv() then returns 0.
inline void shutdownSocketWrite(int sock)
{
    if (sock >= 0)
        shutdown(sock, SHUT_WR);
}

inline void shutdownSocketBoth(int sock)
{
    if (sock >= 0)
        shutdown(sock, SHUT_RDWR);
}

inline bool socketInterrupted()
{
    return errno == EINTR;
}

// Applies the same limit to recv() and send(): they fail instead of blocking forever.
inline bool setSocketTimeouts(int sock, int seconds)
{
    timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

// One recv(). Returns bytes read, 0 when the peer closed, -1 on error (timeouts included).
inline int receiveSome(int sock, void *buffer, size_t capacity)
{
    if (capacity == 0)
        return 0;

    if (capacity > (1U << 30))
        capacity = 1U << 30;

    return static_cast<int>(recv(sock, buffer, capacity, 0));
}

// send() may send fewer bytes than asked for, so keep calling it until everything is out.
// MSG_NOSIGNAL: a closed peer gives an error instead of killing the program with SIGPIPE.
inline bool sendAllBytes(int sock, const void *buffer, size_t length)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(buffer);

    size_t sentTotal = 0;

    while (sentTotal < length)
    {
        size_t remaining = length - sentTotal;

        if (remaining > (1U << 30))
            remaining = 1U << 30;

        ssize_t sent = send(sock, bytes + sentTotal, remaining, MSG_NOSIGNAL);

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

// Connects to an IPv4 address ("127.0.0.1"; "localhost" is accepted as 127.0.0.1).
// Used by the client (to the load balancer) and the load balancer (to the server).
// Returns the socket, or -1 if the connection fails.
inline int connectHostSocket(const char *host, int port)
{
    if (host == nullptr || port < 1 || port > 65535)
        return -1;

    if (std::strcmp(host, "localhost") == 0)
        host = "127.0.0.1";

    sockaddr_in address;

    std::memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
        return -1;

    // SOCK_CLOEXEC: a program started with system() must not inherit this socket.
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (sock < 0)
        return -1;

    if (connect(sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        close(sock);

        return -1;
    }

    return sock;
}

// ---- CRC-32 (the checksum) ----
class Crc32
{
    uint32_t state;

    struct Table
    {
        uint32_t values[256];

        Table()
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t value = i;

                for (int bit = 0; bit < 8; ++bit)
                    value = (value & 1U) ? (value >> 1) ^ 0xEDB88320U : (value >> 1);

                values[i] = value;
            }
        }
    };

    // Built once, thread-safe (two threads of the load balancer compute CRCs).
    static const Table &table()
    {
        static const Table instance;

        return instance;
    }

public:

    Crc32() : state(0xFFFFFFFFU)
    {
    }

    void update(const void *data, size_t length)
    {
        const unsigned char *bytes = static_cast<const unsigned char *>(data);

        const Table &lookup = table();

        for (size_t i = 0; i < length; ++i)
            state = lookup.values[(state ^ bytes[i]) & 0xFFU] ^ (state >> 8);
    }

    uint32_t value() const
    {
        return state ^ 0xFFFFFFFFU;
    }
};

inline void encodeChecksum(uint32_t checksum, unsigned char output[4])
{
    output[0] = static_cast<unsigned char>((checksum >> 24) & 0xFFU);
    output[1] = static_cast<unsigned char>((checksum >> 16) & 0xFFU);
    output[2] = static_cast<unsigned char>((checksum >> 8) & 0xFFU);
    output[3] = static_cast<unsigned char>(checksum & 0xFFU);
}

inline uint32_t decodeChecksum(const unsigned char input[4])
{
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

// The receiver cannot know where the body ends until the stream ends, so it always holds back
// the newest 4 bytes. feed() returns the bytes that are definitely body (status byte included).
// At the end, verify() compares the held-back 4 bytes with the CRC of everything before them.
class TrailerChecker
{
    unsigned char tail[4];
    size_t tailCount;
    Crc32 crc;

public:

    TrailerChecker() : tailCount(0)
    {
    }

    size_t feed(const unsigned char *input, size_t length, unsigned char *output)
    {
        size_t produced = 0;

        for (size_t i = 0; i < length; ++i)
        {
            if (tailCount < 4)
            {
                tail[tailCount++] = input[i];
            }
            else
            {
                output[produced++] = tail[0];

                crc.update(tail, 1);

                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = tail[3];
                tail[3] = input[i];
            }
        }

        return produced;
    }

    bool verify() const
    {
        return tailCount == 4 && decodeChecksum(tail) == crc.value();
    }

    bool getTrailer(unsigned char output[4]) const
    {
        if (tailCount != 4)
            return false;

        std::memcpy(output, tail, 4);

        return true;
    }
};

// Sends [status][payload][CRC-32]. The CRC covers the status byte too.
inline bool sendCheckedBytes(int sock, uint8_t status, const void *payload, size_t payloadLength)
{
    Crc32 crc;

    crc.update(&status, 1);

    if (!sendAllBytes(sock, &status, 1))
        return false;

    if (payloadLength > 0)
    {
        crc.update(payload, payloadLength);

        if (!sendAllBytes(sock, payload, payloadLength))
            return false;
    }

    unsigned char trailer[4];

    encodeChecksum(crc.value(), trailer);

    return sendAllBytes(sock, trailer, sizeof(trailer));
}

// Same, but streams a file in chunks, so the whole file is never held in memory.
inline bool sendCheckedFile(int sock, uint8_t status, FILE *file, uint64_t maxPayloadBytes = UINT64_MAX)
{
    if (file == nullptr)
        return false;

    if (!fileSeek64(file, 0, SEEK_SET))
        return false;

    Crc32 crc;

    crc.update(&status, 1);

    if (!sendAllBytes(sock, &status, 1))
        return false;

    unsigned char buffer[IO_BUFFER_SIZE];

    uint64_t total = 0;

    while (true)
    {
        size_t count = std::fread(buffer, 1, sizeof(buffer), file);

        if (count > 0)
        {
            total += count;

            if (total > maxPayloadBytes)
                return false;

            crc.update(buffer, count);

            if (!sendAllBytes(sock, buffer, count))
                return false;
        }

        if (count < sizeof(buffer))
        {
            if (std::ferror(file))
                return false;

            if (std::feof(file))
                break;
        }
    }

    unsigned char trailer[4];

    encodeChecksum(crc.value(), trailer);

    return sendAllBytes(sock, trailer, sizeof(trailer));
}