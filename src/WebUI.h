#pragma once
// Web UI and JSON API on port 80 (page in web/index.html). Everything runs on the loop task.
void webui_begin();
void webui_handleClient();

// Append a line to the log the Web UI shows (the last 48 are kept).
void webui_pushLog(const char* line);
