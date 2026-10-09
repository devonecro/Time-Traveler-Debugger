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

Not Yet



