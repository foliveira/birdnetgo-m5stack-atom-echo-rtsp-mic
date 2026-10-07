// Host tests for src/PromOut.h. Build and run with test/host/run.sh.
// With a file argument it also writes a sample exposition there, e.g. to check it with promtool:
//   podman run --rm -i --entrypoint promtool docker.io/prom/prometheus check metrics < sample.txt
#include "PromOut.h"
#include "check.h"

#include <string>

static std::string sample(char* buf, size_t cap) {
    PromOut p(buf, cap);
    static const char* const labels[] = {"version", "3.1.0", "name", "garden \"mic\"\\1", nullptr};
    p.info("rtspmic_info", "Firmware version and device name.", labels);
    p.gauge("rtspmic_wifi_rssi_dbm", "Received signal strength of the access point.", -63);
    p.gauge("rtspmic_temperature_celsius", "Chip temperature (uncalibrated).", 51.25, 1);
    p.gauge("rtspmic_audio_peak_ratio", "Peak level, 1 = full scale.\nSecond line.", NAN, 3);
    p.counter("rtspmic_frames_sent_total", "RTP frames sent since boot.", 4000000000u);
    return buf;
}

int main(int argc, char** argv) {
    char buf[2048];
    const std::string s = sample(buf, sizeof(buf));
    CHECK(s.find("# HELP rtspmic_info Firmware version and device name.\n# TYPE rtspmic_info gauge\n"
                 "rtspmic_info{version=\"3.1.0\",name=\"garden \\\"mic\\\"\\\\1\"} 1\n") == 0);
    CHECK(s.find("\nrtspmic_wifi_rssi_dbm -63\n") != std::string::npos);
    CHECK(s.find("\nrtspmic_temperature_celsius 51.2\n") != std::string::npos ||
          s.find("\nrtspmic_temperature_celsius 51.3\n") != std::string::npos);
    CHECK(s.find("# HELP rtspmic_audio_peak_ratio Peak level, 1 = full scale.\\nSecond line.\n") != std::string::npos);
    CHECK(s.find("\nrtspmic_audio_peak_ratio NaN\n") != std::string::npos);
    CHECK(s.find("# TYPE rtspmic_frames_sent_total counter\nrtspmic_frames_sent_total 4000000000\n") != std::string::npos);

    char small[64];
    PromOut p(small, sizeof(small));
    p.gauge("rtspmic_a_rather_long_metric_name", "Help text that does not fit.", 1);
    CHECK(p.overflow());
    CHECK_EQ(p.length(), sizeof(small) - 1);

    if (argc > 1) {
        FILE* f = fopen(argv[1], "w");
        if (f) { fputs(s.c_str(), f); fclose(f); }
    }
    return checkSummary("test_prom");
}
