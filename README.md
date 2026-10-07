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


