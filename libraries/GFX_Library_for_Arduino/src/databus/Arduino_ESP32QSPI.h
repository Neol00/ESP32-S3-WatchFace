#pragma once

#include "Arduino_DataBus.h"

#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
#include <driver/spi_master.h>

#ifndef ESP32QSPI_MAX_PIXELS_AT_ONCE
#define ESP32QSPI_MAX_PIXELS_AT_ONCE 1024
#endif
#ifndef ESP32QSPI_FREQUENCY
#define ESP32QSPI_FREQUENCY 40000000
#endif
#ifndef ESP32QSPI_SPI_MODE
#define ESP32QSPI_SPI_MODE SPI_MODE0
#endif
#ifndef ESP32QSPI_SPI_HOST
#define ESP32QSPI_SPI_HOST SPI2_HOST
#endif
#ifndef ESP32QSPI_DMA_CHANNEL
#define ESP32QSPI_DMA_CHANNEL SPI_DMA_CH_AUTO
#endif

class Arduino_ESP32QSPI : public Arduino_DataBus
{
public:
  Arduino_ESP32QSPI(
      int8_t cs, int8_t sck, int8_t mosi, int8_t miso, int8_t quadwp, int8_t quadhd, bool is_shared_interface = false); // Constructor

  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t dataMode = GFX_NOT_DEFINED) override;
  void beginWrite() override;
  void endWrite() override;
  void writeCommand(uint8_t) override;
  void writeCommand16(uint16_t) override;
  void writeCommandBytes(uint8_t *data, uint32_t len) override;
  void write(uint8_t) override;
  void write16(uint16_t) override;

  void writeC8D8(uint8_t c, uint8_t d) override;
  void writeC8D16(uint8_t c, uint16_t d) override;
  void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) override;
  void writeC8D16D16Split(uint8_t c, uint16_t d1, uint16_t d2) override;

  void writeC8Bytes(uint8_t c, uint8_t *data, uint32_t len);

  void writeRepeat(uint16_t p, uint32_t len) override;
  void writePixels(uint16_t *data, uint32_t len) override;
  void write16bitBeRGBBitmapR1(uint16_t *bitmap, int16_t w, int16_t h) override;

  void batchOperation(const uint8_t *operations, size_t len) override;
  void writeBytes(uint8_t *data, uint32_t len) override;

  void writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len) override;
  void writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len) override;
  void writeYCbCrPixels(uint8_t *yData, uint8_t *cbData, uint8_t *crData, uint16_t w, uint16_t h) override;

  // LOCAL PATCH (ESP32-S3-WatchFace): ASYNC whole-area pixel write. `data` must be
  // DMA-capable (internal SRAM) and ALREADY in panel byte order (big-endian RGB565).
  // Queues the whole area as a few large DMA transactions and returns immediately;
  // `done_cb(done_arg)` fires from the SPI transfer-done ISR after the LAST segment
  // (keep it trivial — it's interrupt context). Returns false (nothing sent) if the
  // area needs more than ASYNC_MAX_SEG segments — caller should fall back to the
  // sync writePixels() path. Any sync bus call transparently waits out an in-flight
  // async write first (drain hook in CS_LOW), so commands can't interleave a tile.
  bool writePixelsBeAsync(const uint8_t *data, uint32_t len, void (*done_cb)(void *), void *done_arg);
  void waitAsync(); // block until no async write is in flight (reaps queued results)

protected:
private:
  GFX_INLINE void CS_HIGH(void);
  GFX_INLINE void CS_LOW(void);
  GFX_INLINE void POLL_START();
  GFX_INLINE void POLL_END();

  // LOCAL PATCH (ESP32-S3-WatchFace): async write plumbing. The S3's SPI peripheral
  // caps one transaction at 2^18 bits (32 KB), so a tile is split into up to
  // ASYNC_MAX_SEG queued segments sharing ONE CS bracket: the pre/post transaction
  // callbacks drive CS (and fire done_cb) only on the segments flagged first/last.
  // Sync paths leave trans.user NULL so the callbacks ignore them.
  struct AsyncSeg
  {
    Arduino_ESP32QSPI *bus;
    bool first;
    bool last;
  };
  static void _async_pre_cb(spi_transaction_t *t);
  static void _async_post_cb(spi_transaction_t *t);
  static const int ASYNC_MAX_SEG = 4;
  spi_transaction_ext_t _async_tran[ASYNC_MAX_SEG];
  AsyncSeg _async_seg[ASYNC_MAX_SEG];
  volatile uint8_t _async_pending = 0; // queued-but-unreaped async transactions
  void (*_async_done_cb)(void *) = nullptr;
  void *_async_done_arg = nullptr;

  int8_t _cs, _sck, _mosi, _miso, _quadwp, _quadhd;
  bool _is_shared_interface;

  PORTreg_t _csPortSet; ///< PORT register for chip select SET
  PORTreg_t _csPortClr; ///< PORT register for chip select CLEAR
  uint32_t _csPinMask;  ///< Bitmask for chip select

  spi_device_handle_t _handle;
  spi_transaction_ext_t _spi_tran_ext;
  spi_transaction_t *_spi_tran;

  // LOCAL PATCH (ESP32-S3-WatchFace): a SECOND transaction struct so writePixels()
  // can pipeline — keep transaction A in flight (DMA) while filling buffer B for
  // transaction B. Each queued transaction's struct must stay alive until its
  // get_trans_result returns, so the two chunks-in-flight need their own structs.
  spi_transaction_ext_t _spi_tran_ext2;

  union
  {
    uint8_t *_buffer;
    uint16_t *_buffer16;
    uint32_t *_buffer32;
  };
  union
  {
    uint8_t *_2nd_buffer;
    uint16_t *_2nd_buffer16;
    uint32_t *_2nd_buffer32;
  };
};

#endif // #if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
