#include "platform/UsbSerial.h"

#include <cstring>

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

void OnRx(uint8_t* buf, uint32_t* len) { g_serial.OnReceive(buf, *len); }

USBD_CDC_HandleTypeDef* Cdc() {
  if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return nullptr;
  return static_cast<USBD_CDC_HandleTypeDef*>(hUsbDeviceFS.pClassData);
}

// One USB transfer at most this long; a length that is a multiple of the 64-byte packet
// ends with a zero-length packet, which the CDC class sends itself.
constexpr uint32_t kMaxTransfer = 4096u;

}  // namespace

UsbSerial& Serial() { return g_serial; }

void UsbSerial::Init() {
  static_assert(sizeof g_tx == kTxSize && sizeof g_rx == kRxSize, "ring sizes");
  Seed().usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
  Seed().usb_handle.SetReceiveCallback(OnRx, daisy::UsbHandle::FS_INTERNAL);
  // libDaisy enables the OTG_FS interrupts at priority 0, the audio DMA's: put them below it
  // (companion-app.md §7.2's bring-up rule), so a USB burst never delays an audio block.
  HAL_NVIC_SetPriority(OTG_FS_EP1_OUT_IRQn, 2, 0);
  HAL_NVIC_SetPriority(OTG_FS_EP1_IN_IRQn, 2, 0);
  HAL_NVIC_SetPriority(OTG_FS_IRQn, 2, 0);
}

bool UsbSerial::Configured() const { return Cdc() != nullptr; }

void UsbSerial::Pump() {
  USBD_CDC_HandleTypeDef* cdc = Cdc();
  if (cdc == nullptr) {
    inFlight_ = 0;  // unplugged or not yet enumerated: the transfer is gone
    return;
  }
  if (inFlight_ != 0) {
    if (cdc->TxState != 0) return;
    txTail_ += inFlight_;
    inFlight_ = 0;
  }
  const uint32_t queued = Queued();
  if (queued == 0) return;
  const uint32_t start = txTail_ & (kTxSize - 1u);
  uint32_t       n     = kTxSize - start;  // contiguous to the end of the ring
  if (n > queued) n = queued;
  if (n > kMaxTransfer) n = kMaxTransfer;
  if (CDC_Transmit_FS(g_tx + start, static_cast<uint16_t>(n)) == USBD_OK) inFlight_ = n;
}

void UsbSerial::Write(const char* data, size_t size, Mode mode) {
  uint32_t lastProgress = daisy::System::GetNow();
  while (size > 0) {
    const uint32_t room = kTxSize - Queued();
    if (room == 0) {
      if (mode == Mode::Drop || daisy::System::GetNow() - lastProgress > 2000u) {
        dropped_ += static_cast<uint32_t>(size);
        return;
      }
      const uint32_t before = txTail_;
      Pump();
      if (txTail_ != before) lastProgress = daisy::System::GetNow();
      continue;
    }
    const uint32_t n = size < room ? static_cast<uint32_t>(size) : room;
    for (uint32_t i = 0; i < n; ++i) g_tx[(txHead_ + i) & (kTxSize - 1u)] = static_cast<uint8_t>(data[i]);
    txHead_ += n;
    data += n;
    size -= n;
    lastProgress = daisy::System::GetNow();
  }
  Pump();
}

void UsbSerial::WriteLine(const std::string& line, Mode mode) {
  // A line is queued whole or not at all when dropping, so the host never sees half a line.
  if (mode == Mode::Drop && line.size() + 1u > kTxSize - Queued()) {
    Pump();
    if (line.size() + 1u > kTxSize - Queued()) {
      dropped_ += static_cast<uint32_t>(line.size() + 1u);
      return;
    }
  }
  Write(line.data(), line.size(), mode);
  Write("\n", 1, mode);
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
