#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace brainscape::fw {

// Line-oriented USB serial over libDaisy's CDC device on the Seed's micro-USB port (OTG_FS,
// "FS_INTERNAL"), with real buffering: libDaisy's CDC_Transmit_FS sends a caller-owned buffer
// and refuses while a transfer is in flight, so output is queued in a 32 KiB ring (AXI SRAM)
// and handed to the CDC class one contiguous chunk at a time, from the main loop only. Input
// arrives in the USB interrupt into a small ring, read as lines.
//
// libDaisy's CDC class code is ST's SLA0044 middleware (companion-app.md §7.2): fine for
// these undistributed bring-up images, and the reason the product firmware moves to
// TinyUSB.
class UsbSerial {
 public:
  enum class Mode : uint8_t {
    Block,  // wait for room, pumping the USB, up to 2 s without progress, then drop
    Drop,   // queue what fits now, drop the rest (counted): for code that must not stall
  };

  void Init();
  // The host has enumerated and configured the CDC interface.
  bool Configured() const;

  void Write(const char* data, size_t size, Mode mode);
  void WriteLine(const std::string& line, Mode mode);  // appends "\n"

  // Starts the next USB transfer when the previous one finished. Call it often from the
  // main loop; Write(Block) and Flush call it themselves.
  void Pump();
  // Pumps until everything queued has been sent, or `timeoutMs` passes without progress.
  // True when the ring is empty.
  bool Flush(uint32_t timeoutMs);

  // A complete input line, without its line ending.
  bool ReadLine(std::string* line);

  uint32_t DroppedBytes() const { return dropped_; }

  // Called from the CDC receive interrupt.
  void OnReceive(const uint8_t* data, uint32_t size);

 private:
  static constexpr uint32_t kTxSize = 32u * 1024u;  // powers of two: indices are masked
  static constexpr uint32_t kRxSize = 1024u;

  uint32_t Queued() const { return txHead_ - txTail_; }

  uint32_t          txHead_     = 0;  // free-running; main loop only
  uint32_t          txTail_     = 0;
  uint32_t          inFlight_   = 0;  // bytes of the transfer the CDC class holds
  uint32_t          dropped_    = 0;
  std::atomic<uint32_t> rxHead_{0};   // written by the USB interrupt (release)
  uint32_t          rxTail_     = 0;  // main loop
  std::string       pending_;         // a partial input line
};

UsbSerial& Serial();

}  // namespace brainscape::fw
