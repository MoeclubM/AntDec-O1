#include "device_store.h"
#include "stm32f1xx_hal.h"
#include <stddef.h>
#include <string.h>

/* Time and output settings share two append-only pages; commit is last. */
#define STORE_BASE 0x0803F000UL
#define PAGE_BYTES 2048UL
#define RECORD_MAGIC 0x41545031UL
#define FILTER_MAGIC 0x41544680UL
#define SETTINGS_MAGIC 0x41445331UL
typedef struct { uint32_t sequence, seconds, inverse, magic; } legacy_record_t;
typedef struct {
  uint32_t sequence, seconds, inverse;
  device_settings_t settings;
  uint16_t revision;
  uint32_t checksum, magic;
} record_t;
_Static_assert(sizeof(device_settings_t) == 10, "Settings wire layout changed");
_Static_assert(sizeof(record_t) == 32, "Journal record layout changed");
static const device_settings_t k_defaults = {0, 0, 40, 0, 2, 32, 8, 15, 0, 1};
static device_settings_t s_settings, s_saved_settings;
static uint16_t s_revision, s_saved_revision, s_sync_revision;
static uint8_t s_sync_parts;
static bool s_sync_active;
static uint32_t s_seconds, s_saved, s_sequence, s_next = STORE_BASE, s_page = STORE_BASE;
static uint32_t s_committed_page = STORE_BASE;
static uint32_t s_source, s_source_ms, s_retry_ms;
static uint32_t s_settings_ms;
static bool s_seen_source, s_was_playing, s_stop_pending, s_failed;

static uint32_t record_crc(const record_t *record)
{
  const uint8_t *data = (const uint8_t *)record;
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 0; i < offsetof(record_t, checksum); i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
  }
  return ~crc;
}

bool DeviceStore_SettingsValid(const device_settings_t *s)
{
  return s->filter < 8U && s->balance >= -24 && s->balance <= 24 &&
         s->startup <= 200U && s->ceiling <= s->startup &&
         s->automute_level && s->automute_time && s->invert < 4U && s->automute_enabled < 2U;
}

void DeviceStore_Init(void)
{
  s_settings = s_saved_settings = k_defaults;
  /* Existing 16-byte records migrate without erasing their accumulated time. */
  for (uint32_t address = STORE_BASE; address < STORE_BASE + 2 * PAGE_BYTES; address += sizeof(legacy_record_t)) {
    const legacy_record_t *old = (const legacy_record_t *)address;
    uint32_t page = address & ~(PAGE_BYTES - 1U), size = 0;
    device_settings_t settings = k_defaults;
    uint16_t revision = 0;
    if (address + sizeof(record_t) <= page + PAGE_BYTES) {
      const record_t *r = (const record_t *)address;
      if (r->magic == SETTINGS_MAGIC && r->inverse == ~r->seconds &&
          r->checksum == record_crc(r) && DeviceStore_SettingsValid(&r->settings)) {
        settings = r->settings; revision = r->revision; size = sizeof(record_t);
      }
    }
    if (!size) {
      bool has_filter = (old->magic & 0xFFFFFFC0UL) == FILTER_MAGIC &&
                        (((old->magic >> 3) ^ old->magic) & 7U) == 7U;
      if ((old->magic == RECORD_MAGIC || has_filter) && old->inverse == ~old->seconds) {
        settings.filter = has_filter ? (uint8_t)(old->magic & 7U) : 0U;
        size = sizeof(legacy_record_t);
      }
    }
    if (!size || old->sequence <= s_sequence) continue;
    s_sequence = old->sequence; s_saved = s_seconds = old->seconds;
    s_settings = s_saved_settings = settings;
    s_revision = s_saved_revision = revision;
    s_next = address + size; s_page = s_committed_page = page;
  }
}

static bool save(void)
{
  uint32_t next = s_next, page = s_page;
  while (next + sizeof(record_t) <= page + PAGE_BYTES) {
    const uint32_t *words = (const uint32_t *)next;
    bool erased = true;
    for (uint32_t i = 0; i < sizeof(record_t) / 4U; i++) erased = erased && words[i] == UINT32_MAX;
    if (erased) break;
    next += sizeof(legacy_record_t);
  }
  if (next + sizeof(record_t) > page + PAGE_BYTES) {
    page = page == STORE_BASE ? STORE_BASE + PAGE_BYTES : STORE_BASE;
    /* Recycle an uncommitted failed page before erasing the last valid record. */
    if (s_sequence && page == s_committed_page) page = s_page;
    next = page;
  }
  HAL_FLASH_Unlock();
  bool ok = true;
  if (next == page) {
    FLASH_EraseInitTypeDef erase = {.TypeErase = FLASH_TYPEERASE_PAGES, .PageAddress = page, .NbPages = 1};
    uint32_t error;
    ok = HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
  }
  if (!ok) {
    HAL_FLASH_Lock(); s_failed = true; s_retry_ms = HAL_GetTick(); return false;
  }
  record_t r = {.sequence = s_sequence + 1U, .seconds = s_seconds, .inverse = ~s_seconds,
                .settings = s_settings, .revision = s_revision, .magic = SETTINGS_MAGIC};
  r.checksum = record_crc(&r);
  uint32_t words[sizeof(record_t) / 4U];
  memcpy(words, &r, sizeof(r));
  for (uint32_t i = 0; ok && i < sizeof(record_t) / 4U; i++)
    ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, next + i * 4U, words[i]) == HAL_OK;
  HAL_FLASH_Lock();
  ok = ok && memcmp((const void *)next, &r, sizeof(r)) == 0;
  if (ok) {
    s_sequence = r.sequence; s_saved = r.seconds; s_saved_settings = s_settings; s_saved_revision = s_revision;
    s_committed_page = page; s_stop_pending = false;
  }
  s_page = page; s_next = next + sizeof(record_t);
  s_failed = !ok;
  s_retry_ms = HAL_GetTick();
  return ok;
}

void DeviceStore_ObservePlayTime(uint32_t source_seconds, uint32_t now_ms)
{
  if (s_seen_source) {
    uint32_t delta = (source_seconds - s_source) & 0xFFFFFU;
    /* A source reset establishes a fresh baseline; it must not add a 12-day wrap. */
    if (delta <= (now_ms - s_source_ms) / 1000U + 2U) s_seconds += delta;
  }
  s_source = source_seconds;
  s_source_ms = now_ms;
  s_seen_source = true;
}

void DeviceStore_Tick(bool playing, uint32_t now_ms)
{
  if (s_was_playing && !playing && s_seconds != s_saved) s_stop_pending = true;
  if (s_sync_active) { s_was_playing = playing; return; }
  bool settings_dirty = DeviceStore_SettingsPending();
  bool time_due = s_seconds != s_saved &&
                  (s_seconds - s_saved >= 120U || s_stop_pending || s_failed);
  if ((!settings_dirty || now_ms - s_settings_ms >= 2000U) &&
      (!s_failed || now_ms - s_retry_ms >= 5000U) && (settings_dirty || time_due)) save();
  s_was_playing = playing;
}

uint32_t DeviceStore_PlaySeconds(void) { return s_seconds; }
bool DeviceStore_SaveFailed(void) { return s_failed; }
const device_settings_t *DeviceStore_Settings(void) { return &s_settings; }
bool DeviceStore_SettingsPending(void) {
  return s_revision != s_saved_revision || memcmp(&s_settings, &s_saved_settings, sizeof(s_settings)) != 0;
}
void DeviceStore_UpdateSettings(const device_settings_t *settings)
{
  if (memcmp(&s_settings, settings, sizeof(s_settings)) == 0) return;
  s_settings = *settings;
  if (!s_sync_active) ++s_revision;
  s_settings_ms = HAL_GetTick();
}
uint16_t DeviceStore_SettingsRevision(void) { return s_revision; }
bool DeviceStore_SyncActive(void) { return s_sync_active; }
bool DeviceStore_SyncSettings(uint8_t part)
{
  unsigned kind = part >> 6;
  if (!kind) {
    s_sync_revision = (uint16_t)(part & 15U) << 12;
    s_sync_parts = 0; s_sync_active = true;
    return true;
  }
  if (!s_sync_active) return kind == 3U && s_sync_parts == 3U && s_revision == s_sync_revision;
  if (kind == 1U) { s_sync_revision = (s_sync_revision & ~63U) | (part & 63U); s_sync_parts |= 1U; }
  else if (kind == 2U) { s_sync_revision = (s_sync_revision & ~(63U << 6)) | ((uint16_t)(part & 63U) << 6); s_sync_parts |= 2U; }
  else {
    if (s_sync_parts != 3U || (part & 15U) != (s_sync_revision >> 12)) return false;
    s_revision = s_sync_revision; s_sync_active = false; s_settings_ms = HAL_GetTick();
  }
  return true;
}
void DeviceStore_AbortSync(void)
{
  if (s_sync_parts == 3U) s_revision = s_sync_revision;
  s_sync_active = false; s_settings_ms = HAL_GetTick();
}
bool DeviceStore_ResetPlayTime(void)
{
  if (s_sync_active) return false;
  uint32_t before = s_seconds;
  s_seconds = 0U;
  if (save()) return true;
  s_seconds = before;
  return false;
}
