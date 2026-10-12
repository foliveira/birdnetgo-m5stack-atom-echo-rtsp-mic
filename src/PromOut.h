#pragma once
// Prometheus text exposition format (0.0.4) into a fixed buffer (no heap). Each call writes one
// metric family with a single sample: its HELP and TYPE lines, then the value. Plain C++, tested
// on the host by test/host/test_prom.cpp.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class PromOut {
public:
    PromOut(char* buf, size_t cap) : buf_(buf), cap_(cap) { if (cap_) buf_[0] = '\0'; }

    PromOut& gauge(const char* name, const char* help, double v, int decimals = 0) {
        head(name, help, "gauge");
        raw(name);
        value(v, decimals);
        return *this;
    }
    // name should end in _total.
    PromOut& counter(const char* name, const char* help, uint32_t v) {
        head(name, help, "counter");
        raw(name);
        fmt(" %lu\n", (unsigned long)v);
        return *this;
    }
    // A gauge of 1 whose labels carry text (version, name). labels: key, value, key, value, ..., nullptr.
    PromOut& info(const char* name, const char* help, const char* const* labels) {
        head(name, help, "gauge");
        raw(name);
        put('{');
        for (const char* const* l = labels; l[0] && l[1]; l += 2) {
            if (l != labels) put(',');
            raw(l[0]);
            raw("=\"");
            for (const char* p = l[1]; *p; p++) {
                if (*p == '\\' || *p == '"') { put('\\'); put(*p); }
                else if (*p == '\n') raw("\\n");
                else put(*p);
            }
            put('"');
        }
        raw("} 1\n");
        return *this;
    }

    const char* c_str() const { return buf_; }
    size_t length() const { return len_; }
    bool overflow() const { return overflow_; }

private:
    void head(const char* name, const char* help, const char* type) {
        raw("# HELP "); raw(name); put(' ');
        for (const char* p = help; *p; p++) {
            if (*p == '\\') raw("\\\\");
            else if (*p == '\n') raw("\\n");
            else put(*p);
        }
        raw("\n# TYPE "); raw(name); put(' '); raw(type); put('\n');
    }
    void value(double v, int decimals) {
        if (isnan(v)) raw(" NaN\n");
        else if (isinf(v)) raw(v > 0 ? " +Inf\n" : " -Inf\n");
        else fmt(" %.*f\n", decimals, v);
    }
    void put(char c) {
        if (len_ + 1 >= cap_) { overflow_ = true; return; }
        buf_[len_++] = c;
        buf_[len_] = '\0';
    }
    void raw(const char* t) { while (*t) put(*t++); }
    template <typename... A> void fmt(const char* f, A... a) {
        char tmp[40];
        snprintf(tmp, sizeof(tmp), f, a...);
        raw(tmp);
    }

    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflow_ = false;
};
