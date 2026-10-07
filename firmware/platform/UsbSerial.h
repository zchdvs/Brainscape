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
// Whole lines only: a line is queued completely or dropped completely, so the host never
// receives part of one, and a transfer the host never took (a USB reset, an unplug or a
// reconfiguration while it was in flight) is discarded up to the end of the line it cut,
// never sent twice. Completion comes from the CDC class's TransmitCplt callback, and the
// class's Init/DeInit mark every (re)configuration, so a suspend, which keeps the transfer,
// is told apart from a reset, which loses it. After any loss the next line is preceded by
//   {"type":"resync","droppedBytes":N,"droppedLines":M}
// on a line of its own (with a newline before it, ending whatever partial text the host
// holds), so the host tools can tell transport loss from a failed check.
//
// libDaisy's CDC class code is ST's SLA0044 middleware (companion-app.md §7.2): fine for
// these undistributed bring-up images, and the reason the product firmware moves to
// TinyUSB.
class UsbSerial {
 public:
  enum class Mode : uint8_t {
    Block,  // wait for room, pumping the USB, up to 2 s without progress, then drop the line
    Drop,   // queue the line if it fits now, else drop it (counted): for code that must not stall
  };

  void Init();
  // The host has enumerated and configured the CDC interface.
  bool Configured() const;

  // Queues `line` and a newline, whole or not at all.
  void WriteLine(const std::string& line, Mode mode);

  // Starts the next USB transfer when the previous one finished. Call it often from the
  // main loop; WriteLine(Block) and Flush call it themselves.
  void Pump();
  // Pumps until everything queued has been sent, or `timeoutMs` passes without progress.
  // True when the ring is empty.
  bool Flush(uint32_t timeoutMs);

  // A complete input line, without its line ending.
  bool ReadLine(std::string* line);

  uint32_t DroppedBytes() const { return dropped_; }
  uint32_t DroppedLines() const { return droppedLines_; }

  // Called from the CDC receive interrupt.
  void OnReceive(const uint8_t* data, uint32_t size);

 private:
  static constexpr uint32_t kTxSize = 32u * 1024u;  // powers of two: indices are masked
  static constexpr uint32_t kRxSize = 1024u;

  uint32_t Queued() const { return txHead_ - txTail_; }
  uint32_t Room() const { return kTxSize - Queued(); }
  void     Copy(const char* data, uint32_t size);
  void     DropLine(uint32_t bytes);
  void     DiscardInFlight();

  uint32_t          txHead_       = 0;  // free-running; main loop only
  uint32_t          txTail_       = 0;
  uint32_t          inFlight_     = 0;  // bytes of the transfer the CDC class holds
  uint32_t          doneAtStart_  = 0;  // completion count when that transfer started
  uint32_t          genAtStart_   = 0;  // configuration count when it started
  uint32_t          dropped_      = 0;
  uint32_t          droppedLines_ = 0;
  bool              resync_       = false;  // a line was lost since the last one queued
  std::atomic<uint32_t> rxHead_{0};         // written by the USB interrupt (release)
  uint32_t          rxTail_       = 0;      // main loop
  std::string       pending_;               // a partial input line
};

UsbSerial& Serial();

}  // namespace brainscape::fw
