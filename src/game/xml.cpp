// game/xml: XmlSimple, a minimal tag reader / writer over C strings (the tool-side "XSDSchemaSof"
// node files: <Node> blocks of <name>value</name> elements); string search only, no parser.
#include "types.h"
#include "xml.h"
#include <string.h>
#include <stdio.h>

#ifdef TARGET_PC
// The GCC 2.95/MSL libc this was written against declares a single non-const-correct strstr
// overload (always returns char*); libc++ has the standard const-correct pair, so a const char*
// first argument here picks the const-returning overload and the plain `char* p = strstr(...)`
// below no longer converts implicitly. Same behaviour, host-correct overload, macro'd over the
// call sites instead of touching each one.
static inline char* strstr_host(const char* s, const char* n) { return const_cast<char*>(strstr(s, n)); }
#define strstr strstr_host
#endif

// Finds "<tag" in `src`; *out = its position. 0 when absent.
int XmlSimple::GetXmlStart(char** pOut, const char* pIn, const char* pName)
{
    char buf[256];

    strcpy(buf, "<");
    strcat(buf, pName);
    *pOut = strstr(pIn, buf);
    if (*pOut == NULL) {
        return 0;
    }
    return 1;
}

// The next "<tag" after position `src`.
int XmlSimple::GetXmlNext(char** pOut, const char* pIn, const char* pName)
{
    return GetXmlStart(pOut, pIn + 1, pName);
}

// Copies the text between "<tag>" and "</tag>" into `out`. 0 when either is missing.
int XmlSimple::GetXmlElem(char* pOut, const char* pIn, const char* pName)
{
    char start[256];
    char end[256];
    char* p;
    char* q;
    int len;

    strcpy(start, "<");
    strcat(start, pName);
    strcat(start, ">");
    p = strstr(pIn, start);
    if (p == NULL) {
        return 0;
    }
    p += strlen(start);
    strcpy(end, "</");
    strcat(end, pName);
    strcat(end, ">");
    q = strstr(pIn, end);
    if (q == NULL) {
        return 0;
    }
    len = q - p;
    strncpy(pOut, p, len);
    pOut[len] = '\0';
    return 1;
}

// Writes the document opening tag; *size grows by its length.
int XmlSimple::SetXmlStart(int* pOut, char* pIn)
{
    strcpy(pIn, "<XSDSchemaSof xmlns=\"http://tempuri.org/XSDSchemaSof.xsd\">\n");
    *pOut += strlen(pIn);
    return 1;
}

// Writes the document closing tag.
int XmlSimple::SetXmlEnd(int* pOut, char* pIn)
{
    strcpy(pIn, "</XSDSchemaSof>\n");
    *pOut += strlen(pIn);
    return 1;
}

// Writes "<Node>".
int XmlSimple::SetXmlElemStart(int* size, char* buf)
{
    strcpy(buf, "\t<Node xmlns=\"\">\n");
    *size += strlen(buf);
    return 1;
}

// Writes "</Node>".
int XmlSimple::SetXmlElemEnd(int* size, char* buf)
{
    strcpy(buf, "\t</Node>\n");
    *size += strlen(buf);
    return 1;
}

// Writes "<name>value</name>".
int XmlSimple::SetXmlElem(int* pOut, char* pIn, const char* pName, const char* pText)
{
    char tmp[256];

    sprintf(tmp, "\t\t<%s>%s</%s>\n", pName, pText, pName);
    strcpy(pIn, tmp);
    *pOut += strlen(pIn);
    return 1;
}
