// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <csignal>

#include "helper.h"

using namespace std;


const int LISTEN_BACKLOG = 16;
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;
const int CLIENT_QUEUE_TIMEOUT_SEC = 30;
const int SOCKET_TIMEOUT_SEC = 30;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
// Hand-written linked-list queue.

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;

        Node(const T &value): data(value), next(nullptr)
        {
        }
    };

    Node *frontNode;
    Node *rearNode;

    int itemCount;

public:

    Queue()
        : frontNode(nullptr),
          rearNode(nullptr),
          itemCount(0)
    {
    }

    ~Queue()
    {
        while (frontNode != nullptr)
        {
            Node *oldFront = frontNode;

            frontNode = frontNode->next;

            delete oldFront;
        }

        rearNode = nullptr;
        itemCount = 0;
    }

    Queue(const Queue &) = delete;

    Queue &operator=(const Queue &) = delete;

    void enqueue(const T &value)
    {
        Node *node = new Node(value);

        if (rearNode == nullptr)
        {
            frontNode = rearNode = node;
        }
        else
        {
            rearNode->next = node;

            rearNode = node;
        }

        ++itemCount;
    }

    bool dequeue(T &value)
    {
        if (frontNode == nullptr)
            return false;

        Node *oldFront = frontNode;

        value = oldFront->data;

        frontNode = frontNode->next;

        if (frontNode == nullptr)
            rearNode = nullptr;

        delete oldFront;

        --itemCount;

        return true;
    }

    bool isEmpty() const
    {
        return itemCount == 0;
    }

    int size() const
    {
        return itemCount;
    }
};

struct ClientInfo
{
    Socket sock;

    string address;

    int port;

    time_t arrivedAt;
};


Queue<ClientInfo> pendingClients;

mutex queueMutex;

atomic<bool> keepRunning(true);

volatile sig_atomic_t stopRequested = 0;


void loadBalancerSignalHandler(int)
{
    stopRequested = 1;
}
bool shouldKeepRunning()
{
    return keepRunning.load() && !stopRequested;
}
bool shouldKeepRunning()
{
    return keepRunning.load() && !stopRequested;
}
bool backendIsLocal()
{
    if (
        serverHost == "localhost" ||
        serverHost == "::1"
    )
        return true;

    return
        serverHost.size() >= 4 &&
        serverHost.compare(0, 4, "127.") == 0;
}
// Forwards a checked stream.
//
// The final four bytes are withheld until the CRC passes.
// If verification fails, the checksum trailer is NOT forwarded.

bool forwardCheckedStream(Socket fromSock,Socket toSock,string &errorText)
{
    TrailerChecker checker;

    unsigned char input[IO_BUFFER_SIZE];

    unsigned char readyBytes[IO_BUFFER_SIZE];

    bool payloadSeen = false;

    while (true)
    {
        int received = receiveSome(
            fromSock,
            input,
            sizeof(input)
        );

        if (received == 0)
            break;

        if (received < 0)
        {
            if (socketInterrupted())
                continue;

            errorText = "stream receive failed";

            return false;
        }

        size_t ready = checker.feed(
            input,
            static_cast<size_t>(received),
            readyBytes
        );

        if (ready > 0)
        {
            payloadSeen = true;

            if (
                !sendAllBytes(
                    toSock,
                    readyBytes,
                    ready
                )
            )
            {
                errorText = "stream forwarding failed";

                return false;
            }
        }
    }

    if (!payloadSeen || !checker.verify())
    {
        errorText = "stream checksum mismatch";

        return false;
    }

    unsigned char trailer[4];

    if (
        !checker.getTrailer(trailer) ||

        !sendAllBytes(
            toSock,
            trailer,
            sizeof(trailer)
        )
    )
    {
        errorText = "could not forward checksum trailer";

        return false;
    }

    shutdownSocketWrite(toSock);

    return true;
}
void drainClientUpload(Socket clientSock)
{
    unsigned char buffer[IO_BUFFER_SIZE];

    while (true)
    {
        int received = receiveSome(
            clientSock,
            buffer,
            sizeof(buffer)
        );

        if (received == 0)
            return;

        if (received < 0)
        {
            if (socketInterrupted())
                continue;

            return;
        }
    }
}

// ---- Basic socket helpers ----
bool sendAll(Socket sock, const char *buffer, size_t length)
{
    size_t sentTotal = 0;

    while (sentTotal < length)
    {
        size_t remaining = length - sentTotal;

        int chunk = static_cast<int>(
            std::min(remaining, static_cast<size_t>(INT_MAX))
        );

#ifdef _WIN32

        int sent = send(
            sock,
            buffer + sentTotal,
            chunk,
            0
        );

        if (sent == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEINTR)
                continue;

            return false;
        }

#else

        ssize_t sent = send(
            sock,
            buffer + sentTotal,
            chunk,
            MSG_NOSIGNAL
        );

        if (sent < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }
#endif

        if (sent == 0)
        {
            return false;
        }

        sentTotal += sent;
    }

    return true;
}


bool sendError(Socket clientSock, const string &message)
{
    bool sent = sendCheckedBytes(
        clientSock,
        1,
        message.data(),
        message.size()
    );

    shutdownSocketWrite(clientSock);

    return sent;
}
Socket createListenSocket(int port)
{
    Socket listenSock = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (listenSock == BAD_SOCKET)
    {
        cerr << "load_balancer: socket failed: "
             << socketErrorText() << "\n";

        return BAD_SOCKET;
    }

    int reuse = 1;

#ifdef _WIN32

    setsockopt(
        listenSock,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char *>(&reuse),
        sizeof(reuse)
    );

#else

    setsockopt(
        listenSock,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

#endif

    sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;

    address.sin_addr.s_addr = htonl(INADDR_ANY);

    address.sin_port =
        htons(static_cast<unsigned short>(port));

    if (
        bind(
            listenSock,
            reinterpret_cast<sockaddr *>(&address),
            sizeof(address)
        ) != 0 ||

        listen(listenSock, LISTEN_BACKLOG) != 0
    )
    {
        cerr << "load_balancer: bind/listen failed: "
             << socketErrorText() << "\n";

        closeSocket(listenSock);

        return BAD_SOCKET;
    }

    return listenSock;
}
Socket connectToServer()
{
    Socket serverSock = connectHostSocket(
        serverHost.c_str(),
        serverPort
    );

    if (serverSock != BAD_SOCKET)
    {
        setSocketTimeouts(
            serverSock,
            SOCKET_TIMEOUT_SEC
        );
    }

    return serverSock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer()
{
    if (!backendIsLocal())
        return false;

    string command;

#ifdef _WIN32

    command =
        "start \"\" /B server.exe " +
        to_string(serverPort);

#else

    command =
        "./server " +
        to_string(serverPort) +
        " &";

#endif

    int result = system(command.c_str());

    return result != -1;
}
Socket connectToServerWithWakeup()
{
    Socket serverSock = connectToServer();

    if (serverSock != BAD_SOCKET)
        return serverSock;

    if (backendIsLocal() && !startServer())
    {
        cerr << "load_balancer: "
             << "could not launch local backend process\n";
    }

    for (
        int attempt = 0;
        attempt < STARTUP_MAX_ATTEMPTS;
        ++attempt
    )
    {
        this_thread::sleep_for(
            chrono::milliseconds(
                STARTUP_POLL_INTERVAL_MS
            )
        );

        serverSock = connectToServer();

        if (serverSock != BAD_SOCKET)
            return serverSock;
    }

    return BAD_SOCKET;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

void relayOneClient(const ClientInfo &client)
{
    Socket serverSock =
        connectToServerWithWakeup();

    if (serverSock == BAD_SOCKET)
    {
        sendError(
            client.sock,
            "backend server failed to start"
        );

        drainClientUpload(client.sock);

        closeSocket(client.sock);

        return;
    }

    setSocketTimeouts(
        client.sock,
        SOCKET_TIMEOUT_SEC
    );

    string errorText;

    if (
        !forwardCheckedStream(
            client.sock,
            serverSock,
            errorText
        )
    )
    {
        shutdownSocketBoth(serverSock);

        closeSocket(serverSock);

        const string clientError =
            (errorText == "stream checksum mismatch")
            ? "upload checksum mismatch"
            : "upload forwarding failed";

        sendError(
            client.sock,
            clientError
        );

        closeSocket(client.sock);

        return;
    }

    // Backend response travels back to the client.

    if (
        !forwardCheckedStream(
            serverSock,
            client.sock,
            errorText
        )
    )
    {
        cerr << "load_balancer: backend response "
             << "was incomplete or corrupted: "
             << errorText << "\n";
    }

    closeSocket(serverSock);

    closeSocket(client.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(Socket listenSock)
{
    while (shouldKeepRunning())
    {
        fd_set readSet;

        FD_ZERO(&readSet);

        FD_SET(listenSock, &readSet);

        timeval waitTime;

        waitTime.tv_sec = 0;
        waitTime.tv_usec = 500000;

#ifdef _WIN32

        int ready = select(
            0,
            &readSet,
            nullptr,
            nullptr,
            &waitTime
        );

#else

        int ready = select(
            listenSock + 1,
            &readSet,
            nullptr,
            nullptr,
            &waitTime
        );

#endif

        if (ready == 0)
            continue;

        if (ready < 0)
        {
            if (socketInterrupted())
                continue;

            if (shouldKeepRunning())
            {
                cerr << "load_balancer: select failed: "
                     << socketErrorText() << "\n";
            }

            continue;
        }

        sockaddr_in clientAddress;

        SocketLength addressLength =
            sizeof(clientAddress);

        Socket clientSock = accept(
            listenSock,
            reinterpret_cast<sockaddr *>(&clientAddress),
            &addressLength
        );

        if (clientSock == BAD_SOCKET)
        {
            if (socketInterrupted())
                continue;

            if (shouldKeepRunning())
            {
                cerr << "load_balancer: accept failed: "
                     << socketErrorText() << "\n";
            }

            continue;
        }

        setSocketTimeouts(
            clientSock,
            SOCKET_TIMEOUT_SEC
        );

        char hostText[NI_MAXHOST] = {0};

        char serviceText[NI_MAXSERV] = {0};

        string addressText = "unknown";

        int clientPort = 0;

        if (
            getnameinfo(
                reinterpret_cast<sockaddr *>(&clientAddress),
                addressLength,
                hostText,
                sizeof(hostText),
                serviceText,
                sizeof(serviceText),
                NI_NUMERICHOST | NI_NUMERICSERV
            ) == 0
        )
        {
            addressText = hostText;

            clientPort = atoi(serviceText);
        }

        ClientInfo info;

        info.sock = clientSock;

        info.address = addressText;

        info.port = clientPort;

        info.arrivedAt = time(nullptr);

        bool acceptedIntoQueue = false;

        {
            lock_guard<mutex> lock(queueMutex);

            if (
                pendingClients.size() <
                MAX_PENDING_CLIENTS
            )
            {
                pendingClients.enqueue(info);

                acceptedIntoQueue = true;
            }
        }

        if (!acceptedIntoQueue)
        {
            sendError(
                clientSock,
                "server busy, try again later"
            );

            drainClientUpload(clientSock);

            closeSocket(clientSock);
        }
    }
}
void dispatcherLoop()
{
    while (shouldKeepRunning())
    {
        ClientInfo client;

        bool haveClient = false;

        {
            lock_guard<mutex> lock(queueMutex);

            haveClient =
                pendingClients.dequeue(client);
        }

        if (!haveClient)
        {
            this_thread::sleep_for(
                chrono::milliseconds(100)
            );

            continue;
        }

        if (
            difftime(
                time(nullptr),
                client.arrivedAt
            ) > CLIENT_QUEUE_TIMEOUT_SEC
        )
        {
            // The specification requires silent closure
            // for clients waiting longer than 30 seconds.

            closeSocket(client.sock);

            continue;
        }

        relayOneClient(client);
    }
}
void closeQueuedClients()
{
    ClientInfo client;

    while (true)
    {
        bool found = false;

        {
            lock_guard<mutex> lock(queueMutex);

            found = pendingClients.dequeue(client);
        }

        if (!found)
            break;

        closeSocket(client.sock);
    }
}
bool parsePort(const char *text, int &port)
{
    char *end = nullptr;

    long value = strtol(text, &end, 10);

    if (
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535
    )
        return false;

    port = static_cast<int>(value);

    return true;
}
void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4)
    {
        cerr <<
            "Usage: ./load_balancer "
            "<server_ip> <server_port> [lb_port]\n";

        return 1;
    }

    serverHost = argv[1];

    if (
        !parsePort(argv[2], serverPort) ||

        (
            argc == 4 &&
            !parsePort(argv[3], lbPort)
        )
    )
    {
        cerr << "load_balancer: invalid port\n";

        return 1;
    }

    if (!networkInitialize())
    {
        cerr <<
            "load_balancer: network initialization failed\n";

        return 1;
    }

    signal(SIGINT, loadBalancerSignalHandler);

    signal(SIGTERM, loadBalancerSignalHandler);

    Socket listenSock =
        createListenSocket(lbPort);

    if (listenSock == BAD_SOCKET)
    {
        networkCleanup();

        return 1;
    }

    cout << "load balancer listening on port "
         << lbPort
         << ", backend "
         << serverHost
         << ":"
         << serverPort
         << "\n";

    thread acceptThread(
        acceptLoop,
        listenSock
    );

    dispatcherLoop();

    keepRunning.store(false);

    shutdownSocketBoth(listenSock);

    acceptThread.join();

    closeSocket(listenSock);

    closeQueuedClients();

    networkCleanup();

    return 0;
}