#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <process.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <time.h>

// libember_slim headers for Glow types and helpers
#include "emberplus.h"
#include "emberinternal.h"

// Ensure Ws2_32 gets linked even outside MSVC pragmas
#pragma comment(lib, "Ws2_32.lib")

// Serialize all outbound writes to avoid frame interleaving across threads
static CRITICAL_SECTION g_sendCs;
static int g_sendCsInitialized = 0;
static int send_with_lock(SOCKET s, const char* buf, int len, int flags)
{
    if (g_sendCsInitialized) EnterCriticalSection(&g_sendCs);
    // Use WSASend to avoid recursion if send is macro-replaced
    WSABUF wbuf; DWORD sent = 0; int rc;
    wbuf.buf = (CHAR*)buf; wbuf.len = (ULONG)len;
    rc = WSASend(s, &wbuf, 1, &sent, 0, NULL, NULL);
    if (g_sendCsInitialized) LeaveCriticalSection(&g_sendCs);
    if (rc == 0) return (int)sent; else return SOCKET_ERROR;
}

// Replace send() in included sample with our locked variant
#define send send_with_lock

// ----------------------------------------------------------------------
// Inlined parts from __sample_provider.c (utilities, streams, tree, callbacks)
// ----------------------------------------------------------------------

#ifndef SECURE_CRT
#define stringCopy(dest, size, source) \
   do { strncpy(dest, source, size); dest[(size) - 1] = 0; } while(0)
#else
#define stringCopy(dest, size, source) \
   do { strncpy_s(dest, size, source, (size) - 1); dest[(size) - 1] = 0; } while(0)
#endif

#define stringDup(pStr) \
   (pStr != NULL ? _strdup(pStr) : NULL)

static void onThrowError(int error, pcstr pMessage)
{
   printf_s("ber error %d: '%s'\n", error, pMessage);
}

static void onFailAssertion(pcstr pFileName, int lineNumber)
{
   printf_s("Debug assertion failed @ '%s' line %d", pFileName, lineNumber);
}

// streams
#define STREAMS_COUNT (3)
#define STREAMS_MAX_STRING_LENGTH (16)

static GlowStreamEntry _streams[STREAMS_COUNT];

static void initializePpmStreams()
{
   bzero_item(_streams);

   _streams[0].streamIdentifier = 0;
   _streams[0].streamValue.flag = GlowParameterType_Integer;

   _streams[1].streamIdentifier = 1;
   _streams[1].streamValue.flag = GlowParameterType_Octets;
   _streams[1].streamValue.choice.octets.pOctets = newarr(byte, 8);
   _streams[1].streamValue.choice.octets.length = 8;

   _streams[2].streamIdentifier = 2;
   _streams[2].streamValue.flag = GlowParameterType_String;
   _streams[2].streamValue.choice.pString = newarr(char, STREAMS_MAX_STRING_LENGTH);
}

static void freePpmStreams()
{
   freeMemory(_streams[1].streamValue.choice.octets.pOctets);
   freeMemory(_streams[2].streamValue.choice.pString);
}

static void collectPpmData()
{
   _streams[0].streamValue.choice.integer = ((rand() % 80) - 64) * 32;
   *(short *)&_streams[1].streamValue.choice.octets.pOctets[0] = htons(((rand() % 256) - 128) * 32);
   *(berint *)&_streams[1].streamValue.choice.octets.pOctets[4] = ((rand() % 256) - 255) * 32;
   stringCopy(_streams[2].streamValue.choice.pString, STREAMS_MAX_STRING_LENGTH, "abc");
   _itoa_s(rand() % 100, &_streams[2].streamValue.choice.pString[3], STREAMS_MAX_STRING_LENGTH - 3, 10);
}

// sample tree
#define SampleNode_MaxChildren (16)

typedef struct __SampleNode
{
   bool isParameter;
   GlowFieldFlags fields;
   union { GlowNode node; GlowParameter param; };
   int childrenCount;
   struct __SampleNode *children[SampleNode_MaxChildren];
} SampleNode;

static void sampleNode_init(SampleNode *pThis, SampleNode *pParent)
{
   bzero_item(*pThis);
   if(pParent != NULL)
   {
      if(pParent->childrenCount < SampleNode_MaxChildren)
      {
         pParent->children[pParent->childrenCount] = pThis;
         pParent->childrenCount++;
      }
   }
}

static void sampleNode_free(SampleNode *pThis)
{
   int index;
   SampleNode *pChild;
   for(index = 0; index < pThis->childrenCount; index++)
   {
      pChild = pThis->children[index];
      if(pChild != NULL)
      {
         sampleNode_free(pChild);
         freeMemory(pChild);
      }
   }
   if(pThis->isParameter) glowParameter_free(&pThis->param); else glowNode_free(&pThis->node);
   bzero_item(*pThis);
}

static SampleNode *createNode(SampleNode *pParent, pcstr pIdentifier, pcstr pDescription)
{
   SampleNode *pNode = newobj(SampleNode);
   dword fields = GlowFieldFlag_Identifier;
   sampleNode_init(pNode, pParent);
   pNode->node.pIdentifier = stringDup(pIdentifier);
   if(pDescription != NULL)
   {
      pNode->node.pDescription = stringDup(pDescription);
      fields |= GlowFieldFlag_Description;
   }
   pNode->fields = (GlowFieldFlags)fields;
   return pNode;
}

static SampleNode *createParameter(SampleNode *pParent, pcstr pIdentifier, pcstr pDescription)
{
   SampleNode *pNode = newobj(SampleNode);
   dword fields = GlowFieldFlag_Identifier;
   sampleNode_init(pNode, pParent);
   pNode->isParameter = true;
   pNode->param.pIdentifier = stringDup(pIdentifier);
   if(pDescription != NULL)
   {
      pNode->param.pDescription = stringDup(pDescription);
      fields |= GlowFieldFlag_Description;
   }
   pNode->fields = (GlowFieldFlags)fields;
   return pNode;
}

static void createGain(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "gain", "power in db");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_ReadWrite; fields |= GlowFieldFlag_Access;
   pParam->param.value.choice.integer = 0; pParam->param.value.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Value;
   pParam->param.minimum.choice.integer = 0; pParam->param.minimum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Minimum;
   pParam->param.maximum.choice.integer = 65535; pParam->param.maximum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Maximum;
   pParam->param.factor = 64; fields |= GlowFieldFlag_Factor;
   pParam->param.pFormat = "%Lf db"; fields |= GlowFieldFlag_Format;
   pParam->param.pSchemaIdentifiers = stringDup("de.l-s-b.emberplus.samples.gain"); fields |= GlowFieldFlag_SchemaIdentifier;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createVolume(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "volume", "loud?");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_ReadWrite; fields |= GlowFieldFlag_Access;
   pParam->param.value.choice.real = 0.0; pParam->param.value.flag = GlowParameterType_Real; fields |= GlowFieldFlag_Value;
   pParam->param.minimum.choice.real = -1000.0; pParam->param.minimum.flag = GlowParameterType_Real; fields |= GlowFieldFlag_Minimum;
   pParam->param.maximum.choice.real = 1000.0; pParam->param.maximum.flag = GlowParameterType_Real; fields |= GlowFieldFlag_Maximum;
   pParam->param.pFormat = "%Lf db"; fields |= GlowFieldFlag_Format;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createFormat(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "format", "simple enum");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_ReadWrite; fields |= GlowFieldFlag_Access;
   pParam->param.value.choice.integer = 0; pParam->param.value.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Value;
   pParam->param.pEnumeration = "4:3\n16:9\nHD\nFull HD\nCinema"; fields |= GlowFieldFlag_Enumeration;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createStream1(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "stream1", "1/32 db");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_Read; fields |= GlowFieldFlag_Access;
   pParam->param.value.flag = GlowParameterType_Integer; pParam->param.value.choice.integer = 0; fields |= GlowFieldFlag_Value;
   pParam->param.minimum.choice.integer = -64 * 32; pParam->param.minimum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Minimum;
   pParam->param.maximum.choice.integer = 15 * 32; pParam->param.maximum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Maximum;
   pParam->param.streamIdentifier = 0; fields |= GlowFieldFlag_StreamIdentifier;
   pParam->param.pFormula = "($/32 + log(2) - 1/5 + e^(1/4))\n($*32)"; fields |= GlowFieldFlag_Formula;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createStream2(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "stream2", "1/32 db");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_None; fields |= GlowFieldFlag_Access;
   pParam->param.type = GlowParameterType_Integer; fields |= GlowFieldFlag_Type;
   pParam->param.factor = 32; fields |= GlowFieldFlag_Factor;
   pParam->param.minimum.choice.integer = -128 * 32; pParam->param.minimum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Minimum;
   pParam->param.maximum.choice.integer = 127 * 32; pParam->param.maximum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Maximum;
   pParam->param.streamIdentifier = 1; fields |= GlowFieldFlag_StreamIdentifier;
   pParam->param.streamDescriptor.format = GlowStreamFormat_SignedInt16BigEndian;
   pParam->param.streamDescriptor.offset = 0; fields |= GlowFieldFlag_StreamDescriptor;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createStream3(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "stream3", "1/32 db");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_None; fields |= GlowFieldFlag_Access;
   pParam->param.type = GlowParameterType_Integer; fields |= GlowFieldFlag_Type;
   pParam->param.factor = 32; fields |= GlowFieldFlag_Factor;
   pParam->param.minimum.choice.integer = -255 * 32; pParam->param.minimum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Minimum;
   pParam->param.maximum.choice.integer = 0 * 32; pParam->param.maximum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Maximum;
   pParam->param.streamIdentifier = 1; fields |= GlowFieldFlag_StreamIdentifier;
   pParam->param.streamDescriptor.format = GlowStreamFormat_SignedInt32LittleEndian;
   pParam->param.streamDescriptor.offset = 4; fields |= GlowFieldFlag_StreamDescriptor;
   pParam->fields = (GlowFieldFlags)fields;
}

static void createStream4(SampleNode *pParent)
{
   SampleNode *pParam = createParameter(pParent, "stream4", "strings");
   dword fields = pParam->fields;
   pParam->param.access = GlowAccess_None; fields |= GlowFieldFlag_Access;
   pParam->param.type = GlowParameterType_String; fields |= GlowFieldFlag_Type;
   pParam->param.maximum.choice.integer = STREAMS_MAX_STRING_LENGTH - 1; pParam->param.maximum.flag = GlowParameterType_Integer; fields |= GlowFieldFlag_Maximum;
   pParam->param.streamIdentifier = 2; fields |= GlowFieldFlag_StreamIdentifier;
   pParam->fields = (GlowFieldFlags)fields;
}

static void buildTree(SampleNode *pRoot)
{
   SampleNode *pVisualRoot, *pFood, *pNonFood, *pStreams, *pAudio;
   pVisualRoot = createNode(pRoot, "root", "root");
   pFood = createNode(pVisualRoot, "food", "Some food");
   createNode(pFood, "parsley", "tasty herb");
   createNode(pFood, "salad", "eat a lot of it!");
   pNonFood = createNode(pVisualRoot, "non-food", "tech stuff");
   pAudio = createNode(pNonFood, "audio", "ask Lawo");
   createGain(pAudio); createVolume(pAudio); createFormat(pAudio); createNode(pAudio, "dangling", NULL);
   pStreams = createNode(pNonFood, "streams", "see 'em move");
   pStreams->node.pSchemaIdentifiers = stringDup("de.l-s-b.emberplus.samples.streams");
   pStreams->fields |= GlowFieldFlag_SchemaIdentifier;
   createStream1(pStreams); createStream2(pStreams); createStream3(pStreams); createStream4(pStreams);
}

static SampleNode *findNode(SampleNode *pRoot, const berint *pPath, int *pPathLength)
{
   int pathIndex; int nodeIndex; SampleNode *pCursor = pRoot;
   for(pathIndex = 0; pathIndex < *pPathLength; pathIndex++)
   {
      nodeIndex = pPath[pathIndex];
      if(nodeIndex < pCursor->childrenCount) pCursor = pCursor->children[nodeIndex]; else break;
   }
   *pPathLength = pathIndex; return pCursor;
}

typedef struct { SOCKET sock; unsigned int streamSubscriptions[STREAMS_COUNT]; pstr pEnumeration; } ClientInfo;

SampleNode _root;

static void onCommand(const GlowCommand *pCommand, const berint *pPath, int pathLength, voidptr state)
{
   int bufferSize = 512; byte *pBuffer; GlowOutput output; ClientInfo *pClientInfo = (ClientInfo *)state; berint *pOutPath; int nodeIndex; SampleNode *pCurrent; SampleNode *pCursor = findNode(&_root, pPath, &pathLength); SOCKET sock = pClientInfo->sock; int fields;
   if(pCommand->number == GlowCommandType_GetDirectory)
   {
      pOutPath = newarr(berint, GLOW_MAX_TREE_DEPTH); memcpy(pOutPath, pPath, pathLength * sizeof(berint)); pBuffer = newarr(byte, bufferSize); glowOutput_init(&output, pBuffer, bufferSize, 0);
      if(pCursor->isParameter)
      {
         fields = pCursor->fields & pCommand->options.dirFieldMask; glowOutput_beginPackage(&output, true); glow_writeQualifiedParameter(&output, &pCursor->param, (GlowFieldFlags)fields, pOutPath, pathLength); send(sock, (char *)pBuffer, glowOutput_finishPackage(&output), 0);
      }
      else if(pCursor->childrenCount == 0)
      {
         fields = 0; glowOutput_beginPackage(&output, true); glow_writeQualifiedNode(&output, &pCursor->node, (GlowFieldFlags)fields, pOutPath, pathLength); send(sock, (char *)pBuffer, glowOutput_finishPackage(&output), 0);
      }
      else
      {
         for(nodeIndex = 0; nodeIndex < pCursor->childrenCount; nodeIndex++)
         {
            pCurrent = pCursor->children[nodeIndex];
            if(pCurrent != NULL)
            {
               glowOutput_beginPackage(&output, nodeIndex == pCursor->childrenCount - 1); pOutPath[pathLength] = nodeIndex; fields = pCurrent->fields & pCommand->options.dirFieldMask;
               if(pCurrent->isParameter) glow_writeQualifiedParameter(&output, &pCurrent->param, (GlowFieldFlags)fields, pOutPath, pathLength + 1); else glow_writeQualifiedNode(&output, &pCurrent->node, (GlowFieldFlags)fields, pOutPath, pathLength + 1);
               send(sock, (char *)pBuffer, glowOutput_finishPackage(&output), 0);
            }
         }
      }
      freeMemory(pBuffer); freeMemory(pOutPath);
   }
   else if(pCommand->number == GlowCommandType_Subscribe)
   {
      if(pCursor->isParameter && pCursor->param.streamIdentifier >= 0 && pCursor->param.streamIdentifier < STREAMS_COUNT)
         pClientInfo->streamSubscriptions[pCursor->param.streamIdentifier]++;
   }
   else if(pCommand->number == GlowCommandType_Unsubscribe)
   {
      if(pCursor->isParameter && pCursor->param.streamIdentifier >= 0 && pCursor->param.streamIdentifier < STREAMS_COUNT)
         pClientInfo->streamSubscriptions[pCursor->param.streamIdentifier]--;
   }
}

static void onParameter(const GlowParameter *pParameter, GlowFieldFlags fields, const berint *pPath, int pathLength, voidptr state)
{
   GlowOutput output; const int bufferSize = 512; byte *pBuffer; ClientInfo *pClientInfo = (ClientInfo *)state; SampleNode *pCursor = findNode(&_root, pPath, &pathLength); SOCKET sock = pClientInfo->sock;
   if(pCursor->isParameter && (fields & GlowFieldFlag_Value) == GlowFieldFlag_Value)
   {
      glowValue_free(&pCursor->param.value); glowValue_copyFrom(&pCursor->param.value, &pParameter->value);
      pBuffer = newarr(byte, bufferSize); glowOutput_init(&output, pBuffer, bufferSize, 0); glowOutput_beginPackage(&output, true);
      glow_writeQualifiedParameter(&output, &pCursor->param, GlowFieldFlag_Value, pPath, pathLength); send(sock, (char *)pBuffer, glowOutput_finishPackage(&output), 0); freeMemory(pBuffer);
   }
}

static void onOtherPackageReceived(const byte *pPackage, int length, voidptr state)
{
   ClientInfo *pClientInfo = (ClientInfo *)state; byte buffer[16]; unsigned int txLength;
   if(length >= 4 && pPackage[1] == EMBER_MESSAGE_ID && pPackage[2] == EMBER_COMMAND_KEEPALIVE_REQUEST)
   {
      txLength = emberFraming_writeKeepAliveResponse(buffer, sizeof(buffer), pPackage[0]); send(pClientInfo->sock, (char *)buffer, txLength, 0);
   }
}

static void sendPpmStreams(SOCKET sock, byte *pBuffer, int bufferSize, const ClientInfo *pClientInfo)
{
   GlowOutput output; int index; int entriesWritten = 0;
   for(index = 0; index < STREAMS_COUNT; index++)
   {
      if(pClientInfo->streamSubscriptions[index] > 0)
      {
         if(entriesWritten == 0)
         { glowOutput_init(&output, pBuffer, bufferSize, 0); glowOutput_beginStreamPackage(&output, true); }
         glow_writeStreamEntry(&output, &_streams[index]); entriesWritten++;
      }
   }
   if(entriesWritten > 0) send(sock, (char *)pBuffer, glowOutput_finishPackage(&output), 0);
}

static void initSockets()
{ WSADATA wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData); }

static void shutdownSockets()
{ WSACleanup(); }

// ----------------------------------------------------------------------
// Globals to bridge JSON thread updates to the current Ember+ client
// ----------------------------------------------------------------------
static volatile SOCKET g_clientSock = INVALID_SOCKET;

// Forward decls from included sample (in same TU)
extern SampleNode _root;
static const char* safeIdent(SampleNode* n)
{
    if (!n) return "?";
    if (n->isParameter) return n->param.pIdentifier ? n->param.pIdentifier : "?";
    return n->node.pIdentifier ? n->node.pIdentifier : "?";
}

static void path_to_ident_string(const berint *pPath, int pathLength, char *out, size_t cap)
{
    out[0] = '\0';
    SampleNode *cursor = &_root;
    size_t used = 0;
    for (int i = 0; i < pathLength; ++i)
    {
        int idx = (int)pPath[i];
        if (idx < 0 || idx >= cursor->childrenCount) break;
        SampleNode *child = cursor->children[idx];
        const char *id = safeIdent(child);
        size_t len = strlen(id);
        if (used && used + 1 < cap) out[used++] = '.';
        if (used + len >= cap) len = cap - used - 1;
        memcpy(out + used, id, len);
        used += len;
        out[used] = '\0';
        cursor = child;
    }
}

// ----------------------------------------------------------------------
// Utilities: trim, parse minimal JSON line (path + value)
// ----------------------------------------------------------------------
static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t n = strlen(s);
    while (n>0 && isspace((unsigned char)s[n-1])) { s[--n] = '\0'; }
}

static int parse_json_line(const char *line, char *outPath, size_t outPathSize, long double *outValue)
{
    // extremely small JSON extractor: expects {"path":"...","value":<num>}
    const char *pPath = strstr(line, "\"path\"");
    const char *pValue = strstr(line, "\"value\"");
    if (!pPath || !pValue) return 0;
    const char *q = strchr(pPath, '"');
    if (!q) return 0; // first quote
    q = strchr(q+1, '"');
    if (!q) return 0; // second quote after key
    const char *start = strchr(q+1, '"');
    if (!start) return 0;
    const char *end = strchr(start+1, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - (start+1));
    if (len >= outPathSize) len = outPathSize - 1;
    memcpy(outPath, start+1, len);
    outPath[len] = '\0';

    // value: find ':' after "value"
    const char *colon = strchr(pValue, ':');
    if (!colon) return 0;
    char *endptr = NULL;
    long double v = strtold(colon+1, &endptr);
    if (endptr == colon+1) return 0; // no number
    *outValue = v;
    return 1;
}

// ----------------------------------------------------------------------
// Tree helpers: find node by identifier path, compute numeric path
// ----------------------------------------------------------------------
static int get_child_index_by_identifier(SampleNode *parent, const char *ident)
{
    int i;
    for (i = 0; i < parent->childrenCount; i++)
    {
        SampleNode *c = parent->children[i];
        if (c == NULL) continue;
        if (c->isParameter)
        {
            if (c->param.pIdentifier && strcmp(c->param.pIdentifier, ident) == 0)
                return i;
        }
        else
        {
            if (c->node.pIdentifier && strcmp(c->node.pIdentifier, ident) == 0)
                return i;
        }
    }
    return -1;
}

static SampleNode* find_by_identifier_path(const char *path, berint *outIdxPath, int *outLen)
{
    // path like: root.non-food.audio.volume
    char buf[256];
    strncpy(buf, path, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    SampleNode *cursor = &_root;
    int len = 0;
    char *saveptr = NULL;
    char *tok = strtok_s(buf, ".", &saveptr);
    while (tok)
    {
        int idx = get_child_index_by_identifier(cursor, tok);
        if (idx < 0) break;
        if (outIdxPath && len < GLOW_MAX_TREE_DEPTH)
            outIdxPath[len] = (berint)idx;
        len++;
        cursor = cursor->children[idx];
        tok = strtok_s(NULL, ".", &saveptr);
    }
    if (outLen) *outLen = len;
    return cursor;
}

// Allow friendly aliases like "demo.level" and optional missing root prefix
static int try_resolve_with_variants(const char *input, berint *outIdxPath, int *outLen, SampleNode **outNode)
{
    // 1) direct
    SampleNode *n = find_by_identifier_path(input, outIdxPath, outLen);
    if (n && n->isParameter) { *outNode = n; return 1; }

    // 2) prepend root.
    if (strncmp(input, "root.", 5) != 0)
    {
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "root.%s", input);
        int len2 = 0; berint p2[GLOW_MAX_TREE_DEPTH];
        n = find_by_identifier_path(tmp, p2, &len2);
        if (n && n->isParameter) { memcpy(outIdxPath, p2, sizeof(berint)*len2); *outLen = len2; *outNode = n; return 1; }
    }

    // 3) friendly aliases
    if (strcmp(input, "demo.level") == 0)
    {
        const char *alias = "root.non-food.audio.volume";
        int len3 = 0; berint p3[GLOW_MAX_TREE_DEPTH];
        n = find_by_identifier_path(alias, p3, &len3);
        if (n && n->isParameter) { memcpy(outIdxPath, p3, sizeof(berint)*len3); *outLen = len3; *outNode = n; return 1; }
    }
    else if (strcmp(input, "demo.gain") == 0)
    {
        const char *alias = "root.non-food.audio.gain";
        int len3 = 0; berint p3[GLOW_MAX_TREE_DEPTH];
        n = find_by_identifier_path(alias, p3, &len3);
        if (n && n->isParameter) { memcpy(outIdxPath, p3, sizeof(berint)*len3); *outLen = len3; *outNode = n; return 1; }
    }
    else if (strcmp(input, "demo.format") == 0)
    {
        const char *alias = "root.non-food.audio.format";
        int len3 = 0; berint p3[GLOW_MAX_TREE_DEPTH];
        n = find_by_identifier_path(alias, p3, &len3);
        if (n && n->isParameter) { memcpy(outIdxPath, p3, sizeof(berint)*len3); *outLen = len3; *outNode = n; return 1; }
    }

    *outNode = n; // could be NULL or non-parameter
    return 0;
}

static void make_read_only_recursive(SampleNode *n)
{
    if (n == NULL) return;
    if (n->isParameter)
    {
        n->param.access = GlowAccess_Read;
        n->fields |= GlowFieldFlag_Access;
    }
    int i;
    for (i = 0; i < n->childrenCount; i++)
        make_read_only_recursive(n->children[i]);
}

// ----------------------------------------------------------------------
// Publish updated value to Ember+ client
// ----------------------------------------------------------------------
static void publish_parameter_value(SampleNode *paramNode, const berint *idxPath, int pathLen)
{
    printf_s("[tx] publish_parameter_value is called.\n");
    if (g_clientSock == INVALID_SOCKET) {
    printf_s("[tx] g_clientSock is INVALID_SOCKET. return.\n");
    return;
    }

    if (!(paramNode && paramNode->isParameter)){
    printf_s("[tx] !(paramNode && paramNode->isParameter). return.\n");
    return;
    }


    const int bufferSize = 512;
    byte *pBuffer = newarr(byte, bufferSize);
    GlowOutput output;
    glowOutput_init(&output, pBuffer, bufferSize, 0);
    glowOutput_beginPackage(&output, true);
    glow_writeQualifiedParameter(&output, &paramNode->param, GlowFieldFlag_Value, idxPath, pathLen);
    int bytes = glowOutput_finishPackage(&output);
    char pathStr[256];
    path_to_ident_string(idxPath, pathLen, pathStr, sizeof(pathStr));
    printf_s("[tx] send value update for %s, %d bytes\n", pathStr, bytes);
    int sent = send(g_clientSock, (char *)pBuffer, bytes, 0);
    if (sent == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        printf_s("[tx] send error WSA=%d\n", err);
    }
    freeMemory(pBuffer);
}

static void set_value_and_publish(const char *identifierPath, long double value)
{
    berint idxPath[GLOW_MAX_TREE_DEPTH];
    int pathLen = 0;
    SampleNode *n = NULL;
    int ok = try_resolve_with_variants(identifierPath, idxPath, &pathLen, &n);
    if (!ok)
    {
        printf_s("[json] target not found or not a parameter: %s\n", identifierPath);
        return;
    }

    // Update numeric types only (Integer/Real)
    if (n->param.value.flag == GlowParameterType_Real)
    {
        long double prev = n->param.value.choice.real;
        n->param.value.choice.real = value;
        char pathStr[256];
        path_to_ident_string(idxPath, pathLen, pathStr, sizeof(pathStr));
        printf_s("[json] updated REAL %s: %Lg -> %Lg\n", pathStr, prev, value);
    }
    else if (n->param.value.flag == GlowParameterType_Integer)
    {
        // clamp to berint range
        if (value > (long double)LLONG_MAX) value = (long double)LLONG_MAX;
        if (value < (long double)LLONG_MIN) value = (long double)LLONG_MIN;
        berint prev = n->param.value.choice.integer;
        n->param.value.choice.integer = (berint)(long long)value;
        char pathStr[256];
        path_to_ident_string(idxPath, pathLen, pathStr, sizeof(pathStr));
        printf_s("[json] updated INTEGER %s: %lld -> %lld\n", pathStr, (long long)prev, (long long)n->param.value.choice.integer);
    }
    else
    {
        printf_s("[json] unsupported parameter type for %s; only Integer/Real supported\n", identifierPath);
        return; // unsupported type for this simple updater
    }
    printf_s("[json] trying to call publish_parameter_value...\n");
    publish_parameter_value(n, idxPath, pathLen);
}

// ----------------------------------------------------------------------
// JSON TCP listener thread (127.0.0.1:5001)
// ----------------------------------------------------------------------
static unsigned __stdcall json_listener(void *arg)
{
    SOCKET ls = INVALID_SOCKET;
    struct sockaddr_in addr;
    int on = 1;

    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5001);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(ls); return 0; }
    if (listen(ls, 4) == SOCKET_ERROR) { closesocket(ls); return 0; }

    printf_s("[json] listening on 127.0.0.1:5001\n");

    for(;;)
    {
        struct sockaddr_in peer;
        int peerlen = sizeof(peer);
        char ipstr[INET_ADDRSTRLEN] = {0};
        SOCKET cs = accept(ls, (struct sockaddr*)&peer, &peerlen);
        if (cs == INVALID_SOCKET) break;
        inet_ntop(AF_INET, &peer.sin_addr, ipstr, sizeof(ipstr));
        printf_s("[json] client connected from %s:%d\n", ipstr, (int)ntohs(peer.sin_port));
        char buf[1024];
        int n = recv(cs, buf, sizeof(buf)-1, 0);
        if (n > 0)
        {
            buf[n] = '\0';
            // read only first line
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            trim(buf);
            printf_s("[json] received %d bytes: %s\n", n, buf);
            char path[256];
            long double val = 0.0;
            if (parse_json_line(buf, path, sizeof(path), &val))
            {
                printf_s("[json] parsed path='%s' value=%Lg\n", path, val);
                set_value_and_publish(path, val);
            }
            else
            {
                printf_s("[json] parse failed; expected {\"path\":...,\"value\":...}\n");
            }
        }
        else
        {
            printf_s("[json] recv returned %d\n", n);
        }
        closesocket(cs);
    }

    closesocket(ls);
    return 0;
}

// ----------------------------------------------------------------------
// Wrap sample callbacks with logs for subscribe/unsubscribe/value set
// ----------------------------------------------------------------------
static void onParameter_logged(const GlowParameter *pParameter, GlowFieldFlags fields, const berint *pPath, int pathLength, voidptr state)
{
    char pathStr[256];
    path_to_ident_string(pPath, pathLength, pathStr, sizeof(pathStr));
    if ((fields & GlowFieldFlag_Value) == GlowFieldFlag_Value)
    {
        if (pParameter->value.flag == GlowParameterType_Real)
            printf_s("[rx] SET value REAL %s = %Lg\n", pathStr, pParameter->value.choice.real);
        else if (pParameter->value.flag == GlowParameterType_Integer)
            printf_s("[rx] SET value INT %s = %lld\n", pathStr, (long long)pParameter->value.choice.integer);
        else if (pParameter->value.flag == GlowParameterType_String)
            printf_s("[rx] SET value STR %s = '%s'\n", pathStr, pParameter->value.choice.pString ? pParameter->value.choice.pString : "");
        else
            printf_s("[rx] SET value %s (type=%d)\n", pathStr, (int)pParameter->value.flag);
    }
    onParameter(pParameter, fields, pPath, pathLength, state);
}

static void onCommand_logged(const GlowCommand *pCommand, const berint *pPath, int pathLength, voidptr state)
{
    char pathStr[256];
    path_to_ident_string(pPath, pathLength, pathStr, sizeof(pathStr));
    if (pCommand->number == GlowCommandType_Subscribe)
    {
        printf_s("[rx] SUBSCRIBE %s\n", pathStr);
    }
    else if (pCommand->number == GlowCommandType_Unsubscribe)
    {
        printf_s("[rx] UNSUBSCRIBE %s\n", pathStr);
    }
    else if (pCommand->number == GlowCommandType_GetDirectory)
    {
        printf_s("[rx] GET-DIRECTORY %s (mask=0x%X)\n", pathStr, (unsigned int)pCommand->options.dirFieldMask);
    }
    onCommand(pCommand, pPath, pathLength, state);
}

// Forward declaration of real glowReader_init
void glowReader_init(GlowReader *pThis,
    onNode_t onNode,
    onParameter_t onParameter,
    onCommand_t onCommand,
    onStreamEntry_t onStreamEntry,
    voidptr state,
    byte *pRxBuffer,
    unsigned int rxBufferSize);

// Interposed reader init: inject our logging callbacks, then return
static void glowReader_init_wrapped(GlowReader *pReader,
    onNode_t onNode,
    onParameter_t onParam,
    onCommand_t onCmd,
    onStreamEntry_t onStream,
    voidptr state,
    byte *pRxBuffer,
    unsigned int rxBufferSize)
{
    // call real glowReader_init with our wrappers
    glowReader_init(pReader, onNode, onParameter_logged, onCommand_logged, onStream, state, pRxBuffer, rxBufferSize);
}

// ----------------------------------------------------------------------
// Wrappers: make all params read-only; start JSON listener; delegate
// ----------------------------------------------------------------------
static void handleClient(SOCKET sock)
{
    // Based on sample's handleClient, plus g_clientSock hookup
    byte buffer[64];
    int read;
    const int rxBufferSize = 1024;
    const struct timeval timeout = {0, 50 * 1000};
    const int noDelay = 1;
    const int streamBufferSize = 128;
    fd_set fdset; int fdsReady; ClientInfo client;

    GlowReader *pReader = newobj(GlowReader);
    byte *pRxBuffer = newarr(byte, rxBufferSize);
    byte *pStreamBuffer = newarr(byte, streamBufferSize);

    srand((unsigned int)time(NULL));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&noDelay, sizeof(noDelay));

    bzero_item(client); client.sock = sock;

    g_clientSock = sock; // expose to JSON publisher
    glowReader_init_wrapped(pReader, NULL, onParameter, onCommand, NULL, (voidptr)&client, pRxBuffer, rxBufferSize);
    pReader->onOtherPackageReceived = onOtherPackageReceived;

    while(true)
    {
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        fdsReady = select(1, &fdset, NULL, NULL, &timeout);
        if(fdsReady == 1)
        {
            if(FD_ISSET(sock, &fdset))
            {
                read = recv(sock, (char *)buffer, sizeof(buffer), 0);
                printf_s("received %d bytes\n", read);
                if(read > 0) glowReader_readBytes(pReader, buffer, read); else break;
            }
        }
        else if(fdsReady == 0)
        {
            collectPpmData();
            sendPpmStreams(sock, pStreamBuffer, streamBufferSize, &client);
        }
        else
        {
            break;
        }
    }

    g_clientSock = INVALID_SOCKET;
    closesocket(sock);
    glowReader_free(pReader);
    freeMemory(pStreamBuffer); freeMemory(pRxBuffer); freeMemory(pReader);
}

static void acceptClient(int port)
{
    // Start JSON listener thread beforehand; reuse sample's socket init
    initSockets();
    if (!g_sendCsInitialized) { InitializeCriticalSection(&g_sendCs); g_sendCsInitialized = 1; }
    uintptr_t th = _beginthreadex(NULL, 0, json_listener, NULL, 0, NULL);
    if (th) CloseHandle((HANDLE)th);

    // Implement our own accept loop to ensure we can wrap handleClient
    SOCKET listenSock, clientSock;
    struct sockaddr_in localAddr;

    listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons((unsigned short)port);
    bind(listenSock, (struct sockaddr *)&localAddr, sizeof(localAddr));
    listen(listenSock, 1);

    clientSock = accept(listenSock, NULL, NULL);
    if(clientSock > 0)
    {
        printf_s("client accepted\n");
        handleClient(clientSock);
    }

    closesocket(listenSock);

    shutdownSockets();
    if (g_sendCsInitialized) { DeleteCriticalSection(&g_sendCs); g_sendCsInitialized = 0; }
}

// Entry point mirroring sample, but enforce read-only fields
int main(int argc, char **argv)
{
    ember_init(onThrowError, onFailAssertion, malloc, free);
    initializePpmStreams();
    buildTree(&_root);

    // Force all parameters to read-only
    make_read_only_recursive(&_root);

    int port = 9000;
    if (argc >= 2)
        port = atoi(argv[1]);

    printf_s("[ro_provider] Ember+ provider on %d; JSON on 127.0.0.1:5001\n", port);
    acceptClient(port);

    sampleNode_free(&_root);
    freePpmStreams();
    return 0;
}
