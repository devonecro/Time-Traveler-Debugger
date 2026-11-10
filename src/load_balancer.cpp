// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================
//
// Many clients connect at the same time. Each sends one source string. They wait in a queue
// (first come, first served). For each client, in order: make sure the server is running
// (start it with system() if not), pass the string to the server, pass the reply back.
// It does not read or understand the messages. It only moves bytes.
//
// Two threads:
//   acceptor thread (acceptLoop)     -> takes new clients and puts them in the queue.
//   main thread (dispatcherLoop)     -> serves ONE client completely, then the next.
//
// The server is started with system("./server 5001 &"). The "&" is required.

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
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds)
const int CLIENT_QUEUE_TIMEOUT_SEC = 30;   // a client that waited longer is closed without a reply
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: hand-written linked list (no std::queue) ----
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
    int sock;           // the connected socket
    string address;     // the client's IP as text
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1; // set to 0 by SIGINT / SIGTERM; the loops run while (flag)

void handler(int sg)
{
    (void)sg;
    flag = 0;
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

bool forwardCheckedStream(int fromSock,int toSock,string &errorText)
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
// Reads and throws away whatever the client is still sending, so closing the socket afterwards
// does not reset the connection and wipe out the reply we just sent. Bounded in bytes and time.
void drainClientUpload(int clientSock)
{
    unsigned char buffer[IO_BUFFER_SIZE];

    uint64_t drained = 0;

    time_t started = time(nullptr);

    while (drained <= MAX_SOURCE_BYTES + 16 &&
           difftime(time(nullptr), started) < 30)
    {
        int received = receiveSome(
            clientSock,
            buffer,
            sizeof(buffer)
        );

        if (received <= 0)
            return;

        drained += static_cast<uint64_t>(received);
    }
}

// ---- Basic socket helpers ----
// Reply format: [status: 1 byte][payload][checksum: 4 bytes]. Error: status 1 + message text.
bool sendError(int sock, const string &message)
{
    bool sent = sendCheckedBytes(sock, 1, message.data(), message.size());

    shutdownSocketWrite(sock);

    return sent;
}
int createListenSocket(int port)
{
    // SOCK_CLOEXEC: the server we start with system() must not inherit this socket.
    int listenSock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (listenSock < 0)
    {
        perror("load_balancer: socket failed");

        return -1;
    }

    int reuse = 1;

    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address = {};

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port));

    if (
        bind(listenSock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listenSock, LISTEN_BACKLOG) != 0
    )
    {
        perror("load_balancer: bind/listen failed");

        close(listenSock);

        return -1;
    }

    return listenSock;
}
int connectToServer()
{
    int serverSock = connectHostSocket(serverHost.c_str(), serverPort);

    if (serverSock >= 0)
        setSocketTimeouts(serverSock, RESPONSE_TIMEOUT_SEC); // the server may need time to execute

    return serverSock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer()
{
    if (!backendIsLocal())
        return false;

    string command = "./server " + to_string(serverPort) + " &";

    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    int serverSock = connectToServer();

    if (serverSock >= 0)
        return serverSock;

    if (!startServer())
    {
        cerr << "load_balancer: could not launch local backend process\n";

        return -1;
    }

    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS; ++attempt)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);

        serverSock = connectToServer();

        if (serverSock >= 0)
            return serverSock;
    }

    return -1;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

void relayOneClient(const ClientInfo &client)
{
    int serverSock =
        connectToServerWithWakeup();

    if (serverSock == -1)
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

// Runs on its own detached thread so the acceptor never waits on a slow client.
// This thread is the one place that closes a refused client's socket.
void rejectClient(int clientSock)
{
    sendError(clientSock, "server busy, try again later");

    drainClientUpload(clientSock);

    closeSocket(clientSock);
}

void acceptLoop(int listenSock)
{
    while (flag)
    {
        sockaddr_in clientAddress = {};

        socklen_t addressLength = sizeof(clientAddress);

        int clientSock = accept4(
            listenSock,
            reinterpret_cast<sockaddr *>(&clientAddress),
            &addressLength,
            SOCK_CLOEXEC
        );

        if (clientSock < 0)
        {
            if (flag)
            {
                perror("load_balancer: accept failed");

                usleep(100000);
            }

            continue;
        }

        setSocketTimeouts(clientSock, SOCKET_TIMEOUT_SEC);

        char addressText[INET_ADDRSTRLEN] = "unknown";

        inet_ntop(AF_INET, &clientAddress.sin_addr, addressText, sizeof(addressText));

        ClientInfo info;

        info.sock = clientSock;
        info.address = addressText;
        info.port = ntohs(clientAddress.sin_port);
        info.arrivedAt = time(nullptr);

        bool acceptedIntoQueue = false;

        {
            lock_guard<mutex> lock(queueMutex);

            if (pendingClients.size() < MAX_PENDING_CLIENTS)
            {
                pendingClients.enqueue(info);

                acceptedIntoQueue = true;
            }
        }

        if (!acceptedIntoQueue)
            thread(rejectClient, clientSock).detach();
    }
}
void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo client;

        bool haveClient = false;

        {
            lock_guard<mutex> lock(queueMutex);

            haveClient = pendingClients.dequeue(client);
        }

        if (!haveClient)
        {
            usleep(100000);

            continue;
        }

        if (difftime(time(nullptr), client.arrivedAt) > CLIENT_QUEUE_TIMEOUT_SEC)
        {
            // Waited too long: close without a reply.
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

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4)
    {
        cerr << "Usage: ./load_balancer <server_ip> <server_port> [lb_port]\n";

        return 1;
    }

    serverHost = argv[1];

    if (!parsePort(argv[2], serverPort) || (argc == 4 && !parsePort(argv[3], lbPort)))
    {
        cerr << "load_balancer: invalid port\n";

        return 1;
    }

    signal(SIGINT, handler);  // Ctrl+C clears flag so the loops can stop and clean up
    signal(SIGTERM, handler);

    int listenSock = createListenSocket(lbPort);

    if (listenSock < 0)
        return 1;

    cout << "load balancer listening on port " << lbPort
         << ", backend " << serverHost << ":" << serverPort << "\n";

    thread acceptThread(acceptLoop, listenSock);

    dispatcherLoop();

    flag = 0;

    shutdownSocketBoth(listenSock); // wakes the acceptor out of accept()

    acceptThread.join();

    closeSocket(listenSock);

    closeQueuedClients();

    return 0;
}