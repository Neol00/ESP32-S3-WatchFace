#include "Arduino_ESP32QSPI.h"

#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)

Arduino_ESP32QSPI::Arduino_ESP32QSPI(
    int8_t cs, int8_t sck, int8_t mosi, int8_t miso, int8_t quadwp, int8_t quadhd, bool is_shared_interface /* = false */)
    : _cs(cs), _sck(sck), _mosi(mosi), _miso(miso), _quadwp(quadwp), _quadhd(quadhd), _is_shared_interface(is_shared_interface)
{
}

bool Arduino_ESP32QSPI::begin(int32_t speed, int8_t dataMode)
{
  // set SPI parameters
  _speed = (speed <= GFX_NOT_DEFINED) ? ESP32QSPI_FREQUENCY : speed;
  _dataMode = (dataMode == GFX_NOT_DEFINED) ? ESP32QSPI_SPI_MODE : dataMode;

  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH); // disable chip select
#if (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
  if (_cs >= 32)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT1_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT1_W1TC_REG;
  }
  else if (_cs != GFX_NOT_DEFINED)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#else
  if (_cs != GFX_NOT_DEFINED)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#endif

  if (speed != GFX_SKIP_DATABUS_UNDERLAYING_BEGIN)
  {
    spi_bus_config_t buscfg = {
        .mosi_io_num = _mosi,
        .miso_io_num = _miso,
        .sclk_io_num = _sck,
        .quadwp_io_num = _quadwp,
        .quadhd_io_num = _quadhd,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        // LOCAL PATCH (ESP32-S3-WatchFace): was (MAX_PIXELS_AT_ONCE*16)+8. The async
        // tile write queues 32 KB segments (SPI hw max is 2^18 bits/transaction), so
        // the driver must size its DMA descriptors for that.
        .max_transfer_sz = 32768 + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
#if (!defined(ESP_ARDUINO_VERSION_MAJOR)) || (ESP_ARDUINO_VERSION_MAJOR < 3)
    // skip this
#else
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
#endif
        .intr_flags = 0};
    esp_err_t ret = spi_bus_initialize(ESP32QSPI_SPI_HOST, &buscfg, ESP32QSPI_DMA_CHANNEL);
    if (ret != ESP_OK)
    {
      ESP_ERROR_CHECK(ret);
      return false;
    }
  }

  spi_device_interface_config_t devcfg = {
      .command_bits = 8,
      .address_bits = 24,
      .dummy_bits = 0,
      .mode = (uint8_t)_dataMode,
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
      .clock_source = SPI_CLK_SRC_DEFAULT,
#endif
      .duty_cycle_pos = 0,
      .cs_ena_pretrans = 0,
      .cs_ena_posttrans = 0,
      .clock_speed_hz = _speed,
      .input_delay_ns = 0,
      .spics_io_num = -1, // avoid use system CS control
      .flags = SPI_DEVICE_HALFDUPLEX,
      // LOCAL PATCH (ESP32-S3-WatchFace): 1 -> 2 so writePixels() can keep one DMA
      // transaction in flight while the CPU converts the next chunk (pipelined flush);
      // -> 6 so writePixelsBeAsync() can queue up to ASYNC_MAX_SEG tile segments.
      .queue_size = 6,
      // LOCAL PATCH (ESP32-S3-WatchFace): CS + completion for the async tile write.
      // No-ops for every transaction whose .user is NULL (all the sync paths).
      .pre_cb = Arduino_ESP32QSPI::_async_pre_cb,
      .post_cb = Arduino_ESP32QSPI::_async_post_cb};
  esp_err_t ret = spi_bus_add_device(ESP32QSPI_SPI_HOST, &devcfg, &_handle);
  if (ret != ESP_OK)
  {
    ESP_ERROR_CHECK(ret);
    return false;
  }

  // LOCAL PATCH (ESP32-S3-WatchFace): do NOT permanently acquire the bus.
  // spi_device_acquire_bus() forbids the queued/interrupt API (spi_device_queue_trans)
  // for its whole duration — only polling transactions are allowed while the bus is
  // held. Our pipelined writePixels() uses the queued API, so the permanent acquire
  // would break it. The CO5300 is the ONLY device on SPI2_HOST (touch/RTC/IMU/PMU are
  // all on I2C), so there's nothing to arbitrate against — the acquire was a no-cost
  // optimization we can simply drop. Polling transactions still work fine without it.
  // (The original: `if (!_is_shared_interface) spi_device_acquire_bus(_handle, portMAX_DELAY);`)

  memset(&_spi_tran_ext, 0, sizeof(_spi_tran_ext));
  memset(&_spi_tran_ext2, 0, sizeof(_spi_tran_ext2));
  _spi_tran = (spi_transaction_t *)&_spi_tran_ext;

  _buffer = (uint8_t *)heap_caps_aligned_alloc(16, ESP32QSPI_MAX_PIXELS_AT_ONCE * 2, MALLOC_CAP_DMA);
  if (!_buffer)
  {
    return false;
  }
  _2nd_buffer = (uint8_t *)heap_caps_aligned_alloc(16, ESP32QSPI_MAX_PIXELS_AT_ONCE * 2, MALLOC_CAP_DMA);
  if (!_2nd_buffer)
  {
    return false;
  }

  return true;
}

void Arduino_ESP32QSPI::beginWrite()
{
  if (_is_shared_interface)
  {
    spi_device_acquire_bus(_handle, portMAX_DELAY);
  }
}

void Arduino_ESP32QSPI::endWrite()
{
  if (_is_shared_interface)
  {
    spi_device_release_bus(_handle);
  }
}

void Arduino_ESP32QSPI::writeCommand(uint8_t c)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_buffer = NULL;
  _spi_tran_ext.base.length = 0;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeCommand16(uint16_t c)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = c;
  _spi_tran_ext.base.tx_buffer = NULL;
  _spi_tran_ext.base.length = 0;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeCommandBytes(uint8_t *data, uint32_t len)
{
  CS_LOW();
  uint32_t l;
  while (len)
  {
    l = (len >= (ESP32QSPI_MAX_PIXELS_AT_ONCE << 1)) ? (ESP32QSPI_MAX_PIXELS_AT_ONCE << 1) : len;

    _spi_tran_ext.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    _spi_tran_ext.base.tx_buffer = data;
    _spi_tran_ext.base.length = l << 3;

    POLL_START();
    POLL_END();

    len -= l;
    data += l;
  }
  CS_HIGH();
}

void Arduino_ESP32QSPI::write(uint8_t d)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MODE_QIO;
  _spi_tran_ext.base.cmd = 0x32;
  _spi_tran_ext.base.addr = 0x003C00;
  _spi_tran_ext.base.tx_data[0] = d;
  _spi_tran_ext.base.length = 8;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::write16(uint16_t d)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MODE_QIO;
  _spi_tran_ext.base.cmd = 0x32;
  _spi_tran_ext.base.addr = 0x003C00;
  _spi_tran_ext.base.tx_data[0] = d >> 8;
  _spi_tran_ext.base.tx_data[1] = d;
  _spi_tran_ext.base.length = 16;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeC8D8(uint8_t c, uint8_t d)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_data[0] = d;
  _spi_tran_ext.base.length = 8;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeC8D16(uint8_t c, uint16_t d)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_data[0] = d >> 8;
  _spi_tran_ext.base.tx_data[1] = d;
  _spi_tran_ext.base.length = 16;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_data[0] = d1 >> 8;
  _spi_tran_ext.base.tx_data[1] = d1;
  _spi_tran_ext.base.tx_data[2] = d2 >> 8;
  _spi_tran_ext.base.tx_data[3] = d2;
  _spi_tran_ext.base.length = 32;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeC8D16D16Split(uint8_t c, uint16_t d1, uint16_t d2)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_data[0] = d1 >> 8;
  _spi_tran_ext.base.tx_data[1] = d1;
  _spi_tran_ext.base.tx_data[2] = d2 >> 8;
  _spi_tran_ext.base.tx_data[3] = d2;
  _spi_tran_ext.base.length = 32;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeC8Bytes(uint8_t c, uint8_t *data, uint32_t len)
{
  CS_LOW();
  _spi_tran_ext.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  _spi_tran_ext.base.cmd = 0x02;
  _spi_tran_ext.base.addr = ((uint32_t)c) << 8;
  _spi_tran_ext.base.tx_buffer = data;
  _spi_tran_ext.base.length = len << 3;
  POLL_START();
  POLL_END();
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeRepeat(uint16_t p, uint32_t len)
{
  bool first_send = true;

  uint16_t bufLen = (len >= ESP32QSPI_MAX_PIXELS_AT_ONCE) ? ESP32QSPI_MAX_PIXELS_AT_ONCE : len;
  int16_t xferLen, l;
  uint32_t c32;
  MSB_32_16_16_SET(c32, p, p);

  l = (bufLen + 1) / 2;
  for (uint32_t i = 0; i < l; i++)
  {
    _buffer32[i] = c32;
  }

  CS_LOW();
  // Issue pixels in blocks from temp buffer
  while (len) // While pixels remain
  {
    xferLen = (bufLen <= len) ? bufLen : len; // How many this pass?

    if (first_send)
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
      _spi_tran_ext.base.cmd = 0x32;
      _spi_tran_ext.base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                 SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    _spi_tran_ext.base.tx_buffer = _buffer16;
    _spi_tran_ext.base.length = xferLen << 4;

    POLL_START();
    POLL_END();

    len -= xferLen;
  }
  CS_HIGH();
}

// LOCAL PATCH (ESP32-S3-WatchFace): PIPELINED flush.
// The original here was fully serial per chunk: convert RGB565->panel bytes into one
// DMA buffer, then spi_device_polling_start/end (CPU spins until the transfer is done),
// repeat. On this command-driven CO5300 the QSPI push is the bottleneck and that spin
// was pure idle wait. Now we PING-PONG the two pre-allocated DMA buffers and use the
// queued (interrupt) API: queue chunk N's transfer (returns immediately, DMA runs in
// the background), convert chunk N+1 into the OTHER buffer while N transmits, then
// reap N's result before reusing its buffer. So the per-chunk CPU conversion overlaps
// the previous chunk's transmission instead of waiting for it. Single CS bracket and
// the same per-chunk flags/cmd/addr as before, so the panel sees an identical write.
// Each in-flight chunk needs its own transaction struct (_spi_tran_ext / _spi_tran_ext2)
// and its own DMA buffer (_buffer32 / _2nd_buffer32) — both must stay alive until that
// chunk's get_trans_result returns.
void Arduino_ESP32QSPI::writePixels(uint16_t *data, uint32_t len)
{
  CS_LOW();

  spi_transaction_ext_t *trans[2] = {&_spi_tran_ext, &_spi_tran_ext2};
  uint32_t *bufs[2] = {_buffer32, _2nd_buffer32};

  uint8_t slot = 0;          // which buffer/struct to fill+queue this iteration
  bool in_flight = false;    // is there a previously-queued transaction to reap?
  bool first_send = true;
  uint32_t l, l2;
  uint16_t p1, p2;

  while (len)
  {
    l = (len > ESP32QSPI_MAX_PIXELS_AT_ONCE) ? ESP32QSPI_MAX_PIXELS_AT_ONCE : len;

    spi_transaction_ext_t *t = trans[slot];
    uint32_t *buf = bufs[slot];

    if (first_send)
    {
      t->base.flags = SPI_TRANS_MODE_QIO;
      t->base.cmd = 0x32;
      t->base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      t->base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                      SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }

    // Convert this chunk into the slot's DMA buffer (CPU work; overlaps the OTHER
    // slot's in-flight DMA transfer queued on the previous iteration).
    l2 = l >> 1;
    for (uint32_t i = 0; i < l2; ++i)
    {
      p1 = *data++;
      p2 = *data++;
      MSB_32_16_16_SET(buf[i], p1, p2);
    }
    if (l & 1)
    {
      p1 = *data++;
      MSB_16_SET(((uint16_t *)buf)[l - 1], p1);
    }

    t->base.tx_buffer = buf;
    t->base.length = l << 4;

    // Hand this chunk to the DMA engine; returns immediately (does NOT wait).
    spi_device_queue_trans(_handle, (spi_transaction_t *)t, portMAX_DELAY);

    // Now reap the PREVIOUS chunk (queued last iteration), if any. By the time we
    // get here its transfer has been running while we did the conversion above, so
    // this wait is short-to-zero — that overlap is the whole win.
    if (in_flight)
    {
      spi_transaction_t *done;
      spi_device_get_trans_result(_handle, &done, portMAX_DELAY);
    }
    in_flight = true;
    slot ^= 1;                 // ping-pong to the other buffer/struct

    len -= l;
  }

  // Drain the last queued transaction before dropping CS.
  if (in_flight)
  {
    spi_transaction_t *done;
    spi_device_get_trans_result(_handle, &done, portMAX_DELAY);
  }

  CS_HIGH();
}

// LOCAL PATCH (ESP32-S3-WatchFace): ASYNC tile write — see header. Unlike
// writePixels() above there is NO per-chunk CPU work at all: the buffer is already
// in panel byte order, so the whole area goes out as <=ASYNC_MAX_SEG large queued
// DMA transactions and this returns as soon as they're queued (~µs). The caller's
// CPU time is then free (LVGL renders the next tile) while the wire drains.
// CS and the completion callback are driven from _async_pre_cb/_async_post_cb.
bool Arduino_ESP32QSPI::writePixelsBeAsync(const uint8_t *data, uint32_t len, void (*done_cb)(void *), void *done_arg)
{
  // 32760 = largest multiple of 4 bytes under the 2^18-bit hw transaction cap.
  const uint32_t SEG = 32760;
  if (!len)
  {
    if (done_cb)
    {
      done_cb(done_arg);
    }
    return true;
  }
  uint32_t nseg = (len + SEG - 1) / SEG;
  if (nseg > (uint32_t)ASYNC_MAX_SEG)
  {
    return false; // too big for one bracket — caller uses the sync path
  }

  waitAsync(); // only one async write may be in flight
  _async_done_cb = done_cb;
  _async_done_arg = done_arg;

  for (uint32_t i = 0; i < nseg; i++)
  {
    spi_transaction_ext_t *t = &_async_tran[i];
    AsyncSeg *s = &_async_seg[i];
    s->bus = this;
    s->first = (i == 0);
    s->last = (i == nseg - 1);

    uint32_t off = i * SEG;
    uint32_t l = len - off;
    if (l > SEG)
    {
      l = SEG;
    }

    memset(t, 0, sizeof(*t));
    if (i == 0)
    {
      // first segment carries the write-continue command, like writePixels()
      t->base.flags = SPI_TRANS_MODE_QIO;
      t->base.cmd = 0x32;
      t->base.addr = 0x003C00;
    }
    else
    {
      t->base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                      SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    t->base.tx_buffer = data + off;
    t->base.length = l << 3;
    t->base.user = s;

    if (spi_device_queue_trans(_handle, (spi_transaction_t *)t, portMAX_DELAY) != ESP_OK)
    {
      waitAsync(); // reap whatever did queue, leave the bus clean
      return false;
    }
    _async_pending++;
  }
  return true;
}

void Arduino_ESP32QSPI::waitAsync()
{
  while (_async_pending)
  {
    spi_transaction_t *done;
    spi_device_get_trans_result(_handle, &done, portMAX_DELAY);
    _async_pending--;
  }
}

// Both callbacks run in the SPI interrupt for EVERY transaction on this device;
// sync transactions have .user == NULL and fall straight through. CS is written
// via the raw port registers (NOT CS_LOW/CS_HIGH — CS_LOW drains the async queue
// and must never run inside this ISR).
void Arduino_ESP32QSPI::_async_pre_cb(spi_transaction_t *t)
{
  AsyncSeg *s = (AsyncSeg *)t->user;
  if (s && s->first)
  {
    *(s->bus->_csPortClr) = s->bus->_csPinMask;
  }
}

void Arduino_ESP32QSPI::_async_post_cb(spi_transaction_t *t)
{
  AsyncSeg *s = (AsyncSeg *)t->user;
  if (s && s->last)
  {
    Arduino_ESP32QSPI *b = s->bus;
    *(b->_csPortSet) = b->_csPinMask;
    if (b->_async_done_cb)
    {
      b->_async_done_cb(b->_async_done_arg);
    }
  }
}

void Arduino_ESP32QSPI::batchOperation(const uint8_t *operations, size_t len)
{
  for (size_t i = 0; i < len; ++i)
  {
    uint8_t l = 0;
    switch (operations[i])
    {
    case BEGIN_WRITE:
      beginWrite();
      break;
    case WRITE_COMMAND_8:
      writeCommand(operations[++i]);
      break;
    case WRITE_COMMAND_16:
      _data16.msb = operations[++i];
      _data16.lsb = operations[++i];
      writeCommand16(_data16.value);
      break;
    case WRITE_DATA_8:
      write(operations[++i]);
      break;
    case WRITE_DATA_16:
      _data16.msb = operations[++i];
      _data16.lsb = operations[++i];
      write16(_data16.value);
      break;
    case WRITE_BYTES:
      l = operations[++i];
      memcpy(_buffer, operations + i + 1, l);
      i += l;
      writeBytes(_buffer, l);
      break;
    case WRITE_C8_D8:
      l = operations[++i];
      writeC8D8(l, operations[++i]);
      break;
    case WRITE_C8_D16:
      l = operations[++i];
      _data16.msb = operations[++i];
      _data16.lsb = operations[++i];
      writeC8D16(l, _data16.value);
      break;
    case WRITE_C8_BYTES:
    {
      uint8_t c = operations[++i];
      l = operations[++i];
      memcpy(_buffer, operations + i + 1, l);
      i += l;
      writeC8Bytes(c, _buffer, l);
    }
    break;
    case WRITE_C16_D16:
      break;
    case END_WRITE:
      endWrite();
      break;
    case DELAY:
      delay(operations[++i]);
      break;
    default:
      printf("Unknown operation id at %d: %d\n", i, operations[i]);
      break;
    }
  }
}

void Arduino_ESP32QSPI::writeBytes(uint8_t *data, uint32_t len)
{
  CS_LOW();
  uint32_t l;
  bool first_send = true;
  while (len)
  {
    l = (len >= (ESP32QSPI_MAX_PIXELS_AT_ONCE << 1)) ? (ESP32QSPI_MAX_PIXELS_AT_ONCE << 1) : len;

    if (first_send)
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
      _spi_tran_ext.base.cmd = 0x32;
      _spi_tran_ext.base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                 SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }

    _spi_tran_ext.base.tx_buffer = data;
    _spi_tran_ext.base.length = l << 3;

    POLL_START();
    POLL_END();

    len -= l;
    data += l;
  }
  CS_HIGH();
}

void Arduino_ESP32QSPI::write16bitBeRGBBitmapR1(uint16_t *bitmap, int16_t w, int16_t h)
{
  if (h > ESP32QSPI_MAX_PIXELS_AT_ONCE)
  {
    log_e("h > ESP32QSPI_MAX_PIXELS_AT_ONCE, h: %d", h);
  }
  else
  {
    CS_LOW();
    uint32_t l = h << 4;
    bool first_send = true;
    uint16_t *p;
    uint16_t *origin_offset = bitmap + ((h - 1) * w);

    for (int16_t i = 0; i < w; i++)
    {
      if (first_send)
      {
        _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
        _spi_tran_ext.base.cmd = 0x32;
        _spi_tran_ext.base.addr = 0x003C00;
        first_send = false;
      }
      else
      {
        POLL_END();
        _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                   SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      }

      p = origin_offset + i;
      for (int16_t j = 0; j < h; j++)
      {
        _buffer16[j] = *p;
        p -= w;
      }

      _spi_tran_ext.base.tx_buffer = _buffer16;
      _spi_tran_ext.base.length = l;

      POLL_START();
    }
    POLL_END();
    CS_HIGH();
  }
}

void Arduino_ESP32QSPI::writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len)
{
  CS_LOW();
  uint32_t l, l2;
  uint16_t p1, p2;
  bool first_send = true;
  while (len)
  {
    l = (len > ESP32QSPI_MAX_PIXELS_AT_ONCE) ? ESP32QSPI_MAX_PIXELS_AT_ONCE : len;

    if (first_send)
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
      _spi_tran_ext.base.cmd = 0x32;
      _spi_tran_ext.base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                 SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    l2 = l >> 1;
    for (uint32_t i = 0; i < l2; ++i)
    {
      p1 = idx[*data++];
      p2 = idx[*data++];
      MSB_32_16_16_SET(_buffer32[i], p1, p2);
    }
    if (l & 1)
    {
      p1 = idx[*data++];
      MSB_16_SET(_buffer16[l - 1], p1);
    }

    _spi_tran_ext.base.tx_buffer = _buffer32;
    _spi_tran_ext.base.length = l << 4;

    POLL_START();
    POLL_END();

    len -= l;
  }
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len)
{
  CS_LOW();
  uint32_t l;
  uint16_t p;
  bool first_send = true;
  while (len)
  {
    l = (len > (ESP32QSPI_MAX_PIXELS_AT_ONCE >> 1)) ? (ESP32QSPI_MAX_PIXELS_AT_ONCE >> 1) : len;

    if (first_send)
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
      _spi_tran_ext.base.cmd = 0x32;
      _spi_tran_ext.base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                 SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    for (uint32_t i = 0; i < l; ++i)
    {
      p = idx[*data++];
      MSB_32_16_16_SET(_buffer32[i], p, p);
    }

    _spi_tran_ext.base.tx_buffer = _buffer32;
    _spi_tran_ext.base.length = l << 5;

    POLL_START();
    POLL_END();

    len -= l;
  }
  CS_HIGH();
}

void Arduino_ESP32QSPI::writeYCbCrPixels(uint8_t *yData, uint8_t *cbData, uint8_t *crData, uint16_t w, uint16_t h)
{
  if (w > (ESP32QSPI_MAX_PIXELS_AT_ONCE / 2))
  {
    Arduino_DataBus::writeYCbCrPixels(yData, cbData, crData, w, h);
  }
  else
  {
    bool first_send = true;

    int cols = w >> 1;
    int rows = h >> 1;
    uint8_t *yData2 = yData + w;
    uint16_t *dest = _buffer16;
    uint16_t *dest2 = dest + w;

    uint16_t out_bits = w << 5;

    uint8_t pxCb, pxCr;
    int16_t pxR, pxG, pxB, pxY;

    CS_LOW();
    for (int row = 0; row < rows; ++row)
    {
      for (int col = 0; col < cols; ++col)
      {
        pxCb = *cbData++;
        pxCr = *crData++;
        pxR = CR2R16[pxCr];
        pxG = -CB2G16[pxCb] - CR2G16[pxCr];
        pxB = CB2B16[pxCb];

        pxY = Y2I16[*yData++];
        *dest++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData++];
        *dest++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData2++];
        *dest2++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData2++];
        *dest2++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
      }
      yData += w;
      yData2 += w;

      if (first_send)
      {
        _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
        _spi_tran_ext.base.cmd = 0x32;
        _spi_tran_ext.base.addr = 0x003C00;
        first_send = false;
      }
      else
      {
        POLL_END();
        _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                   SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      }

      if (row & 1)
      {
        _spi_tran_ext.base.tx_buffer = _2nd_buffer32;
        dest = _buffer16;
      }
      else
      {
        _spi_tran_ext.base.tx_buffer = _buffer32;
        dest = _2nd_buffer16;
      }
      _spi_tran_ext.base.length = out_bits;

      POLL_START();
      dest2 = dest + w;
    }
    POLL_END();
    CS_HIGH();
  }
}
/******** low level bit twiddling **********/

GFX_INLINE void Arduino_ESP32QSPI::CS_HIGH(void)
{
  *_csPortSet = _csPinMask;
}

GFX_INLINE void Arduino_ESP32QSPI::CS_LOW(void)
{
  // LOCAL PATCH (ESP32-S3-WatchFace): every sync bus operation starts with CS_LOW,
  // so this is the single choke point that keeps commands (addr window, brightness,
  // sleep) from interleaving with an in-flight async tile write.
  waitAsync();
  *_csPortClr = _csPinMask;
}

GFX_INLINE void Arduino_ESP32QSPI::POLL_START()
{
  spi_device_polling_start(_handle, _spi_tran, portMAX_DELAY);
}

GFX_INLINE void Arduino_ESP32QSPI::POLL_END()
{
  spi_device_polling_end(_handle, portMAX_DELAY);
}

#endif // #if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
