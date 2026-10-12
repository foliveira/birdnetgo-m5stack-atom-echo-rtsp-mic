#pragma once
// Framing of RTSP requests during negotiation. Plain C++, tested on the host by
// test/host/test_rtsp.cpp.
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Copies the value of header `name` (case-insensitive, as in HTTP) from the header block
// req[0..len) into out without surrounding blanks, truncated to cap - 1 characters. The first
// line (the request line) is never a header. Returns false if the header is absent.
inline bool rtspHeaderValue(const char* req, size_t len, const char* name, char* out, size_t cap) {
    const size_t n = strlen(name);
    const char* end = req + len;
    const char* nl = (const char*)memchr(req, '\n', len);
    while (nl && nl + 1 < end) {
        const char* line = nl + 1;
        nl = (const char*)memchr(line, '\n', end - line);
        const char* lineEnd = nl ? nl : end;
        if ((size_t)(lineEnd - line) <= n || strncasecmp(line, name, n) != 0 || line[n] != ':') continue;
        const char* v = line + n + 1;
        const char* ve = lineEnd;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vlen = (size_t)(ve - v);
        if (cap) {
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
        }
        return true;
    }
    return false;
}

// Looks at the first request in buf[0..len): its header block ends with an empty line and a
// Content-Length body may follow. Returns the request's total length and sets *headerLen to the
// length of the header block without the empty line; 0 if more bytes are needed; -1 if the
// request cannot fit in cap bytes or its Content-Length is malformed (discard the buffer).
inline int rtspRequestLength(const char* buf, int len, int cap, int* headerLen) {
    int h = -1;
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { h = i; break; }
    }
    if (h < 0) return len >= cap ? -1 : 0;
    long body = 0;
    char v[12];
    if (rtspHeaderValue(buf, (size_t)h, "Content-Length", v, sizeof(v))) {
        char* e = nullptr;
        body = strtol(v, &e, 10);
        if (e == v || *e != '\0' || body < 0 || body > cap) return -1;
    }
    const long total = h + 4 + body;
    if (total > cap) return -1;
    if (headerLen) *headerLen = h;
    return len >= total ? (int)total : 0;
}
