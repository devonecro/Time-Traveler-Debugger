// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include "helper.h"

using namespace std;

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack():top(nullptr),count(0)
    {}
    ~Stack()
    {
        while (top != nullptr)
        {
            Node *oldTop = top;

            top = top->next;

            delete oldTop;
        }
    }
    Stack(const Stack &) = delete;

    Stack &operator=(const Stack &) = delete;

    void push(const T &val)
    {
        if(count >= MAX_STACK_DEPTH)
        {
            throw std::overflow_error("Stack size exceeded!");
        }
        Node * newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if(isEmpty())
        {
            throw std::underflow_error("Stack is empty!");
        }

        Node * temp = top;
        T value = temp->data;

        top = top->next;
        delete temp;
        count--;

        return value; // returns the top's value
    }
    T &peek()
    {
        if(!(isEmpty()))
        {
            return top->data;
        }
        throw std::underflow_error("Tried to peek an empty Stack!");
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        if(count == 0)
        {
            return true;
        }
        return false;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node * temp = top;
        int32_t index = 0;
        while(temp!=nullptr && index < maxLen)
        {
            out[index] = temp->data;

            index++; // counts of data
            temp = temp->next;
        }
        return index;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    ~Timeline()
    {
        TimelineNode *current = head;

        while (current != nullptr)
        {
            TimelineNode *next = current->next;

            delete current->data;
            delete current;

            current = next;
        }
    }

    Timeline(const Timeline &) = delete;

    Timeline &operator=(const Timeline &) = delete;
    void record(Snapshot *s)
    {
        TimelineNode * newNode = new TimelineNode;
        newNode->data = s;
        newNode->next = nullptr;

        if(head == nullptr)
        {
            newNode->prev = nullptr;
            head = tail = newNode;
        }
        else{
            newNode->prev = tail;
            tail->next = newNode;
            tail = newNode;
        }

        stepCount++;
        // add record in the timeline
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// // Core structs
// struct Variable
// {
//     string name;
//     int32_t value;
// };
// struct Frame
// {
//     string func_name;
//     int32_t argc;
//     Variable argv[MAX_VARS_PER_FRAME];
//     int32_t returnLine;
//     Variable locals[MAX_VARS_PER_FRAME];
//     int32_t localCount;
// };
// struct Snapshot
// {
//     Frame callStack[MAX_STACK_DEPTH];
//     int32_t stackDepth;
// };
// struct TTDBHeader
// {
//     char magic[4]; // "TTDB"
//     int32_t version;
//     int32_t stepCount;
//     int64_t indexOffset;
// };
bool writeHeader(FILE *f, const TTDBHeader &h)
{
    return
        std::fwrite(h.magic, 1, 4, f) == 4 &&

        std::fwrite(
            &h.version,
            sizeof(int32_t),
            1,
            f
        ) == 1 &&

        std::fwrite(
            &h.stepCount,
            sizeof(int32_t),
            1,
            f
        ) == 1 &&

        std::fwrite(
            &h.indexOffset,
            sizeof(int64_t),
            1,
            f
        ) == 1;
}

// resolve.bin - bookkeeping
// struct FuncEntry
// {
//     string funcName;
//     int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
// };
// struct PendingPatch
// {
//     int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back && overwrite
//     string targetFuncName;
// };

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while(getline(in,out))
    {
        bool blank = true;

        for(char c : out)
        {
            if(c != ' ' && c != '\t' && c!= '\r')
            {
                blank = false;
                break; // skips blank lines && moves forward
            }
        }
        if(!blank)
        {
            return true;
        }
    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string &line)
{
    string word_  = "";
    int i = 0;

    while(i < line.length() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }

    while(i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] !='\r')
    {
        word_ += line[i];
        i++;
    }

    return word_;
    // returns first word from the input string
}
string secondWord(const string &line)
{
    string word_  = "";
    int i = 0;

    while(i < line.length() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }

    while(i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] !='\r')
    {
        i++; // skipping the first word
    }

    while(i < line.length() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++; // skipping spaces before second word
    }

    while(i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] !='\r')
    {
        word_ += line[i];
        i++; // skipping the first word
    }
    return word_;

    // returns the second word
}
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath,ios::binary);

    if(!in.is_open())
    {
        return false;
    }
    string line;
    bool isinsideFunction = false;

    while(readSourceLine(in,line))
    {
        string keyword = firstWord(line);

        if(keyword == "func")
        {
            if(isinsideFunction)
            {
                return false;
            }
            isinsideFunction = true;
        }
        else if(keyword == "func_end")
        {
            if(!isinsideFunction)
            {
                return false;
            }
            isinsideFunction = false;
        }
    }
    if(isinsideFunction)
    {
        return false;
    }
    return true;
    // for each func defined there should be exactly one func_end && no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(
    FILE *f,
    int64_t offsetField,
    const string &text
)
{
    int64_t startPosition = fileTell64(f);

    if (
        startPosition < 0 ||
        text.size() >
        static_cast<size_t>(
            INT32_MAX
        )
    )
        return -1;

    int32_t stringSize =
        static_cast<int32_t>(text.size());

    if (
        fwrite(&offsetField, sizeof(int64_t), 1, f) != 1 ||

        fwrite(&stringSize, sizeof(int32_t), 1, f) != 1 ||

        (
            stringSize > 0 &&
            fwrite(
                text.data(),
                1,
                static_cast<size_t>(stringSize),
                f
            ) != static_cast<size_t>(stringSize)
        )
    )
        return -1;

    return startPosition;
}


int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offsetField;
    int32_t stringSize;

    if (
        fread(&offsetField, sizeof(int64_t), 1, f) != 1
    )
        return -1;

    if (
        fread(&stringSize, sizeof(int32_t), 1, f) != 1
    )
        return -1;

    if (
        stringSize < 0 ||
        static_cast<uint64_t>(stringSize) > MAX_SOURCE_BYTES
    )
        return -1;

    outText.resize(
        static_cast<size_t>(stringSize)
    );

    if (stringSize > 0)
    {
        if (
            fread(
                &outText[0],
                1,
                static_cast<size_t>(stringSize),
                f
            ) != static_cast<size_t>(stringSize)
        )
            return -1;
    }

    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) && CALL (remember which function it needs
    // && where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 


    ifstream source(sourcePath, ios::binary); // code starts here

    if(!source.is_open())
    {
        return -1;
    }

    FILE * resolveFile = fopen(resolveBinPath,"w+b"); // write in binary

    if(resolveFile == nullptr)
    {
        return -1;
    }

    string line;

    while(readSourceLine(source,line))

    {
        int64_t currentPosition =
    fileTell64(resolveFile);

if (
    currentPosition < 0 ||
    writeResolveRecord(
        resolveFile,
        currentPosition,
        line
    ) < 0
    )
    {
        fclose(resolveFile);
        return -1;
    }
        string first_word = firstWord(line);

        if(first_word == "func")
        {
            if(funcCount >= MAX_FUNCS)
            {
                fclose(resolveFile);
                return -1; // we had more functions here
            }

            funcArray[funcCount].funcName = secondWord(line); // extract the name of function && store
            funcArray[funcCount].byteOffsetInResolveBin = currentPosition; 
            funcCount++;
        }
        else if(first_word == "call")
        {
            if(patchCount >= MAX_PATCHES)
            {
                fclose(resolveFile);
                return -1;
            }

            patches[patchCount].byteOffsetOfOffsetField = currentPosition; // this tells we have to cocme back to this offset to replace the location when the function is found
            patches[patchCount].targetFuncName = secondWord(line);

            patchCount++; // if we have a function to be called we will put it inside the patch struct with it's values
        }
    }
    
    if (source.bad())
    {
        fclose(resolveFile);
        return -1;
    }
    for(int i = 0; i<patchCount; i++)
    {
        int64_t targetOffset = -1;

        for(int j = 0; j<funcCount; j++)
        {
            if(patches[i].targetFuncName == funcArray[j].funcName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if(targetOffset == -1)
        {
            fclose(resolveFile);
            return -1;
        }

        if (
    !fileSeek64(
        resolveFile,
        patches[i].byteOffsetOfOffsetField,
        SEEK_SET
    ) ||

    fwrite(
        &targetOffset,
        sizeof(int64_t),
        1,
        resolveFile
    ) != 1
)
    {
        fclose(resolveFile);
        return -1;
    }
    }

    int64_t mainOffset = -1;

    for (int i = 0; i < funcCount; i++)
    {   
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }

    fclose(resolveFile);

    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    enum TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int32_t tokenCount = 0;
    int i = 0;

    while(i < line.length() && tokenCount < maxTokens)
    {

        while(i < line.length() && (line[i] == '\r' || line[i] == '\t' || line[i] == ' '))
        {
            i++;
        }

        if(i >=line.length()) 
        {
            break; // if we go past the line we will break the main while && return tokens = 0 , indicates issues with line
        }

        // upto this point we have successfully moved past the trailing spaces
        string word = ""; // word initilization

        while(i <line.length() && line[i] != '\r' && line[i] != '\t' && line[i] != ' ')
        {
            word += line[i];
            i++;
        }

        tokens[tokenCount].text = word;

        if(tokenCount == 0)
        {
            tokens[tokenCount].type = KEYWORD;
        }
        else if(tokenCount == 1)
        {
            tokens[tokenCount].type = IDENTIFIER;
        }
        else
        {
            tokens[tokenCount].type = PARAM;
        }

        tokenCount++;
    }

    return tokenCount;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul && div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot * snapshot = new Snapshot;
    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack,MAX_STACK_DEPTH);
    return snapshot; // copies the callStack
    // build the snapshot based on the callStack given
}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *file = fopen(resolveBinPath, "rb");

    if(file == nullptr)
    {
        return;
    }
    if(mainOffset < 0)
    {
        fclose(file);
        return;
    }

    Stack<Frame> callStack;

    string callerArgumentNames[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];
    int32_t callerArgumentCounts[MAX_STACK_DEPTH] = {};

    if (!fileSeek64(file, mainOffset, SEEK_SET))
    {
        fclose(file);
        return;
    }

    string line;

    if(readResolveRecord(file, line) == -1)
    {
        fclose(file);
        return;
    }

    Frame mainFrame;

    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;

    callStack.push(mainFrame);

    while(!callStack.isEmpty())
    {
        int64_t offsetField = readResolveRecord(file, line);

        if(offsetField == -1)
        {
            break;
        }

        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(line, tokens, MAX_TOKENS);

        if(tokenCount == 0)
        {
            continue; // found no token in this line so we move forward to next line
        }

        string keyword = tokens[0].text; // first word 

        Frame &currentFrame = callStack.peek();

        if(keyword == "set")
        {
            if(tokenCount < 3)
            {
                continue;
            }
            string variableName = tokens[1].text; 
            int32_t value = stoi(tokens[2].text);

            Variable *variable = nullptr;

            for(int i = 0; i < currentFrame.argc; i++)
            {
                if(currentFrame.argv[i].name == variableName)
                {
                    variable = &currentFrame.argv[i];
                    break;
                }
            }

            if(variable == nullptr)
            {
                for(int i = 0; i < currentFrame.localCount; i++)
                {
                    if(currentFrame.locals[i].name == variableName)
                    {
                        variable = &currentFrame.locals[i];
                        break;
                    }
                }
            }

            if(variable != nullptr)
            {
                variable->value = value;
            }
            else
            {
                if(currentFrame.localCount < MAX_VARS_PER_FRAME)
                {
                    currentFrame.locals[currentFrame.localCount].name = variableName;
                    currentFrame.locals[currentFrame.localCount].value = value;

                    currentFrame.localCount++;
                }
            }
        }

        else if(keyword == "add" || keyword == "sub" ||
                keyword == "mul" || keyword == "div")
        {
            if(tokenCount < 3)
            {
                continue;
            }

            Variable *first = nullptr;
            Variable *second = nullptr;

            for(int i = 0; i < currentFrame.argc; i++)
            {
                if(currentFrame.argv[i].name == tokens[1].text)
                {
                    first = &currentFrame.argv[i];
                }

                if(currentFrame.argv[i].name == tokens[2].text)
                {
                    second = &currentFrame.argv[i];
                }
            }

            for(int i = 0; i < currentFrame.localCount; i++)
            {
                if(currentFrame.locals[i].name == tokens[1].text)
                {
                    first = &currentFrame.locals[i];
                }

                if(currentFrame.locals[i].name == tokens[2].text)
                {
                    second = &currentFrame.locals[i];
                }
            }

            if(first != nullptr && second != nullptr)
            {
                if(keyword == "add")
                {
                    first->value = first->value + second->value;
                }
                else if(keyword == "sub")
                {
                    first->value = first->value - second->value;
                }
                else if(keyword == "mul")
                {
                    first->value = first->value * second->value;
                }
                else if(keyword == "div")
                {
                    if(second->value != 0)
                    {
                        first->value = first->value / second->value;
                    }
                }
            }
        }

        else if(keyword == "call")
        {
            if(tokenCount < 2)
            {
                continue;
            }

            int64_t returnPosition64 = fileTell64(file);

            if (
                returnPosition64 < 0 ||
                returnPosition64 >
                INT32_MAX
            )
                break;

            int32_t returnPosition =
                static_cast<int32_t>(returnPosition64);

            Frame &callerFrame = callStack.peek();

            if (!fileSeek64(file, offsetField, SEEK_SET))
                break;

            string functionLine;

            if(readResolveRecord(file, functionLine) == -1)
            {
                break;
            }

            Token functionTokens[MAX_TOKENS];

            int32_t functionTokenCount =
                tokenizeLine(functionLine, functionTokens, MAX_TOKENS);

            if(functionTokenCount < 2 ||
               functionTokens[0].text != "func" ||
               functionTokenCount != tokenCount)
            {
                break;
            }

            if(callStack.depth() >= MAX_STACK_DEPTH)
            {
                break;
            }

            Frame newFrame;

            int32_t newDepth = callStack.depth();
            callerArgumentCounts[newDepth] = 0;

            newFrame.func_name = tokens[1].text;
            newFrame.argc = 0;
            newFrame.localCount = 0;
            newFrame.returnLine = returnPosition;

            bool validArguments = true;

            for(int i = 2; i < functionTokenCount && i < tokenCount; i++)
            {
                string argumentName = tokens[i].text;

                Variable *argument = nullptr;

                for(int j = 0; j < callerFrame.argc; j++)
                {
                    if(callerFrame.argv[j].name == argumentName)
                    {
                        argument = &callerFrame.argv[j];
                        break;
                    }
                }

                if(argument == nullptr)
                {
                    for(int j = 0; j < callerFrame.localCount; j++)
                    {
                        if(callerFrame.locals[j].name == argumentName)
                        {
                            argument = &callerFrame.locals[j];
                            break;
                        }
                    }
                }

                if(argument != nullptr &&
                   newFrame.argc < MAX_VARS_PER_FRAME)
                {
                    newFrame.argv[newFrame.argc].name =
                        functionTokens[i].text;

                    newFrame.argv[newFrame.argc].value =
                        argument->value;

                    callerArgumentNames[newDepth][newFrame.argc] =
                        argumentName;

                    callerArgumentCounts[newDepth]++;

                    newFrame.argc++;
                }
                else
                {
                    validArguments = false;
                    break;
                }
            }

            if(!validArguments)
            {
                break;
            }

            callStack.push(newFrame);
        }

        else if(keyword == "func_end")
        {
            int32_t finishedDepth = callStack.depth() - 1;

            Frame finishedFrame = callStack.pop();

            if(callStack.isEmpty())
            {
                Snapshot *snapshot = buildSnapshot(callStack);
                timeline.record(snapshot);
                break;
            }

            Frame &callerFrame = callStack.peek();

            for(int i = 0; i < callerArgumentCounts[finishedDepth]; i++)
            {
                string callerName = callerArgumentNames[finishedDepth][i];

                bool found = false;

                for(int j = 0; j < callerFrame.argc; j++)
                {
                    if(callerFrame.argv[j].name == callerName)
                    {
                        callerFrame.argv[j].value =
                            finishedFrame.argv[i].value;

                        found = true;
                        break;
                    }
                }

                if(!found)
                {
                    for(int j = 0; j < callerFrame.localCount; j++)
                    {
                        if(callerFrame.locals[j].name == callerName)
                        {
                            callerFrame.locals[j].value =
                                finishedFrame.argv[i].value;

                            break;
                        }
                    }
                }
            }

            if (!fileSeek64(file,finishedFrame.returnLine,SEEK_SET))
                break;
        }

        Snapshot *snapshot = buildSnapshot(callStack);
        timeline.record(snapshot);
    }
    fclose(file);
}

bool writeString(FILE *file, const string &str)
{
    if (
        str.size() >
        static_cast<size_t>(
            INT32_MAX
        )
    )
        return false;

    int32_t length =
        static_cast<int32_t>(str.size());

    if (
        std::fwrite(
            &length,
            sizeof(int32_t),
            1,
            file
        ) != 1
    )
        return false;

    return
        length == 0 ||

        std::fwrite(
            str.data(),
            1,
            static_cast<size_t>(length),
            file
        ) == static_cast<size_t>(length);
}


bool writeVariable(FILE *file, const Variable &var)
{
    return
        writeString(file, var.name) &&

        std::fwrite(
            &var.value,
            sizeof(int32_t),
            1,
            file
        ) == 1;
}


bool writeFrame(FILE *file, const Frame &frame)
{
    if (
        frame.argc < 0 ||
        frame.argc > MAX_VARS_PER_FRAME ||

        frame.localCount < 0 ||
        frame.localCount > MAX_VARS_PER_FRAME
    )
        return false;

    if (!writeString(file, frame.func_name))
        return false;

    if (
        std::fwrite(
            &frame.argc,
            sizeof(int32_t),
            1,
            file
        ) != 1
    )
        return false;

    for (int i = 0; i < frame.argc; ++i)
    {
        if (!writeVariable(file, frame.argv[i]))
            return false;
    }

    if (
        std::fwrite(
            &frame.returnLine,
            sizeof(int32_t),
            1,
            file
        ) != 1
    )
        return false;

    if (
        std::fwrite(
            &frame.localCount,
            sizeof(int32_t),
            1,
            file
        ) != 1
    )
        return false;

    for (int i = 0; i < frame.localCount; ++i)
    {
        if (!writeVariable(file, frame.locals[i]))
            return false;
    }

    return true;
}


bool writeSnapshot(FILE *file, const Snapshot &snapshot)
{
    if (
        snapshot.stackDepth < 0 ||
        snapshot.stackDepth > MAX_STACK_DEPTH
    )
        return false;

    if (
        std::fwrite(
            &snapshot.stackDepth,
            sizeof(int32_t),
            1,
            file
        ) != 1
    )
        return false;

    for (int i = 0; i < snapshot.stackDepth; ++i)
    {
        if (!writeFrame(file, snapshot.callStack[i]))
            return false;
    }

    return true;
}


// PASS 0x3: SERIALIZE TIMELINE

bool writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE *file = std::fopen(tdbgPath, "wb+");

    if (file == nullptr)
        return false;

    const int32_t stepCount =
        timeline.getStepCount();

    if (stepCount <= 0)
    {
        std::fclose(file);
        std::remove(tdbgPath);

        return false;
    }

    TTDBHeader header = {
        {'T', 'T', 'D', 'B'},
        TTDB_FORMAT_VERSION,
        stepCount,
        0
    };

    bool ok = writeHeader(file, header);

    int64_t *index = new int64_t[stepCount];

    TimelineNode *current = timeline.begin();

    int32_t i = 0;

    while (
        ok &&
        current != nullptr &&
        i < stepCount
    )
    {
        int64_t position = fileTell64(file);

        if (position < 0)
        {
            ok = false;
            break;
        }

        index[static_cast<size_t>(i)] = position;

        if (!writeSnapshot(file, *current->data))
        {
            ok = false;
            break;
        }

        current = current->next;

        ++i;
    }

    if (i != stepCount || current != nullptr)
        ok = false;

    if (ok)
    {
        header.indexOffset = fileTell64(file);

        if (header.indexOffset < 0)
            ok = false;
    }

    if (
        ok &&
        std::fwrite(
            index,
            sizeof(int64_t),
            static_cast<size_t>(stepCount),
            file
        ) != static_cast<size_t>(stepCount)
    )
        ok = false;

    if (
        ok &&
        !fileSeek64(file, 0, SEEK_SET)
    )
        ok = false;

    if (ok && !writeHeader(file, header))
        ok = false;

    if (ok && std::fflush(file) != 0)
        ok = false;

    if (std::fclose(file) != 0)
        ok = false;

    delete[] index;

    if (!ok)
        std::remove(tdbgPath);

    return ok;
}
// Phase 2 ----- server implementation


// ======================= PHASE 2 SERVER TRANSPORT =======================



void printTimeline(Timeline &timeline)
{
    TimelineNode* current = timeline.begin();
    int step = 1;

    while (current != nullptr)
    {
        Snapshot* s = current->data;

        cout << "\nStep " << step++ << endl;
        cout << "Stack depth: " << s->stackDepth << endl;

        for (int i = 0; i < s->stackDepth; i++)
        {
            Frame &f = s->callStack[i];

            cout << "Function: " << f.func_name << endl;

            for (int j = 0; j < f.argc; j++)
            {
                cout << "  Arg: " << f.argv[j].name
                     << " = " << f.argv[j].value << endl;
            }

            for (int j = 0; j < f.localCount; j++)
            {
                cout << "  Local: " << f.locals[j].name
                     << " = " << f.locals[j].value << endl;
            }
        }

        current = current->next;
    }
}



int createServerListenSocket(int port)
{
    int listenSock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (listenSock < 0)
    {
        perror("server: socket failed");

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
        listen(listenSock, 16) != 0
    )
    {
        perror("server: bind/listen failed");

        close(listenSock);

        return -1;
    }

    return listenSock;
}


bool receiveUploadToFile(
    int clientSock,
    const char *sourcePath,
    string &errorText
)
{
    FILE *output = std::fopen(sourcePath, "wb");

    if (output == nullptr)
    {
        errorText = "server could not create source file";

        // Drain the upload before replying so the client can finish sending.
        unsigned char discardBuffer[IO_BUFFER_SIZE];

        while (true)
        {
            int received = receiveSome(
                clientSock,
                discardBuffer,
                sizeof(discardBuffer)
            );

            if (received == 0)
                break;


            if (received < 0)
                break;
        }

        return false;
    }

    TrailerChecker checker;

    unsigned char input[IO_BUFFER_SIZE];
    unsigned char payload[IO_BUFFER_SIZE];

    bool haveStatus = false;

    unsigned char status = 255;

    uint64_t payloadBytes = 0;

    bool tooLarge = false;
    bool writeFailed = false;
    bool receiveFailed = false;

    while (true)
    {
        int received = receiveSome(
            clientSock,
            input,
            sizeof(input)
        );

        if (received == 0)
            break;

        if (received < 0)
        {

            receiveFailed = true;
            break;
        }

        size_t ready = checker.feed(
            input,
            static_cast<size_t>(received),
            payload
        );

        size_t offset = 0;

        if (!haveStatus && ready > 0)
        {
            status = payload[0];

            haveStatus = true;

            offset = 1;
        }

        if (ready > offset)
        {
            size_t bodyCount = ready - offset;

            payloadBytes += bodyCount;

            if (payloadBytes > MAX_SOURCE_BYTES)
            {
                tooLarge = true;
            }
            else if (
                !writeFailed &&
                std::fwrite(
                    payload + offset,
                    1,
                    bodyCount,
                    output
                ) != bodyCount
            )
            {
                writeFailed = true;
            }
        }
    }

    bool checksumOK = checker.verify();

    if (std::fflush(output) != 0)
        writeFailed = true;

    if (std::fclose(output) != 0)
        writeFailed = true;

    if (receiveFailed)
        errorText = "upload interrupted";

    else if (!checksumOK)
        errorText = "upload checksum mismatch";

    else if (!haveStatus || status != 0)
        errorText = "invalid upload status";

    else if (tooLarge)
        errorText = "source file exceeds 15 MiB limit";

    else if (writeFailed)
        errorText = "server could not write source file";

    if (!errorText.empty())
    {
        std::remove(sourcePath);

        return false;
    }

    return true;
}



bool runPhaseOnePipeline(
    const char *sourcePath,
    const char *resolvePath,
    const char *tdbgPath,
    string &errorText
)
{
    std::remove(resolvePath);
    std::remove(tdbgPath);

    if (!validateProgram(sourcePath))
    {
        errorText = "Invalid program structure";

        return false;
    }

    int64_t mainOffset =
        resolveProgram(sourcePath, resolvePath);

    if (mainOffset < 0)
    {
        errorText =
            "Main function missing or invalid function call";

        return false;
    }

    try
    {
        Timeline timeline;

        executeProgram(
            resolvePath,
            mainOffset,
            timeline
        );

        if (timeline.getStepCount() <= 0)
        {
            errorText =
                "Program execution produced no snapshots";

            return false;
        }

        if (!writeTdbg(timeline, tdbgPath))
        {
            errorText = "Could not write .tdbg file";

            return false;
        }
    }
    catch (const std::exception &exception)
    {
        errorText =
            string("Program execution failed: ") +
            exception.what();

        std::remove(tdbgPath);

        return false;
    }
    catch (...)
    {
        errorText = "Program execution failed";

        std::remove(tdbgPath);

        return false;
    }

    return true;
}


void handleServerClient(int clientSock)
{
    setSocketTimeouts(
        clientSock,
        SOCKET_TIMEOUT_SEC
    );

    string errorText;

    if (
        !receiveUploadToFile(
            clientSock,
            "source.bin",
            errorText
        )
    )
    {
        sendCheckedBytes(
            clientSock,
            1,
            errorText.data(),
            errorText.size()
        );

        return;
    }

    if (
        !runPhaseOnePipeline(
            "source.bin",
            "resolve.bin",
            "session.tdbg",
            errorText
        )
    )
    {
        sendCheckedBytes(
            clientSock,
            1,
            errorText.data(),
            errorText.size()
        );

        return;
    }

    FILE *tdbgFile =
        std::fopen("session.tdbg", "rb");

    if (tdbgFile == nullptr)
    {
        const string message =
            "Could not open generated .tdbg file";

        sendCheckedBytes(
            clientSock,
            1,
            message.data(),
            message.size()
        );

        return;
    }

    if (!sendCheckedFile(clientSock, 0, tdbgFile))
    {
        cerr << "server: failed while sending .tdbg response\n";
    }

    std::fclose(tdbgFile);
}


// ======================= SERVER MAIN =======================

int main(int argc, char *argv[])
{
    int port = 0;

    if (argc != 2 || !parsePort(argv[1], port))
    {
        cerr << "Usage: ./server <port>\n";

        return 1;
    }

    int listenSock = createServerListenSocket(port);

    if (listenSock < 0)
        return 1;

    cout << "server listening on port " << port << "\n";

    // The load balancer sends one client at a time, so serving them one by one is enough.
    while (true)
    {
        int clientSock = accept(listenSock, nullptr, nullptr);

        if (clientSock < 0)
        {
            perror("server: accept failed");

            usleep(100000);

            continue;
        }

        handleServerClient(clientSock);

        closeSocket(clientSock);
    }

    return 0;
}