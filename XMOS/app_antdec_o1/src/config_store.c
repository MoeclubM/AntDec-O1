#include "config_store.h"
#include <quadflashlib.h>
#include <stddef.h>
#include <string.h>

#define LEGACY_MAGIC 0x414E5432u
#define RECORD_MAGIC 0x414E5433u
#define COMMIT_MAGIC 0x43464731u
#define PAGE_BYTES 256u
extern unsigned int DFU_mode_active;
extern int flash_cmd_enable_ports(void);
extern int flash_cmd_disable_ports(void);
typedef struct { uint32_t magic; uint8_t audio[88]; uint32_t checksum; } legacy_t;
typedef struct {
    uint32_t magic, sequence;
    device_config_t config;
    uint32_t checksum, commit;
} record_t;
typedef char record_fits_page[(sizeof(record_t) <= PAGE_BYTES) ? 1 : -1];
static unsigned s_sequence, s_next_page;
static int s_sector = -1, s_committed_sector = -1;

uint32_t ConfigStore_Checksum(const void *data, unsigned length)
{
    const unsigned char *p = data;
    uint32_t value = 2166136261u;
    for (unsigned i = 0; i < length; i++) value = (value ^ p[i]) * 16777619u;
    return value;
}

/* These functions run only on Endpoint0, serialised with its DFU handler. */
int ConfigStore_Load(device_config_t *config)
{
    if (DFU_mode_active || !flash_cmd_enable_ports()) return 0;
    int count = fl_getNumSectors(), found = 0;
    unsigned char page[PAGE_BYTES] __attribute__((aligned(4)));
    if (count > 4) {
        s_sector = count - 2; s_next_page = 0;
        for (int sector = count - 2; sector < count; sector++) {
            unsigned base = fl_getSectorAddress(sector), pages = fl_getSectorSize(sector) / PAGE_BYTES;
            for (unsigned i = 0; i < pages; i++) {
                if (fl_readPage(base + i * PAGE_BYTES, page)) continue;
                record_t r; memcpy(&r, page, sizeof(r));
                if (r.magic != RECORD_MAGIC || r.commit != COMMIT_MAGIC || r.sequence <= s_sequence ||
                    r.checksum != ConfigStore_Checksum(&r, offsetof(record_t, checksum))) continue;
                *config = r.config; s_sequence = r.sequence; s_sector = s_committed_sector = sector;
                s_next_page = i + 1u; found = 1;
            }
        }
        if (!found && !fl_readPage(fl_getSectorAddress(count - 1), page)) {
            legacy_t old; memcpy(&old, page, sizeof(old));
            if (old.magic == LEGACY_MAGIC && old.checksum == ConfigStore_Checksum(&old, sizeof(old) - 4u)) {
                memset(config, 0, sizeof(*config)); memcpy(config, old.audio, sizeof(old.audio));
                s_committed_sector = count - 1; found = 1;
            }
        }
    }
    flash_cmd_disable_ports();
    return found;
}

int ConfigStore_Save(const device_config_t *config)
{
    if (DFU_mode_active || !flash_cmd_enable_ports()) return -1;
    int rc = -1, count = fl_getNumSectors(), sector = s_sector;
    unsigned next = s_next_page;
    unsigned char page[PAGE_BYTES] __attribute__((aligned(4)));
    unsigned char verify[PAGE_BYTES] __attribute__((aligned(4)));
    if (count <= 4) goto done;
    if (sector < count - 2 || sector >= count) { sector = count - 2; next = 0; }
    unsigned pages = fl_getSectorSize(sector) / PAGE_BYTES;
    while (next < pages) {
        if (fl_readPage(fl_getSectorAddress(sector) + next * PAGE_BYTES, page)) goto done;
        unsigned erased = 1;
        for (unsigned i = 0; i < PAGE_BYTES; i++) if (page[i] != 0xFFu) { erased = 0; break; }
        if (erased) break;
        next++;
    }
    if (next >= pages) {
        int other = sector == count - 2 ? count - 1 : count - 2;
        if (other != s_committed_sector) sector = other;
        next = 0;
    }
    if (!next && fl_eraseSector(sector)) goto done;
    record_t r;
    memset(&r, 0, sizeof(r)); r.magic = RECORD_MAGIC; r.sequence = s_sequence + 1u;
    r.config = *config; r.commit = COMMIT_MAGIC;
    r.checksum = ConfigStore_Checksum(&r, offsetof(record_t, checksum));
    memset(page, 0xFF, sizeof(page)); memcpy(page, &r, sizeof(r));
    unsigned address = fl_getSectorAddress(sector) + next * PAGE_BYTES;
    if (!fl_writePage(address, page) && !fl_readPage(address, verify) && !memcmp(page, verify, PAGE_BYTES)) {
        s_sequence = r.sequence; s_committed_sector = sector; rc = 0;
    }
    s_sector = sector; s_next_page = next + 1u;
done:
    flash_cmd_disable_ports();
    return rc;
}
