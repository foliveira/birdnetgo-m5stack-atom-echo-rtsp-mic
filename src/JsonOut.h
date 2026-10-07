#pragma once
// Small JSON writer into a fixed buffer (no heap). Inserts commas, escapes strings and writes
// non-finite numbers as null, so a handler cannot produce malformed JSON. Plain C++, tested on
// the host by test/host/test_json.cpp.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

class JsonOut {
public:
    JsonOut(char* buf, size_t cap) : buf_(buf), cap_(cap) { if (cap_) buf_[0] = '\0'; }

    JsonOut& open(const char* key = nullptr) { item(key); put('{'); first_ = true; return *this; }
    JsonOut& close() { put('}'); first_ = false; return *this; }

    JsonOut& i(const char* key, long v) { item(key); fmt("%ld", v); return *this; }
    JsonOut& u(const char* key, unsigned long v) { item(key); fmt("%lu", v); return *this; }
    JsonOut& b(const char* key, bool v) { item(key); raw(v ? "true" : "false"); return *this; }
    JsonOut& null(const char* key) { item(key); raw("null"); return *this; }
    JsonOut& f(const char* key, double v, int decimals) {
        item(key);
        if (isfinite(v)) fmt("%.*f", decimals, v); else raw("null");
        return *this;
    }
    JsonOut& s(const char* key, const char* v) {
        item(key);
        if (!v) { raw("null"); return *this; }
        put('"');
        for (const char* p = v; *p; p++) {
            const unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { put('\\'); put((char)c); }
            else if (c == '\n') raw("\\n");
            else if (c < 0x20) fmt("\\u%04x", c);
            else put((char)c);
        }
        put('"');
        return *this;
    }

    const char* c_str() const { return buf_; }
    size_t length() const { return len_; }
    bool overflow() const { return overflow_; }

private:
    void item(const char* key) {
        if (!first_) put(',');
        first_ = false;
        if (key) { put('"'); raw(key); raw("\":"); }
    }
    void put(char c) {
        if (len_ + 1 >= cap_) { overflow_ = true; return; }
        buf_[len_++] = c;
        buf_[len_] = '\0';
    }
    void raw(const char* t) { while (*t) put(*t++); }
    template <typename... A> void fmt(const char* f, A... a) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), f, a...);
        raw(tmp);
    }

    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool first_ = true;
    bool overflow_ = false;
};
