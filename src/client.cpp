// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
// (IO_BUFFER_SIZE, MAX_VARS_PER_FRAME, MAX_STACK_DEPTH and the structs Variable, Frame, Snapshot,
//  TTDBHeader now come from helper.h, so they are NOT declared here any more.)
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg";     // where the downloaded .tdbg is saved
const char *TDBG_TEMP_FILE = "ttdb_new.tdbg"; // a download lands here first, so a failed start cannot hurt the old session
const int MAX_JUMP = 1000000000;              // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE *file = nullptr;     // the .tdbg file stays open for the whole session
    TTDBHeader header = {};
    vector<int64_t> index;    // index[i] = byte offset of snapshot i
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t got = 0;

    while (got < n)
    {
        int r = receiveSome(sock, buf + got, n - got);

        if (r < 0)
        {
            if (socketInterrupted())
                continue;

            return false;
        }

        if (r == 0)
            return false; // closed before n bytes arrived

        got += static_cast<size_t>(r);
    }

    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    return sendAllBytes(sock, buf, n); // helper.h: loops until done and uses MSG_NOSIGNAL
}
int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int sock = connectHostSocket(host, port);

    if (sock < 0)
        return -1;

    setSocketTimeouts(sock, CLIENT_TIMEOUT_SEC); // never wait forever for the load balancer

    return sock;
}
bool uploadSource(int sock, const char *sourcePath)
{
    FILE *source = fopen(sourcePath, "rb");

    if (source == nullptr)
    {
        cerr << "client: could not open source file: "
             << sourcePath << "\n";

        return false;
    }

    if (!fileSeek64(source, 0, SEEK_END))
    {
        fclose(source);

        return false;
    }

    int64_t sourceSize = fileTell64(source);

    if (
        sourceSize < 0 ||
        static_cast<uint64_t>(sourceSize) >
        MAX_SOURCE_BYTES
    )
    {
        cerr <<
            "client: source file must be no larger than 15 MiB\n";

        fclose(source);

        return false;
    }

    bool sent = sendCheckedFile(
        sock,
        0,
        source,
        MAX_SOURCE_BYTES
    );

    fclose(source);

    if (!sent)
        return false;

    shutdownSocketWrite(sock);

    return true;
}
bool receiveResponse(
    int sock,
    const char *outTdbgPath,
    string &outError
)
{
    outError.clear();

    TrailerChecker checker;

    unsigned char input[IO_BUFFER_SIZE];

    unsigned char readyBytes[IO_BUFFER_SIZE];

    bool statusSeen = false;

    bool receivedAnyBytes = false;

    unsigned char status = 255;

    uint64_t bodyBytes = 0;

    bool invalidStatus = false;

    bool writeFailed = false;

    FILE *outputFile = nullptr;

    const size_t MAX_ERROR_TEXT = 1024 * 1024;

    while (true)
    {
        int received = receiveSome(
            sock,
            input,
            sizeof(input)
        );

        if (received == 0)
            break;

        if (received < 0)
        {
            if (socketInterrupted())
                continue;

            writeFailed = true;

            break;
        }

        receivedAnyBytes = true;

        size_t ready = checker.feed(
            input,
            static_cast<size_t>(received),
            readyBytes
        );

        size_t offset = 0;

        if (!statusSeen && ready > 0)
        {
            status = readyBytes[0];

            statusSeen = true;

            offset = 1;

            if (status == 0)
            {
                outputFile = fopen(
                    outTdbgPath,
                    "wb"
                );

                if (outputFile == nullptr)
                    writeFailed = true;
            }
            else if (status != 1)
            {
                invalidStatus = true;
            }
        }

        if (ready > offset && statusSeen)
        {
            size_t count = ready - offset;

            bodyBytes += count;

            if (status == 0)
            {
                if (
                    outputFile != nullptr &&
                    !writeFailed &&
                    fwrite(
                        readyBytes + offset,
                        1,
                        count,
                        outputFile
                    ) != count
                )
                {
                    writeFailed = true;
                }
            }
            else if (status == 1)
            {
                if (
                    outError.size() + count <=
                    MAX_ERROR_TEXT
                )
                {
                    outError.append(
                        reinterpret_cast<const char *>(
                            readyBytes + offset
                        ),
                        count
                    );
                }
                else
                {
                    invalidStatus = true;
                }
            }
        }
    }

    bool checksumOK = checker.verify();

    if (outputFile != nullptr)
    {
        if (fflush(outputFile) != 0)
            writeFailed = true;

        if (fclose(outputFile) != 0)
            writeFailed = true;
    }

    if (!receivedAnyBytes)
    {
        remove(outTdbgPath);

        outError =
            "load balancer closed the connection "
            "without a response (queue timeout is possible)";

        return false;
    }

    if (
        !checksumOK ||
        !statusSeen ||
        invalidStatus ||
        writeFailed
    )
    {
        remove(outTdbgPath);

        outError = "response corrupted";

        return false;
    }

    if (status == 1)
    {
        remove(outTdbgPath);

        if (outError.empty())
            outError = "server returned an error";

        return false;
    }

    if (status != 0 || bodyBytes == 0)
    {
        remove(outTdbgPath);

        outError = "response corrupted";

        return false;
    }

    return true;
}

// NEW function: one whole request to the load balancer.
bool requestDebugFile(const char *host, int port, const char *sourcePath,
                      const char *outTdbgPath, string &outError)
{
    outError.clear();

    int sock = connectToHost(host, port);

    if (sock < 0)
    {
        outError = string("could not connect to the load balancer at ") + host + ":" + to_string(port);

        return false;
    }

    if (!uploadSource(sock, sourcePath))
    {
        closeSocket(sock);

        outError = "could not send the source file (check the path, the 15 MiB limit and the connection)";

        return false;
    }

    bool ok = receiveResponse(sock, outTdbgPath, outError);

    closeSocket(sock);

    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession &s, int32_t delta)
{
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
}