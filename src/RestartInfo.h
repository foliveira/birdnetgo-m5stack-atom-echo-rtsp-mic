#pragma once
// Why the device last restarted: the chip's reset reason, the note the firmware leaves before a
// restart of its own (RTC memory survives a software restart, not a power cycle), and after a
// crash the summary of the core dump the panic handler wrote to flash.

// Call once at boot, before anything can restart the device.
void restartInfoInit();

// Short description of the last restart, e.g. "power on", "Web UI reboot", "crash in task
// AudioCapture at 0x400d2f1c".
const char* restartReason();

// After a crash: task, PC and backtrace on one line, for the log and for decoding with
// xtensa-esp32-elf-addr2line against this build's firmware.elf. Empty otherwise.
const char* restartCrashDetails();

// The bootloader went back to this firmware because the update installed before this boot
// restarted before confirming itself: say so in front of the reason.
void restartInfoMarkRollback();

// Restart now and report `why` as the reason on the next boot.
[[noreturn]] void restartWithNote(const char* why);
