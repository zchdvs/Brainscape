#include "platform/UsbSerial.h"

#include <cstring>

#include "JsonLine.h"
#include "platform/SeedHw.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

extern "C" USBD_HandleTypeDef hUsbDeviceFS;  // libDaisy src/hid/usb.cpp

namespace brainscape::fw {

namespace {

// The rings live in AXI SRAM (cached): the OTG_FS core has no DMA, so the CPU copies the
// bytes into the USB FIFO in the interrupt and there is no cache coherency to manage.
__attribute__((section(".bss.brainscape_axi_usb_tx"))) uint8_t g_tx[32u * 1024u];
__attribute__((section(".bss.brainscape_axi_usb_rx"))) uint8_t g_rx[1024u];

UsbSerial g_serial;

// Counted in the USB interrupt: transfers the CDC class finished (its TransmitCplt hook,
// called from USBD_CDC_DataIn once the last packet and any zero-length packet are sent), and
// (re)configurations (the class's Init on SET_CONFIGURATION, its DeInit on a USB reset, an
// unplug or a new configuration), which drop whatever transfer was in flight.
volatile uint32_t g_txDone = 0;
volatile uint32_t g_cdcGen = 0;
int8_t (*g_cdcInit)(void)   = nullptr;
int8_t (*g_cdcDeInit)(void) = nullptr;

int8_t CdcInit(void) {
  g_cdcGen = g_cdcGen + 1u;
  return g_cdcInit();
}
int8_t CdcDeInit(void) {
  g_cdcGen = g_cdcGen + 1u;
  return g_cdcDeInit();
}
int8_t CdcTransmitCplt(uint8_t* /*buf*/, uint32_t* /*len*/, uint8_t /*epnum*/) {
  g_txDone = g_txDone + 1u;
  return USBD_OK;
}

void OnRx(uint8_t* buf, uint32_t* len) { g_serial.OnReceive(buf, *len); }

// One USB transfer at most this long; a length that is a multiple of the 64-byte packet
// ends with a zero-length packet, which the CDC class sends itself.
constexpr uint32_t kMaxTransfer = 4096u;
constexpr uint32_t kMask        = 32u * 1024u - 1u;

}  // namespace

UsbSerial& Serial() { return g_serial; }

void UsbSerial::Init() {
  static_assert(sizeof g_tx == kTxSize && sizeof g_rx == kRxSize, "ring sizes");
  // libDaisy registers its CDC interface table (src/usbd/usbd_cdc_if.c, a mutable global) by
  // address, without a TransmitCplt: wrap its Init and DeInit and add the completion hook
  // before the device starts.
  if (g_cdcInit == nullptr) {
    g_cdcInit                           = USBD_Interface_fops_FS.Init;
    g_cdcDeInit                         = USBD_Interface_fops_FS.DeInit;
    USBD_Interface_fops_FS.Init         = CdcInit;
    USBD_Interface_fops_FS.DeInit       = CdcDeInit;
    USBD_Interface_fops_FS.TransmitCplt = CdcTransmitCplt;
  }
  Seed().usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
  Seed().usb_handle.SetReceiveCallback(OnRx, daisy::UsbHandle::FS_INTERNAL);
  // libDaisy enables the OTG_FS interrupts at priority 0, the audio DMA's: put them below it
  // (companion-app.md §7.2's bring-up rule), so a USB burst never delays an audio block.
  HAL_NVIC_SetPriority(OTG_FS_EP1_OUT_IRQn, 2, 0);
  HAL_NVIC_SetPriority(OTG_FS_EP1_IN_IRQn, 2, 0);
  HAL_NVIC_SetPriority(OTG_FS_IRQn, 2, 0);
}

bool UsbSerial::Configured() const {
  return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && hUsbDeviceFS.pClassData != nullptr;
}

void UsbSerial::DiscardInFlight() {
  // The host got none of the transfer, or some of its packets: drop it, and the rest of the
  // line it ends inside, and say so before the next line (resync_).
  uint32_t n = inFlight_;
  inFlight_  = 0;
  bool lineEnd = false;
  while (n > 0 || (!lineEnd && txTail_ != txHead_)) {
    const uint8_t c = g_tx[txTail_ & kMask];
    ++txTail_;
    ++dropped_;
    if (n > 0) --n;
    lineEnd = c == '\n';
    if (lineEnd) ++droppedLines_;
  }
  resync_ = true;
}

void UsbSerial::Pump() {
  if (inFlight_ != 0) {
    if (g_txDone != doneAtStart_) {
      txTail_ += inFlight_;  // delivered
      inFlight_ = 0;
    } else if (g_cdcGen != genAtStart_) {
      DiscardInFlight();  // a reset or reconfiguration took it
    } else {
      return;  // still in flight, a suspended bus included
    }
  }
  const uint32_t queued = Queued();
  if (queued == 0) return;
  const uint32_t start = txTail_ & kMask;
  uint32_t       n     = kTxSize - start;  // contiguous to the end of the ring
  if (n > queued) n = queued;
  if (n > kMaxTransfer) n = kMaxTransfer;
  // With the USB interrupt held off, so the class cannot be torn down mid-call.
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (Configured() && static_cast<USBD_CDC_HandleTypeDef*>(hUsbDeviceFS.pClassData)->TxState == 0) {
    doneAtStart_ = g_txDone;
    genAtStart_  = g_cdcGen;
    if (CDC_Transmit_FS(g_tx + start, static_cast<uint16_t>(n)) == USBD_OK) inFlight_ = n;
  }
  if (primask == 0u) __enable_irq();
}

void UsbSerial::Copy(const char* data, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) g_tx[(txHead_ + i) & kMask] = static_cast<uint8_t>(data[i]);
  txHead_ += size;
}

void UsbSerial::DropLine(uint32_t bytes) {
  dropped_ += bytes;
  ++droppedLines_;
  resync_ = true;
}

void UsbSerial::WriteLine(const std::string& line, Mode mode) {
  const uint32_t need = static_cast<uint32_t>(line.size()) + 1u;
  if (need > kTxSize / 2u) {  // never fits beside a resync line and a transfer
    DropLine(need);
    return;
  }
  uint32_t lastProgress = daisy::System::GetNow();
  for (;;) {
    // The resync notice, rebuilt each time: its counts include every loss so far. Its
    // leading newline ends whatever partial line a lost transfer left at the host.
    const std::string notice =
        resync_ ? "\n{\"type\":\"resync\",\"droppedBytes\":" + golden::JsonUInt(dropped_) +
                      ",\"droppedLines\":" + golden::JsonUInt(droppedLines_) + "}\n"
                : std::string();
    if (Room() >= need + notice.size()) {
      Copy(notice.data(), static_cast<uint32_t>(notice.size()));
      Copy(line.data(), need - 1u);
      Copy("\n", 1u);
      resync_ = false;
      Pump();
      return;
    }
    const uint32_t before = txTail_;
    Pump();
    if (txTail_ != before) lastProgress = daisy::System::GetNow();
    if (Room() >= need + notice.size()) continue;
    if (mode == Mode::Drop || daisy::System::GetNow() - lastProgress > 2000u) {
      DropLine(need);
      return;
    }
  }
}

bool UsbSerial::Flush(uint32_t timeoutMs) {
  uint32_t lastProgress = daisy::System::GetNow();
  while (Queued() != 0) {
    const uint32_t before = txTail_;
    Pump();
    if (txTail_ != before) lastProgress = daisy::System::GetNow();
    if (daisy::System::GetNow() - lastProgress > timeoutMs) return false;
  }
  return true;
}

void UsbSerial::OnReceive(const uint8_t* data, uint32_t size) {
  uint32_t head = rxHead_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < size; ++i) {
    if (head - rxTail_ >= kRxSize) break;  // full: drop (commands are short)
    g_rx[head & (kRxSize - 1u)] = data[i];
    ++head;
  }
  rxHead_.store(head, std::memory_order_release);
}

bool UsbSerial::ReadLine(std::string* line) {
  const uint32_t head = rxHead_.load(std::memory_order_acquire);
  while (rxTail_ != head) {
    const char c = static_cast<char>(g_rx[rxTail_ & (kRxSize - 1u)]);
    ++rxTail_;
    if (c == '\n' || c == '\r') {
      if (pending_.empty()) continue;  // the other half of a CR LF, or a blank line
      *line = pending_;
      pending_.clear();
      return true;
    }
    if (pending_.size() < 256u) pending_ += c;
  }
  return false;
}

}  // namespace brainscape::fw
