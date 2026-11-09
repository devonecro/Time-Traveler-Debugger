# Time-Traveler-Debugger
A small time traveller debugger in c++. Featuring c-- a small language with minimal functionality.

## Monday 5 October 1:30 AM
Completed implementation of Stack according to the constraints.

## Wednesday 7 October 11:00 AM
Implemented doubly linked list for timeline class.


## Wednesday 7 October 11:59 AM

Implemented Line read, First Word extraction & second word extraction for the source.bin file.

## Wednesday 7 October 12:44 PM
Added validation

## Wednesday 7 October 3:05 PM
Implemented Reading from resolve file, Writing to Resolve file & resolving file.
In Resolve Function we repeatedly called writing to resolve file and then we implemented added calls to Patch struct array
Later, we nested loops to fix the patches ( if they exist , file is not broken in case of broken it returns -1 as offset)
Patches were replaced with the actual byteoffset location of the functions that were called (This was checked from the function array and the struct it held comparing it's byteoffset and funcname values for validation)

## Wed, 7 October 3:53 PM
Implemented Tokenization where we tokeneized a line into three parts

| Keyword | Identifier | Param

## Wed, 7 October 3:59 PM
Added fixes to snapshot into, fixed temp = temp->next from temp-next and added check for index <= MaxLen


## Friday, 8 October 2:41 AM

Added some fixes from previous classes, fixed peek of Stack.
Implemented Execute function that deals with the following:

### Execute() Implementation
Implemented the executeProgram() function to run instructions from resolve.bin, starting from main.
Added support for set, add, sub, mul, and div.
Used a stack to manage function calls and returns.
Added argument passing and write-back so changes inside a function are reflected in the caller.
Used separate arrays to track arguments without changing the existing structs.
Added snapshots after each executed instruction to keep track of program state.
The execution logic is implemented, but testing and error handling is still pending.

## Friday, 8 October 3:50 PM

Implemented Pass 0x3 Serialization

Here, we take all the snapshots in our timeline and write them into a binary file called seeesion.tbdg

Following helpers were implemented:

WriteHeader() writes magic byes, version, total steps, and index position of file

WriteString() writes the length of string first followed by it's actual characters

WriteVariables() writes variable names and it's values

WriteFrame() writes a function name, arguments, return position and local variables

WriteSnapshots() write the stackdepth and all the frames stores in the snapshot

## Main Function
Write TDBG()


We open the file and write temporary header with indexofffset = 0 because we dunno the index offset yet

then we move in timeline write each snapshot and store it's starting byte position using ftell()

After all snapshots are written we add the index at the end of the file. We use fseek() to go back and update the header with actual index position.

This helps us access every snapshot directly without reading all the previous ones.


### Testing Phase

#### Linux Testing

Completed linux testing 5:15 PM Friday

#### Windows Testing 

Windows Testing Completed through vsstudio using VISUAL STUDIO 2022 compiler.
Replaced every and with && and every or with ||. In token added keyword enum.


## Phase 2

## Monday, 9 November 9:00 PM

Added shared structs, data structures and libraries in the helper.h file

## Monday, 9 November 9:26 PM

Added functions for socket timeout setters for both windows and linux. Socket error text if an error happened, shutting down both the sockets, shutdownSocketWRite() to tell the other side we are done sending the data.
and closing the socket when we are done with the client and server connection.


# Monday , 9 November 9:50 PM

Add SendAllBytes().
In TCP normal send doesn't gurantee everything will be sent so we implemented sendallbytes() this functions sends all the by keeping track of what has already been sent and keep sending until the buffer is empty of errror is occured.

It uses receiveSome() to check whatever data is currently available to read.

connectHostSocket()

The client and loadbalancer both need to establish TCP connection.

This functions is used by both client and loadBalancer. It resolves the host and port and creates a socket and attempts to connect to the resulting socket.

It the attempt fails it closes the socket before trying another address.

Client uses it to connect to loadBalancer and loadBalancer uses it to conenct to the backend server.


# Monday , 9 November 10: 00PM

## CRC-32

We need a method to check if the data was corrupted while travelling. We use CRC-32 as the checksum method. It produces a 32-bit checksum from teh bytes being transmitted.
The implementation updates the checksum incrementally as data is processed. So we don't have to load the whole source file or debugging file into memory.

The checksum covers the payload message. The sender appends the resulting four-byte checksum to the end of the message.


# Monday, 9 November 10:10 PM

## Trailer Checker

We don't know in TCP where does the paylaod end and where dose the checksum begin.

To handle this we used TrailerChecker()

The checker continously processes incoming data bt always keeps the newest four bytes seperate. These four bytes may be part of the payload being commuted, so they can't be treated as the checksum immediately.

When additional bytes arrive, the older bytes can safely be processed as payload. Once the sender closes it's writing side and the receiver reached the end of the stream, the final four bytes are treated as checksum. 

The receiver then compares the last four byes with  the CRC-32 from the status bytes and payload. 

This helps us to commute large files in smaller chunks while not storing the whole file in  memory.

# Monday, 9 November 10:18 PM

Shifted, removed already existing libraries and helper from the server.cpp and included helper.h to cater for those libraries and structures.

Addes stack data structure's destructor. Added destructor for Timeline aswell.


# Monday, 9 November 11:15 PM

## Creating Server Listening Socket


The server receives data from the load balancer so we added server listening socket function that creates a socket that is used to commute with the load balancer.

We used TCP socket for the server.

The listening socket is configured to be reused whereever possible, binds it to the requested port and starts listening to the incoming connections.
When a connection arrives, the server accepts it and obtains a seperate socket for communicating with the incoming particular load-balancer connection.

The listening socket reamins available for the future connection, while the accepted socket is used to receive teh current request and return its response

## Receiving the uploaded File

After the load balancer connects to the server, the server must receive the source file.

Instead of reading the entire uploaded file into one large memory buffer, the server repeatedly received fixed-size chunks and writes that chunks to the source.bin of the server.

This keeps memory usage limited even when the file is large.

The receiveing process also checks the amojnt of data being sent against configured source file limit.

If the connection closes unexpectedly or the file cannot be written correctly, the request is treated as failure.



# Monday, 11:37 PM 

## Connecting Phase 2 ; Socket Programming with Phase 1 Pipeline

Main is changed such that it receives the source.bin file and then it uses the existing phase 1 pipeline  as:

1. ValidateProgram()
2. resolveProgram()
3. executeProgram()
4. writeTbdg()

## Sending .tbdg

Once the execution is completed, the server must send the generated file back to the load balancer.

We added a response-sending process that streams the file in chunks. This avoids loading a potentially large .tdbg file entirely into memory.

The response begins with status byte. A successful response uses status 0, followed by the .tdbg payload and its four-byte CRC-32 trailer.

If processing fails, the server sends status 1 followed by an error message and it's checksum.

We also added error handling in this phase for edge cases like such as open socket, invalid instruction.




















