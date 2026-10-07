// Host tests for src/JsonOut.h. Build and run with test/host/run.sh.
#include "JsonOut.h"
#include "check.h"

#include <string.h>

static void testShapeAndTypes() {
    char buf[256];
    JsonOut j(buf, sizeof(buf));
    j.open();
    j.s("fw", "3.1.0").i("rssi", -61).u("sent", 4000000000UL).b("on", true);
    j.open("audio").u("rate", 32000).f("gain", 3.0, 2).close();
    j.f("temp", NAN, 1).f("inf", INFINITY, 1).null("client").s("none", nullptr);
    j.close();
    CHECK(!j.overflow());
    CHECK(strcmp(buf, "{\"fw\":\"3.1.0\",\"rssi\":-61,\"sent\":4000000000,\"on\":true,"
                      "\"audio\":{\"rate\":32000,\"gain\":3.00},\"temp\":null,\"inf\":null,"
                      "\"client\":null,\"none\":null}") == 0);
    CHECK_EQ(j.length(), strlen(buf));
}

static void testEscaping() {
    char buf[128];
    JsonOut j(buf, sizeof(buf));
    j.open().s("ssid", "a\"b\\c\nd\te\x01").close();
    CHECK(strcmp(buf, "{\"ssid\":\"a\\\"b\\\\c\\nd\\u0009e\\u0001\"}") == 0);
    // UTF-8 passes through untouched.
    JsonOut k(buf, sizeof(buf));
    k.open().s("n", "Caf\xc3\xa9").close();
    CHECK(strcmp(buf, "{\"n\":\"Caf\xc3\xa9\"}") == 0);
}

static void testNestedSiblingsGetCommas() {
    char buf[128];
    JsonOut j(buf, sizeof(buf));
    j.open().open("a").i("x", 1).close().open("b").close().i("c", 2).close();
    CHECK(strcmp(buf, "{\"a\":{\"x\":1},\"b\":{},\"c\":2}") == 0);
}

static void testOverflowIsReportedAndTerminated() {
    char buf[16];
    JsonOut j(buf, sizeof(buf));
    j.open().s("key", "a long value that does not fit").close();
    CHECK(j.overflow());
    CHECK(strlen(buf) == sizeof(buf) - 1);
    CHECK_EQ(j.length(), sizeof(buf) - 1);
}

int main() {
    testShapeAndTypes();
    testEscaping();
    testNestedSiblingsGetCommas();
    testOverflowIsReportedAndTerminated();
    return checkSummary("test_json");
}
