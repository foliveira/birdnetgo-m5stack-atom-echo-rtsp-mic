// Host tests for src/RtspRequest.h. Build and run with test/host/run.sh.
#include "RtspRequest.h"
#include "check.h"

#include <string>

static int frame(const std::string& s, int cap = 1023, int* headerLen = nullptr) {
    int h = -1;
    const int r = rtspRequestLength(s.data(), (int)s.size(), cap, &h);
    if (headerLen) *headerLen = h;
    return r;
}

static std::string header(const std::string& req, const char* name, size_t cap = 64) {
    char out[64] = "?";
    if (!rtspHeaderValue(req.data(), req.size(), name, out, cap)) return "<absent>";
    return out;
}

static void testHeaders() {
    const std::string req = "SETUP rtsp://x/audio/track1 RTSP/1.0\r\n"
                            "CSeq: 3\r\n"
                            "transport:RTP/AVP/TCP;unicast;interleaved=2-3  \r\n"
                            "User-Agent: Lavf60.16.100";
    CHECK(header(req, "CSeq") == "3");
    CHECK(header(req, "cseq") == "3");                                   // names are case-insensitive
    CHECK(header(req, "Transport") == "RTP/AVP/TCP;unicast;interleaved=2-3");   // no blank after ':', trailing blanks cut
    CHECK(header(req, "User-Agent") == "Lavf60.16.100");                 // last line without CRLF
    CHECK(header(req, "Session") == "<absent>");
    CHECK(header(req, "CSe") == "<absent>");                             // a prefix is not a match
    CHECK(header(req, "SETUP rtsp") == "<absent>");                      // the request line is not a header
    CHECK(header(req, "Transport", 8) == "RTP/AVP");                     // truncated to the buffer
}

static void testFraming() {
    const std::string options = "OPTIONS rtsp://x/audio RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    int h = 0;
    CHECK_EQ(frame(options, 1023, &h), (int)options.size());
    CHECK_EQ(h, (int)options.size() - 4);

    // Incomplete header: wait.
    CHECK_EQ(frame(options.substr(0, options.size() - 2)), 0);

    // Two pipelined requests: only the first is framed.
    CHECK_EQ(frame(options + "DESCRIBE rtsp://x/audio RTSP/1.0\r\nCSeq: 2\r\n\r\n"), (int)options.size());

    // A body is part of the request; incomplete until all of it is there.
    const std::string withBody = "SET_PARAMETER rtsp://x/audio RTSP/1.0\r\nCSeq: 5\r\ncontent-length: 10\r\n\r\n";
    CHECK_EQ(frame(withBody + "param: 1\r\n"), (int)withBody.size() + 10);
    CHECK_EQ(frame(withBody + "param"), 0);
    CHECK_EQ(frame(withBody + "param: 1\r\nOPTIONS"), (int)withBody.size() + 10);

    // A Content-Length line inside the body is not a header of this request.
    const std::string bodyLooksLikeHeader = "GET_PARAMETER rtsp://x RTSP/1.0\r\nCSeq: 6\r\nContent-Length: 20\r\n\r\n";
    CHECK_EQ(frame(bodyLooksLikeHeader + "Content-Length: 999\n"), (int)bodyLooksLikeHeader.size() + 20);
}

static void testOversizeAndMalformed() {
    // No end of header and the buffer is full: give up.
    CHECK_EQ(frame(std::string(1023, 'A'), 1023), -1);
    CHECK_EQ(frame(std::string(500, 'A'), 1023), 0);
    // A body that cannot fit.
    CHECK_EQ(frame("ANNOUNCE x RTSP/1.0\r\nContent-Length: 5000\r\n\r\n"), -1);
    // Malformed lengths.
    CHECK_EQ(frame("SET_PARAMETER x RTSP/1.0\r\nContent-Length: -4\r\n\r\n"), -1);
    CHECK_EQ(frame("SET_PARAMETER x RTSP/1.0\r\nContent-Length: abc\r\n\r\n"), -1);
    CHECK_EQ(frame("SET_PARAMETER x RTSP/1.0\r\nContent-Length: 4x\r\n\r\n"), -1);
    CHECK_EQ(frame("SET_PARAMETER x RTSP/1.0\r\nContent-Length:\r\n\r\n"), -1);
    // Exactly fitting.
    const std::string fits = "SET_PARAMETER x RTSP/1.0\r\nContent-Length: 3\r\n\r\n";
    CHECK_EQ(frame(fits + "abc", (int)fits.size() + 3), (int)fits.size() + 3);
    CHECK_EQ(frame(fits + "abc", (int)fits.size() + 2), -1);
}

int main() {
    testHeaders();
    testFraming();
    testOversizeAndMalformed();
    return checkSummary("test_rtsp");
}
