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

struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
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


// ---- small readers used by Part B (put them just above openTdbg) ----
static const int64_t TDBG_HEADER_SIZE = 4 + 4 + 4 + 8; // fields are written one by one, so no padding

static bool readExact(FILE *f, void *buf, size_t n)
{
    return n == 0 || fread(buf, 1, n, f) == n;
}
static bool readInt32(FILE *f, int32_t &v)
{
    return readExact(f, &v, sizeof(v));
}
static bool readInt64(FILE *f, int64_t &v)
{
    return readExact(f, &v, sizeof(v));
}
// A string is [int32 length][characters]. limitEnd stops a bad length from reading past the snapshots.
static bool readString(FILE *f, string &out, int64_t limitEnd)
{
    int32_t length;

    if (!readInt32(f, length) || length < 0 || length > MAX_TDBG_STRING)
        return false;

    int64_t position = fileTell64(f);

    if (position < 0 || position + length > limitEnd)
        return false;

    out.resize(static_cast<size_t>(length));

    return readExact(f, length > 0 ? &out[0] : nullptr, static_cast<size_t>(length));
}
static bool readVariable(FILE *f, Variable &v, int64_t limitEnd)
{
    return readString(f, v.name, limitEnd) && readInt32(f, v.value);
}
static bool readFrame(FILE *f, Frame &fr, int64_t limitEnd)
{
    if (!readString(f, fr.func_name, limitEnd) || !readInt32(f, fr.argc))
        return false;

    if (fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        return false;

    for (int i = 0; i < fr.argc; i++)
        if (!readVariable(f, fr.argv[i], limitEnd))
            return false;

    if (!readInt32(f, fr.returnLine) || !readInt32(f, fr.localCount))
        return false;

    if (fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        return false;

    for (int i = 0; i < fr.localCount; i++)
        if (!readVariable(f, fr.locals[i], limitEnd))
            return false;

    return true;
}


bool openTdbg(const char *path, TdbgReader &r)
{
    r.file = nullptr;
    r.index.clear();

    FILE *f = fopen(path, "rb");

    if (f == nullptr)
        return false;

    auto fail = [&]() { fclose(f); r.index.clear(); return false; };

    // file size, so we can check the layout
    if (!fileSeek64(f, 0, SEEK_END))
        return fail();

    int64_t fileSize = fileTell64(f);

    if (fileSize < TDBG_HEADER_SIZE || !fileSeek64(f, 0, SEEK_SET))
        return fail();

    // header, field by field (the struct in memory has padding, the file does not)
    TTDBHeader h = {};

    if (!readExact(f, h.magic, 4) || !readInt32(f, h.version) ||
        !readInt32(f, h.stepCount) || !readInt64(f, h.indexOffset))
        return fail();

    if (memcmp(h.magic, "TTDB", 4) != 0 || h.version != TTDB_FORMAT_VERSION || h.stepCount <= 0)
        return fail();

    // the index must be the last thing in the file and hold exactly stepCount offsets
    if (h.indexOffset < TDBG_HEADER_SIZE ||
        h.indexOffset + static_cast<int64_t>(h.stepCount) * 8 != fileSize)
        return fail();

    if (!fileSeek64(f, h.indexOffset, SEEK_SET))
        return fail();

    r.index.resize(static_cast<size_t>(h.stepCount));

    if (!readExact(f, r.index.data(), r.index.size() * sizeof(int64_t)))
        return fail();

    // offsets must grow and stay inside the snapshot area
    int64_t previous = TDBG_HEADER_SIZE - 1;

    for (int64_t offset : r.index)
    {
        if (offset <= previous || offset >= h.indexOffset)
            return fail();

        previous = offset;
    }

    r.file = f;
    r.header = h;

    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    if (r.file != nullptr)
        fclose(r.file);

    r.file = nullptr;

    vector<int64_t>().swap(r.index); // really frees the memory
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file == nullptr || step < 0 || step >= r.header.stepCount)
        return nullptr;

    // the index takes us straight to the step, nothing before it is read
    if (!fileSeek64(r.file, r.index[static_cast<size_t>(step)], SEEK_SET))
        return nullptr;

    Snapshot *s = new Snapshot;

    s->stackDepth = 0;

    int32_t depth;

    bool ok = readInt32(r.file, depth) && depth >= 0 && depth <= MAX_STACK_DEPTH;

    if (ok)
    {
        s->stackDepth = depth;

        for (int i = 0; i < depth && ok; i++)
            ok = readFrame(r.file, s->callStack[i], r.header.indexOffset);
    }

    if (!ok)
    {
        delete s;

        return nullptr;
    }

    return s; // the caller deletes it
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================


void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader = TdbgReader();
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);

    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snapshot = readSnapshotAt(s.reader, static_cast<int32_t>(s.currentStep));

    if (snapshot == nullptr)
    {
        cout << "damaged .tdbg file\n";

        return false;
    }

    printSnapshot(static_cast<int32_t>(s.currentStep) + 1, s.reader.header.stepCount, *snapshot);

    delete snapshot;

    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no session yet. Use: start <source file>\n";

        return false;
    }

    int64_t total = s.reader.header.stepCount;
    int64_t target = s.currentStep + delta;

    if (target < 0)
    {
        cout << "cannot go back that far: you are at step " << s.currentStep + 1 << " (first step is 1)\n";

        return false;
    }

    if (target >= total)
    {
        cout << "cannot go forward that far: you are at step " << s.currentStep + 1
             << " of " << total << "\n";

        return false;
    }

    int64_t previous = s.currentStep;

    s.currentStep = target;

    if (!showCurrent(s))
    {
        s.currentStep = previous; // stay where we were if the file turned out damaged

        return false;
    }

    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    string error;

    // 1-2. Download into a temporary name. If it fails, print the error and keep the old session.
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_TEMP_FILE, error))
    {
        cout << error << "\n";

        return false;
    }

    // 3. Open the new file into a NEW local reader.
    TdbgReader fresh;

    if (!openTdbg(TDBG_TEMP_FILE, fresh))
    {
        remove(TDBG_TEMP_FILE);

        cout << "server sent a damaged .tdbg file\n";

        return false;
    }

    // Give the download its permanent name. The open FILE* follows the file, not the name.
    rename(TDBG_TEMP_FILE, TDBG_FILE);

    // 4. Only now close the old reader and put the new one into the session.
    if (s.active_flag)
        closeTdbg(s.reader);

    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;

    // 5. Show step 1.
    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "\n=== Step " << step << " of " << totalSteps << " ===\n";

    if (s.stackDepth == 0)
    {
        cout << "  (call stack is empty: the program has finished)\n";

        return;
    }

    // callStack[0] is the TOP frame (the function running now), main is the last one.
    for (int i = 0; i < s.stackDepth; i++)
    {
        const Frame &f = s.callStack[i];

        cout << "  [" << i << "] " << f.func_name << (i == 0 ? "   <- running" : "") << "\n";

        for (int j = 0; j < f.argc; j++)
            cout << "        arg   " << f.argv[j].name << " = " << f.argv[j].value << "\n";

        for (int j = 0; j < f.localCount; j++)
            cout << "        local " << f.locals[j].name << " = " << f.locals[j].value << "\n";
    }
}
void printHelp()
{
    // Print the commands
    cout << "Commands:\n"
         << "  start <source file>   send the source to the server and show step 1\n"
         << "  next                  go one step forward\n"
         << "  prv                   go one step back\n"
         << "  jmp next <n>          go n steps forward\n"
         << "  jmp prv <n>           go n steps back\n"
         << "  help                  show this text\n"
         << "  quit | exit           leave\n"
         << "(a leading \"" << PROGRAM_NAME << "\" is allowed, e.g. \"" << PROGRAM_NAME << " next\")\n";
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================


// ---- helper for jmp (put it just above runCommand) ----
static bool parseJumpCount(const string &text, int32_t &out)
{
    if (text.empty() || text.size() > 10)
        return false;

    for (char c : text)
        if (c < '0' || c > '9')
            return false;

    long long value = stoll(text);

    if (value < 1 || value > MAX_JUMP)
        return false;

    out = static_cast<int32_t>(value);

    return true;
}
bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    vector<string> words;
    string word;

    for (size_t i = 0; i <= line.size(); i++)
    {
        char c = (i < line.size()) ? line[i] : ' ';

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!word.empty())
                words.push_back(word);

            word.clear();
        }
        else
        {
            word += c;
        }
    }

    if (words.empty())
        return true; // blank line: nothing to do

    // 2. If the first word is "ttdb", drop it.
    if (words[0] == PROGRAM_NAME)
    {
        words.erase(words.begin());

        if (words.empty())
        {
            printHelp();

            return true;
        }
    }

    // 3. Match the first word.
    const string &cmd = words[0];

    if (cmd == "start" && words.size() == 2)
        return cmdStart(s, words[1]);

    if (cmd == "next" && words.size() == 1)
        return cmdNext(s);

    if (cmd == "prv" && words.size() == 1)
        return cmdPrv(s);

    if (cmd == "jmp" && words.size() == 3 && (words[1] == "next" || words[1] == "prv"))
    {
        int32_t n;

        if (parseJumpCount(words[2], n))
            return words[1] == "next" ? cmdJmpNext(s, n) : cmdJmpPrv(s, n);
    }
    else if (cmd == "help" && words.size() == 1)
    {
        printHelp();

        return true;
    }
    else if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
    {
        quitRequested = true;

        return true;
    }

    // 4. Anything else, or a wrong number of words.
    cout << "unknown command\n";

    printHelp();

    return false;
}


int main(int argc, char *argv[])
{
    // ./client <lb_ip> <lb_port>   (with no arguments it uses 127.0.0.1:5000)
    if (argc != 1 && argc != 3)
    {
        cerr << "Usage: ./client <lb_ip> <lb_port>\n";

        return 1;
    }

    if (argc == 3)
    {
        LB_HOST = argv[1];

        if (!parsePort(argv[2], LB_PORT))
        {
            cerr << "client: invalid port\n";

            return 1;
        }
    }

    DebugSession session;

    initSession(session);

    cout << "Time-Travel Debugger client (load balancer " << LB_HOST << ":" << LB_PORT << ")\n";

    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested)
    {
        cout << PROMPT << flush;

        if (!getline(cin, line))
            break; // end of input

        runCommand(session, line, quitRequested); // an error never ends the loop
    }

    closeSession(session);

    return 0;
}