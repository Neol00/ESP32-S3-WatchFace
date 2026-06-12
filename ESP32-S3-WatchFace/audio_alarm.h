/* ============================================================================
 *  audio_alarm.h — alarm chime through the ES8311 codec + NS4150B speaker amp.
 *
 *  Hardware (per the Waveshare schematic / 08_ES8311 demo):
 *    GPIO41 I2S_SCLK (BCLK)   GPIO45 I2S_LRCK (WS)   GPIO40 I2S_DSDIN (data
 *    to codec)   GPIO16 I2S_MCLK   GPIO46 "Codec_CE" — despite the net name
 *    this only enables the NS4150B AMP (10K pulldown; proof: the ES8311 answers
 *    at 0x18, the CE-pin-LOW address, while GPIO46 is HIGH). The ES8311 itself
 *    sits on the always-on 3.3V rail and is NEVER unpowered — not by GPIO46 and
 *    not by the deep-sleep rail cuts — so it MUST be put back into register-level
 *    power-down after every ring (es8311_power_down), or its analog/DAC blocks
 *    keep drawing a few mA forever, deep sleep included.
 *    The ES8311 sits on the shared touch/PMU I2C bus (Wire, addr 0x18).
 *
 *  No audio file: the melody is synthesized live, chiptune style — sustained
 *  band-limited square waves (Game Boy pulse channel) gated by real MIDI note
 *  durations. The note table is GENERATED from a .mid by tools/midi_to_chime.py
 *  into chime_melody.h. It starts at ~30% volume and ramps to full over 25 s,
 *  so it wakes you gently but won't let you sleep through it.
 *
 *  Threading: audio_alarm_start() does ALL I2C from the caller (the loop task,
 *  where every other Wire user lives), then spawns a small task on core 0 that
 *  only renders samples and blocks on the I2S DMA. audio_alarm_stop() flags it,
 *  waits for it to exit, then cuts Codec_CE and frees the I2S channel — between
 *  rings the whole audio path is unpowered and costs nothing.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "ESP_I2S.h"
#include "driver/gpio.h"            // gpio_hold_* (keep the amp OFF through deep sleep)

#define AUDIO_PIN_BCLK   41
#define AUDIO_PIN_LRCK   45
#define AUDIO_PIN_DOUT   40
#define AUDIO_PIN_MCLK   16
#define AUDIO_PIN_CE     46         // Codec_CE: codec + NS4150B power gate, active HIGH

#define AUDIO_SAMPLE_RATE 16000     // MCLK = 256*fs = 4.096 MHz (in the ES8311 coeff table)
#define AUDIO_ES8311_ADDR 0x18      // ES8311_ADDRRES_0 (CE pin strapped low... addr select)

/* ---- speaker loudness profile ----
 * Define AUDIO_SMALL_SPEAKER 1 (in the .ino, before this include) on a watch
 * modified with a tiny/inefficient speaker (in-ear driver): full drive — codec
 * at +12.5 dB digital gain plus full software gain.
 * Default 0 = the ORIGINAL Waveshare speaker, which that drive overwhelms badly
 * (the +12.5 dB boost alone eats the DAC's headroom, so it clips even during
 * the "quiet" ramp-in and would blow the driver at full volume): codec at 0 dB
 * and ~1/4 the software gain, about -24 dB total vs. the small-speaker build. */
#ifndef AUDIO_SMALL_SPEAKER
#define AUDIO_SMALL_SPEAKER 0
#endif
#if AUDIO_SMALL_SPEAKER
#define AUDIO_CODEC_VOL    0xD8     // ES8311 reg32: +12.5 dB (0xBF = 0 dB, 0.5 dB/step)
#define AUDIO_MASTER_GAIN  6500.0f  // ~4 sounding squares before the clamp bites
#else
#define AUDIO_CODEC_VOL    0xBF     // 0 dB — no digital boost, DAC keeps its headroom
#define AUDIO_MASTER_GAIN  1600.0f
#endif

static I2SClass     s_audio_i2s;
static volatile TaskHandle_t s_audio_task = nullptr;  // written by the render task (core 0)
static volatile bool s_audio_stop  = false;   // ask the render task to wind down
static bool          s_audio_live  = false;   // alarm started; stop() has work to do
static bool          s_ding_live   = false;   // notification ding playing; the loop's
                                              // audio_alarm_tick() tears down after it
static bool          s_audio_down_pending = false;    // join timed out with the render task
                                              // still alive: tick() downs the path once it exits

/* ----------------------------- ES8311 over Wire --------------------------- */

static bool es8311_wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t es8311_rd(uint8_t reg) {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)AUDIO_ES8311_ADDR, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

/* Bring the codec from reset to "DAC playing I2S slave, 16-bit @ 16 kHz, MCLK
 * from the MCLK pin". This is the espressif es8311 driver's init path inlined
 * with the coefficients for (MCLK 4.096 MHz, fs 16 kHz): pre_div 1, pre_multi
 * 1x, adc/dac_div 1, single speed, lrck 0x00FF, bclk_div 4, osr 0x10. */
static bool es8311_codec_init(void) {
  if (!es8311_wr(0x00, 0x1F)) return false;       // reset
  es8311_wr(0x00, 0x00);
  es8311_wr(0x00, 0x80);                          // power on, slave mode

  es8311_wr(0x01, 0x3F);                          // all clocks on, MCLK from MCLK pin
  es8311_wr(0x02, es8311_rd(0x02) & 0x07);        // pre_div 1, pre_multi 1x
  es8311_wr(0x03, 0x10);                          // single speed, adc_osr 0x10
  es8311_wr(0x04, 0x10);                          // dac_osr 0x10
  es8311_wr(0x05, 0x00);                          // adc_div 1, dac_div 1
  es8311_wr(0x06, (es8311_rd(0x06) & 0xC0) | 0x03); // sclk not inverted, bclk_div 4
  es8311_wr(0x07, es8311_rd(0x07) & 0xC0);        // lrck_h 0x00
  es8311_wr(0x08, 0xFF);                          // lrck_l 0xFF

  es8311_wr(0x09, 0x0C);                          // SDP-in  16-bit I2S
  es8311_wr(0x0A, 0x0C);                          // SDP-out 16-bit I2S
  es8311_wr(0x0D, 0x01);                          // power up analog circuitry
  es8311_wr(0x0E, 0x02);                          // PGA/ADC modulator on
  es8311_wr(0x12, 0x00);                          // power up DAC
  es8311_wr(0x13, 0x10);                          // enable output to HP drive
  es8311_wr(0x1C, 0x6A);                          // ADC EQ bypass, DC offset cancel
  es8311_wr(0x37, 0x08);                          // bypass DAC EQ

  es8311_wr(0x32, AUDIO_CODEC_VOL);               // DAC volume per the speaker profile above
  es8311_wr(0x31, 0x00);                          // unmute
  return true;
}

/* Put the codec back into its low-power standby. The chip is on the always-on
 * 3.3V rail, so this register script is the ONLY thing standing between "ring
 * once" and "+mA forever". Sequence is esp-adf's es8311_suspend() plus clearing
 * the power-on bit and re-gating the internal clocks. */
static void es8311_power_down(void) {
  es8311_wr(0x31, 0x60);   // mute DAC first (kills the power-down pop)
  es8311_wr(0x32, 0x00);   // DAC volume 0
  es8311_wr(0x0E, 0xFF);   // power down ADC/PGA/modulator
  es8311_wr(0x12, 0x02);   // power down DAC
  es8311_wr(0x14, 0x00);   // PGA off
  es8311_wr(0x0D, 0xFA);   // power down analog circuitry
  es8311_wr(0x15, 0x00);
  es8311_wr(0x37, 0x08);
  es8311_wr(0x45, 0x01);
  es8311_wr(0x00, 0x00);   // clear the power-on bit -> standby
  es8311_wr(0x01, 0x30);   // gate internal clocks (register default)
}

/* ------------------------------- the chime -------------------------------- */
/* Bell voices: fundamental + 2nd/3rd harmonics, each dying exponentially. The
 * harmonics decay faster than the fundamental, which is what reads as "bell"
 * instead of "buzzer". */

#define CHIME_RAMP_S     25.0f      // seconds from gentle (30%) to full volume
#define CHIME_VOICES     10         // organ peaks at ~4 sounding notes; headroom for
                                    // release tails overlapping retriggers

/* The melody (chime_notes[] + CHIME_PERIOD_MS) is generated from a MIDI file by
 * tools/midi_to_chime.py — see the header's comment for the regenerate command. */
#include "chime_melody.h"

#ifndef CHIME_INTRO_MS              // older generated headers: no pickup, loop from 0
#define CHIME_INTRO_MS 0
#endif

/* Chiptune voices: the source tune is an old Game Boy track, so each note is a
 * SUSTAINED band-limited square wave (odd harmonics, like the GB pulse channel)
 * gated by the note's real MIDI duration — not a struck bell. Attack/release
 * are just long enough to kill the on/off clicks. */
#define CHIME_ATTACK_SAMP  (AUDIO_SAMPLE_RATE / 333)  // ~3 ms fade-in
#define CHIME_RELEASE_SAMP (AUDIO_SAMPLE_RATE / 50)   // ~20 ms fade-out

typedef struct {
  float ph, w;                      // phase + per-sample phase increment (rad)
  float amp;
  uint32_t sustain;                 // samples until note-off (from MIDI duration)
  uint16_t attack;                  // samples left of fade-in
  uint16_t release;                 // samples left of fade-out after note-off
  uint8_t  nharm;                   // odd harmonics that stay under the alias guard
  bool live;
} chime_voice_t;

static void audio_render_task(void *arg) {
  (void)arg;
  static int16_t buf[256 * 2];                    // 256 frames stereo = 16 ms
  chime_voice_t v[CHIME_VOICES] = {};
  uint32_t frames_total = 0;                      // ring time, in samples
  uint32_t period_pos   = 0;                      // position inside the melody loop
  size_t   next_note    = 0;

  while (!s_audio_stop) {
    for (int i = 0; i < 256; i++) {
      /* note scheduler: start voices as their time comes up */
      uint32_t pos_ms = period_pos / (AUDIO_SAMPLE_RATE / 1000);
      while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
             pos_ms >= chime_notes[next_note].at_ms) {
        float freq = chime_notes[next_note].freq;
        uint8_t nh = 0;                           // odd harmonics under the 7 kHz
        for (int n = 1; n <= 7; n += 2)           // alias guard (Nyquist is 8 kHz)
          if (freq * n < 7000.0f) nh++;
        for (int k = 0; k < CHIME_VOICES; k++) {
          if (v[k].live) continue;
          v[k] = { 0.0f, 2.0f * (float)M_PI * freq / AUDIO_SAMPLE_RATE,
                   chime_notes[next_note].amp,
                   (uint32_t)chime_notes[next_note].dur_ms * (AUDIO_SAMPLE_RATE / 1000),
                   CHIME_ATTACK_SAMP, CHIME_RELEASE_SAMP, nh, true };
          break;
        }
        next_note++;
      }
      if (++period_pos >= (uint32_t)(CHIME_PERIOD_MS * (AUDIO_SAMPLE_RATE / 1000))) {
        /* repeats skip the pickup: restart at the intro mark, with next_note
         * past every note that starts before it */
        period_pos = (uint32_t)(CHIME_INTRO_MS * (AUDIO_SAMPLE_RATE / 1000));
        next_note  = 0;
        while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
               chime_notes[next_note].at_ms < CHIME_INTRO_MS) next_note++;
      }

      float s = 0.0f;
      for (int k = 0; k < CHIME_VOICES; k++) {
        if (!v[k].live) continue;
        /* band-limited square: sin(ph) + sin(3ph)/3 + sin(5ph)/5 + sin(7ph)/7 */
        float sq = 0.0f, n = 1.0f;
        for (uint8_t h = 0; h < v[k].nharm; h++, n += 2.0f)
          sq += sinf(n * v[k].ph) / n;
        /* gate: attack -> sustain (MIDI duration) -> release -> free */
        float env;
        if      (v[k].attack)  { env = 1.0f - (float)v[k].attack / CHIME_ATTACK_SAMP; v[k].attack--; }
        else if (v[k].sustain) { env = 1.0f; v[k].sustain--; }
        else if (v[k].release) { env = (float)v[k].release / CHIME_RELEASE_SAMP; v[k].release--; }
        else                   { v[k].live = false; continue; }
        s += v[k].amp * env * sq;
        v[k].ph += v[k].w;
        if (v[k].ph > 2.0f * (float)M_PI) v[k].ph -= 2.0f * (float)M_PI;
      }

      /* gentle-start master ramp: 30% -> 100% over CHIME_RAMP_S */
      float t   = (float)frames_total / AUDIO_SAMPLE_RATE;
      float vol = 0.30f + 0.70f * (t >= CHIME_RAMP_S ? 1.0f : t / CHIME_RAMP_S);
      int32_t q = (int32_t)(s * vol * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[2 * i] = buf[2 * i + 1] = (int16_t)q;
      frames_total++;
    }
    s_audio_i2s.write((uint8_t *)buf, sizeof(buf));       // blocks on DMA = self-pacing
  }

  memset(buf, 0, sizeof(buf));                            // flush silence so the amp
  s_audio_i2s.write((uint8_t *)buf, sizeof(buf));         // isn't cut mid-waveform
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ---------------------------- notification ding --------------------------- */
/* One short E6 square-wave blip (same chiptune voice as the alarm): ~3 ms
 * attack, 90 ms sustain, 60 ms release, a bit softer than the melody. The task
 * only renders; the loop's audio_alarm_tick() does the I2C teardown after it. */

static void audio_ding_task(void *arg) {
  (void)arg;
  static int16_t buf[256 * 2];
  const float w = 2.0f * (float)M_PI * 1318.51f / AUDIO_SAMPLE_RATE;  // E6
  const uint32_t attack  = CHIME_ATTACK_SAMP;                         // 5th harmonic
  const uint32_t sustain = (AUDIO_SAMPLE_RATE * 90) / 1000;           // 6.6 kHz — still
  const uint32_t release = (AUDIO_SAMPLE_RATE * 60) / 1000;           // under the guard
  const uint32_t total   = attack + sustain + release;
  uint32_t n = 0;
  float ph = 0.0f;

  while (n < total && !s_audio_stop) {
    for (int i = 0; i < 256; i++, n++) {
      float env = 0.0f;
      if      (n >= total)           env = 0.0f;
      else if (n < attack)           env = (float)n / attack;
      else if (n < attack + sustain) env = 1.0f;
      else                           env = (float)(total - n) / release;
      float sq = sinf(ph) + sinf(3.0f * ph) / 3.0f + sinf(5.0f * ph) / 5.0f;
      int32_t q = (int32_t)(0.7f * env * sq * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[2 * i] = buf[2 * i + 1] = (int16_t)q;
      ph += w;
      if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
    }
    s_audio_i2s.write((uint8_t *)buf, sizeof(buf));
  }
  memset(buf, 0, sizeof(buf));
  s_audio_i2s.write((uint8_t *)buf, sizeof(buf));
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ------------------------------- public API ------------------------------- */

/* Call once in setup(): park the amp rail OFF (and release any deep-sleep hold). */
static void audio_alarm_init(void) {
  gpio_hold_dis((gpio_num_t)AUDIO_PIN_CE);
  pinMode(AUDIO_PIN_CE, OUTPUT);
  digitalWrite(AUDIO_PIN_CE, LOW);
}

/* Call once after Wire.begin(): force the codec into standby no matter what
 * state a crash, watchdog reset, or older firmware left it in (it survives
 * everything short of battery removal — reboots don't reset it). */
static void audio_alarm_quiesce_codec(void) {
  es8311_power_down();
}

/* Latch the amp enable LOW through deep sleep (GPIO46 is not an RTC pin; the
 * digital hold rides on gpio_deep_sleep_hold_en(), which haptics already set).
 * Also re-run the codec standby script: it's ~20 I2C writes of insurance against
 * sleeping with the codec's analog blocks still powered. */
static void audio_alarm_prepare_sleep(void) {
  digitalWrite(AUDIO_PIN_CE, LOW);
  gpio_hold_en((gpio_num_t)AUDIO_PIN_CE);
  es8311_power_down();
}

/* Amp on + I2S up + codec to playback. All I2C happens on the CALLER's task (the
 * loop task, where every other Wire user lives) — render tasks never touch Wire. */
static bool audio_path_up(const char *who) {
  // A previous render task that overstayed its join window may still be winding
  // down: starting a new I2S session under it would tear its channel away mid-write.
  // Refuse; audio_alarm_tick() finishes the deferred teardown once it exits.
  if (s_audio_task || s_audio_down_pending) {
    Serial.printf("[audio] previous render task still winding down - no %s sound\n", who);
    return false;
  }
  digitalWrite(AUDIO_PIN_CE, HIGH);               // enable the NS4150B amp
  delay(10);                                      // let it settle before I2C

  s_audio_i2s.setPins(AUDIO_PIN_BCLK, AUDIO_PIN_LRCK, AUDIO_PIN_DOUT, -1, AUDIO_PIN_MCLK);
  if (!s_audio_i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE,
                         I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.printf("[audio] I2S begin failed - no %s sound\n", who);
    digitalWrite(AUDIO_PIN_CE, LOW);
    return false;
  }
  if (!es8311_codec_init()) {
    Serial.printf("[audio] ES8311 not responding - no %s sound\n", who);
    s_audio_i2s.end();
    digitalWrite(AUDIO_PIN_CE, LOW);
    return false;
  }
  return true;
}

static void audio_path_down(void) {
  digitalWrite(AUDIO_PIN_CE, LOW);                // amp off first: kills any teardown pop
  es8311_power_down();                            // codec back to standby — GPIO46 does NOT
                                                  // cut its power, only the amp's
  s_audio_i2s.end();
}

/* Wait for the current render task (alarm or ding) to exit, then drop the path.
 * NEVER end() the I2S driver while the task is still alive: it may be blocked
 * inside s_audio_i2s.write(), and deleting the channel under it makes the driver
 * teardown fail half-way — ESP_I2S then leaks the channel, and after two of those
 * every begin() fails with "i2s_new_channel: no available channel found" until
 * reboot (the no-speaker bug). Normally the task exits within one ~16 ms chunk;
 * the 1 s ceiling covers it being starved by BLE traffic on core 0. If it STILL
 * hasn't exited, defer the teardown to audio_alarm_tick() instead of forcing it. */
static void audio_path_join_down(void) {
  s_audio_stop = true;
  for (int i = 0; i < 200 && s_audio_task; i++) delay(5);
  if (s_audio_task) {
    Serial.println("[audio] render task did not exit in time - deferring I2S teardown");
    digitalWrite(AUDIO_PIN_CE, LOW);   // silence the amp + codec now, free I2S later
    es8311_power_down();
    s_audio_down_pending = true;
    return;
  }
  audio_path_down();
}

static void audio_alarm_start(void) {
  if (s_audio_live) return;
  if (s_ding_live) {                              // a ding is mid-flight: abort it and
    audio_path_join_down();                       // recycle the path for the alarm
    s_ding_live = false;
  }
  if (!audio_path_up("alarm")) return;            // vibration still works without sound

  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_render_task, "alarm_audio", 4096, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    Serial.println("[audio] render task spawn failed");
    audio_path_down();
    return;
  }
  s_audio_live = true;
}

static void audio_alarm_stop(void) {
  if (!s_audio_live) return;
  audio_path_join_down();
  s_audio_live = false;
}

/* Short notification blip. Call from the LOOP task only (does I2C). No-op while
 * the alarm rings, while a previous ding is still sounding, or when muted. */
static void audio_notify_ding(void) {
  if (settings_get_mute()) return;
  if (s_audio_live || s_ding_live) return;
  if (!audio_path_up("notification")) return;

  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_ding_task, "notif_ding", 3072, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    audio_path_down();
    return;
  }
  s_ding_live = true;
}

/* Call every loop (like haptics_tick): once the ding task has finished rendering,
 * finish its teardown here on the loop task — codec standby + amp off + I2S free. */
static void audio_alarm_tick(void) {
  if (s_audio_down_pending && s_audio_task == nullptr) {   // overdue join: finish it now
    audio_path_down();
    s_audio_down_pending = false;
  }
  if (s_ding_live && s_audio_task == nullptr && !s_audio_down_pending) {
    audio_path_down();
    s_ding_live = false;
  }
}
