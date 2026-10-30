// music.h — the player's music: tracks packed by tools/halo_music.py (assets/music.conf) into the "ffat" data
// partition — raw, no file system — and decoded on the fly while they play.
//
// The image: a 4 KB header ("HWM1", the track count, the sample rate, the block size, samples per block; per track —
// where it is, how long, the title and the artist), then the tracks, each from a 64 KB boundary. A track is IMA ADPCM,
// mono, 22 050 Hz, Microsoft layout: 256-byte blocks — 4 bytes of header (the first sample and the step index), then
// 504 samples, 4 bits each, low nibble first. 11 KB a second; decoding is a few operations a sample.
//
// Halo's audio task (core 0) pulls samples through mu_fill; the player (the frame task) only says what to play —
// through volatile fields (mu_cmd — start that track from its beginning, mu_play), and gets back where it is (mu_pos)
// and that a track ran out (mu_ended). A track is mapped into memory when it starts (esp_partition_mmap: reading goes
// through the flash cache, it does not stop the other core as a flash read call would).
#include <esp_partition.h>

#define MU_MAX 8
struct MuTrack {
  uint32_t off, bytes, samples;
  char title[57], artist[57];
};
static MuTrack mu_tracks[MU_MAX];
static int mu_n, mu_rate = 22050, mu_block = 256, mu_spb = 505;
static const esp_partition_t *mu_part;
static volatile int mu_cmd = -1;    // start this track (from its beginning)
static volatile bool mu_play;       // playing; false — silence, the position stays
static volatile uint32_t mu_pos;    // samples played of the current track
static volatile bool mu_ended;      // the current track ran out
static volatile int mu_cur = -1;    // the track mapped now
static esp_partition_mmap_handle_t mu_map;
static const uint8_t *mu_data;
static int16_t mu_buf[512];         // the decoded block
static int mu_have, mu_at;          // samples in it and how far it has been played
static uint32_t mu_next_block;
static volatile bool mu_audio_started;

static void music_begin() {
  mu_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  static uint8_t head[16 + MU_MAX * 128];
  if (!mu_part || esp_partition_read(mu_part, 0, head, sizeof(head)) != ESP_OK || memcmp(head, "HWM1", 4)) return;
  mu_n = min((int)(head[6] | head[7] << 8), MU_MAX);
  memcpy(&mu_rate, head + 8, 4);
  mu_block = head[12] | head[13] << 8, mu_spb = head[14] | head[15] << 8;
  if (mu_spb > 512) mu_n = 0;
  for (int i = 0; i < mu_n; i++) {
    const uint8_t *t = head + 16 + i * 128;
    memcpy(&mu_tracks[i].off, t, 4), memcpy(&mu_tracks[i].bytes, t + 4, 4), memcpy(&mu_tracks[i].samples, t + 8, 4);
    memcpy(mu_tracks[i].title, t + 12, 56), memcpy(mu_tracks[i].artist, t + 68, 56);
    mu_tracks[i].title[56] = mu_tracks[i].artist[56] = 0;
  }
}

static const int16_t IMA_STEP[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static void mu_decode(const uint8_t *b) {
  int pred = (int16_t)(b[0] | b[1] << 8), idx = min((int)b[2], 88);
  mu_buf[0] = pred;
  const int nb = (mu_spb - 1) / 2;
  for (int i = 0; i < nb; i++) {
    const uint8_t byte = b[4 + i];
    for (int k = 0; k < 2; k++) {
      const int nib = k ? byte >> 4 : byte & 15, step = IMA_STEP[idx];
      int diff = step >> 3;
      if (nib & 1) diff += step >> 2;
      if (nib & 2) diff += step >> 1;
      if (nib & 4) diff += step;
      pred = constrain(nib & 8 ? pred - diff : pred + diff, -32768, 32767);
      idx = constrain(idx + IMA_INDEX[nib], 0, 88);
      mu_buf[1 + 2 * i + k] = pred;
    }
  }
}

static bool mu_open(int t) {  // map the track into memory
  if (mu_data) esp_partition_munmap(mu_map), mu_data = nullptr;
  const void *p = nullptr;
  if (esp_partition_mmap(mu_part, mu_tracks[t].off, mu_tracks[t].bytes, ESP_PARTITION_MMAP_DATA, &p, &mu_map) != ESP_OK)
    return false;
  mu_data = (const uint8_t *)p;
  return true;
}

// The audio task: n samples of the current track (fewer — it ran out, or nothing is playing)
static int mu_fill(int16_t *out, int n) {
  const int cmd = mu_cmd;
  if (cmd >= 0 && cmd < mu_n) {
    mu_cmd = -1;
    mu_cur = mu_open(cmd) ? cmd : -1;
    mu_pos = 0, mu_have = mu_at = 0, mu_next_block = 0, mu_ended = false;
  }
  if (!mu_play || !mu_data || mu_ended) return 0;
  const MuTrack &t = mu_tracks[mu_cur];
  int k = 0;
  while (k < n) {
    if (mu_at >= mu_have) {
      if ((mu_next_block + 1) * mu_block > t.bytes) {
        mu_ended = true;
        break;
      }
      mu_decode(mu_data + mu_next_block++ * mu_block);
      mu_have = mu_spb, mu_at = 0;
    }
    const int m = min(n - k, mu_have - mu_at);
    memcpy(out + k, mu_buf + mu_at, m * 2);
    k += m, mu_at += m;
  }
  mu_pos += k;
  return k;
}

static void music_start_audio() {  // the first play: I2S and the codec — in a task of their own (a few ms of I2C)
  if (mu_audio_started) return;
  mu_audio_started = true;
  xTaskCreatePinnedToCore(
      [](void *) {
        halo_audio_begin(mu_fill, mu_rate);
        vTaskDelete(nullptr);
      },
      "audio_init", 3072, nullptr, 2, nullptr, 0);
}
