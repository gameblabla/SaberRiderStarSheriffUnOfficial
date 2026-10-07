/* Model the firmware boundary, not the reader implementation. In particular,
 * CMOK precedes EHST, WAIT/REJECT never complete an operation, and a short
 * Get/Delete still deletes the whole requested batch at DataEnd. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct TestFile TestFile;
struct TestFile {
    void *cookie;
    unsigned flags;
    int fd;
    size_t (*read)(TestFile *, unsigned char *, size_t);
    size_t (*write)(TestFile *, const unsigned char *, size_t);
    off_t (*seek)(TestFile *, off_t, int);
    int (*close)(TestFile *);
};
#define F_EOF 16
#define F_ERR 32
#define FILE TestFile
#define fopen test_fopen
static uint16_t reg_read(unsigned off);
static void reg_write(unsigned off, uint16_t value);
#define CD_REG_READ(off) reg_read(off)
#define CD_REG_WRITE(off, value) reg_write(off, value)
#include "../../../src/platform/saturn/cd_sat.c"
#undef FILE
#undef fopen

static uint32_t now, next_sector, next_periodic, finish_host, finish_selector;
static uint32_t reply_at, data_at, reply_delay, data_delay;
static bool reply_pending, data_pending;
static uint16_t hirq, cmd[4], reply[4];
static unsigned irq_mask, plays, pauses, queries, ends, commands, drops;
static unsigned wait_count, reject_count, short_count, gap_count;
static bool stalled, always_wait, no_media, error_once, xfer, selector_connected;
static bool host_finishing, selector_finishing;
static uint8_t status;
static uint32_t queue[16], count, drive_fad, drive_end, xfer_words, xfer_count;
static uint32_t command_times[4096];
static unsigned words_read;
static bool short_xfer;

static bool due(uint32_t when) { return (int32_t)(now - when) >= 0; }
static void events(void)
{
    if (reply_pending && due(reply_at)) { hirq |= HIRQ_CMOK; reply_pending = false; }
    if (data_pending && due(data_at)) { hirq |= HIRQ_DRDY; data_pending = false; }
    if (host_finishing && due(finish_host)) { hirq |= HIRQ_EHST; host_finishing = false; }
    if (selector_finishing && due(finish_selector)) { hirq |= HIRQ_ESEL; selector_finishing = false; }
    if (status == 3 && selector_connected && count < 16 && drive_fad < drive_end && due(next_sector)) {
        queue[count++] = drive_fad++;
        next_sector = now + 6700;
        hirq |= HIRQ_CSCT;
        if (drive_fad == drive_end) { status = 1; hirq |= HIRQ_PEND; }
    }
    if (!command_inflight && due(next_periodic)) {
        reply[0] = (uint16_t)((0x20 | status) << 8);
        reply[1] = 0; reply[2] = 0; reply[3] = 0;
        hirq |= HIRQ_SCDQ;
        next_periodic = now + 16700;
    }
}
uint32_t sat_timer_us(void) { now += 25; events(); return now; }
uint8_t cpu_intc_mask_get(void) { return irq_mask; }
void cpu_intc_mask_set(uint8_t mask) { irq_mask = mask; }

static uint16_t payload(uint32_t fad, unsigned word) { return (uint16_t)(fad * 13 + word); }
static uint16_t reg_read(unsigned off)
{
    events();
    if (off == 8) return hirq;
    if (off >= 0x18) return reply[(off - 0x18) / 4];
    assert(off == 0 && xfer && (hirq & HIRQ_DRDY));
    unsigned i = words_read++;
    assert(i < xfer_count * 1024);
    return short_xfer ? 0xDEAD : payload(queue[i / 1024], i % 1024);
}
static void execute(void)
{
    assert(irq_mask == 15);
    if (commands) assert((uint32_t)(now - command_times[commands - 1]) >= 300);
    assert(commands < 4096);
    command_times[commands++] = now;
    if (commands > 30) assert((uint32_t)(now - command_times[commands - 31]) >= 6700);
    if (commands > 60) assert((uint32_t)(now - command_times[commands - 61]) >= 16700);
    unsigned op = cmd[0] >> 8;
    reply[0] = status << 8; reply[1] = reply[2] = reply[3] = 0;
    hirq |= HIRQ_CMOK;
    next_periodic = now + 16700;
    if (op == 0x63 && (wait_count || always_wait)) {
        if (wait_count) wait_count--;
        reply[0] |= 0x8000; return;
    }
    if (op == 0x63 && reject_count) { reject_count--; reply[0] = 0xFF00; return; }
    if (op == 0x30 || op == 0x40 || op == 0x44 || op == 0x46 || op == 0x48) {
        assert(!selector_finishing && !xfer && !host_finishing);
        selector_finishing = true; finish_selector = now + 700;
    }
    switch (op) {
    case 0x30: selector_connected = cmd[2] != 0xFF00; break;
    case 0x40: case 0x44: case 0x46: assert(!selector_connected); break;
    case 0x48: assert(!selector_connected); count = 0; break;
    case 0x10:
        assert(!xfer && !host_finishing && !selector_finishing);
        plays++;
        drive_fad = ((cmd[0] & 127) << 16) | cmd[1];
        drive_end = drive_fad + ((cmd[2] & 127) << 16 | cmd[3]);
        status = no_media ? 6 : error_once && plays == 1 ? 9 : 3;
        if (gap_count) { drive_fad++; gap_count--; }
        next_sector = now + (stalled && plays == 1 ? 100000000 : 30000);
        reply[0] = status << 8;
        break;
    case 0x11:
        assert(!xfer && !host_finishing);
        pauses++; status = 1;
        break;
    case 0x51: assert(!host_finishing); queries++; reply[3] = count; break;
    case 0x54:
        assert(!host_finishing && cmd[1] < count);
        reply[0] |= queue[cmd[1]] >> 16; reply[1] = (uint16_t)queue[cmd[1]];
        break;
    case 0x62:
        assert(!host_finishing && count >= cmd[3]); drops++;
        count -= cmd[3]; memmove(queue, queue + cmd[3], count * sizeof *queue);
        host_finishing = true; finish_host = now + 2300;
        break;
    case 0x63:
        assert(!host_finishing && !xfer && cmd[3] <= count);
        xfer = true; xfer_count = cmd[3]; words_read = 0;
        short_xfer = short_count != 0;
        if (short_count) short_count--;
        xfer_words = xfer_count * 1024 - (short_xfer ? 1 : 0);
        hirq |= HIRQ_DRDY;
        break;
    case 0x06:
        assert(xfer && (hirq & HIRQ_DRDY)); ends++; xfer = false;
        reply[0] |= xfer_words >> 16; reply[1] = (uint16_t)xfer_words;
        count -= xfer_count; memmove(queue, queue + xfer_count, count * sizeof *queue);
        host_finishing = true; finish_host = now + 2300;
        break;
    case 0: break;
    default: assert(!"unexpected command");
    }
}
static void reg_write(unsigned off, uint16_t value)
{
    if (off == 8) { hirq &= value; return; }
    assert(off >= 0x18 && off <= 0x24);
    cmd[(off - 0x18) / 4] = value;
    if (off == 0x24) {
        execute();
        if (reply_delay) { hirq &= ~HIRQ_CMOK; reply_pending = true; reply_at = now + reply_delay; }
        if ((cmd[0] >> 8) == 0x63 && xfer && data_delay) { hirq &= ~HIRQ_DRDY; data_pending = true; data_at = now + data_delay; }
    }
}
static void reset(void)
{
    /* Advance between cases without resetting the production pacing timestamp. */
    now += 10000000;
    next_periodic = now + 16700;
    reply_delay = data_delay = 0; reply_pending = data_pending = false;
    hirq = HIRQ_CMOK | HIRQ_EHST | HIRQ_ESEL;
    status = 1;
    plays = pauses = queries = ends = commands = drops = 0;
    count = drive_fad = drive_end = words_read = 0;
    wait_count = reject_count = short_count = gap_count = 0;
    stalled = always_wait = no_media = error_once = xfer = false;
    selector_connected = host_finishing = selector_finishing = false;
    pending_flags = 0; drive_status = 1; transfer_active = command_inflight = false;
    st_on = data_busy = false; ra_count = 0;
    memset(&cdda, 0, sizeof cdda);
    memset(reply, 0, sizeof reply);
    fails_total = retries_total = stale_total = waits_total = 0;
    read_name = "TEST.PCK";
}
static uint16_t output[8 * 1024];
static void check_read(void)
{
    assert(read_sectors(1000, output, 8, 1008));
    assert(!data_busy && !xfer && !host_finishing && !transfer_active && !fails_total);
    for (unsigned s = 0; s < 8; s++) for (unsigned w = 0; w < 1024; w++)
        assert(output[s * 1024 + w] == payload(1000 + s, w));
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    reset(); check_read(); assert(plays == 1 && !pauses && queries < 25);
    /* An aligned read inside a previous packet's read-ahead must not seek
     * backwards or issue any command: those sectors left the partition. */
    reset();
    ra_buf=(uint8_t *)output;ra_fad=1000;ra_count=8;
    for(unsigned i=0;i<8*1024;i++)output[i]=(uint16_t)i;
    CdFile cached={.name="TEST.PCK",.fad=1000,.fad_end=1008,.size=16384,.pos=2048};
    TestFile f={.cookie=&cached};uint16_t copy[1024];
    assert(cd_read(&f,(unsigned char *)copy,sizeof copy)==sizeof copy);
    assert(!commands&&!plays&&cached.pos==4096);
    for(unsigned i=0;i<1024;i++)assert(copy[i]==1024+i);
    reset(); reply_delay = 1500; data_delay = 3500; check_read();
    reset(); wait_count = 3; check_read(); assert(waits_total == 3 && !retries_total);
    reset(); reject_count = 1; check_read(); assert(retries_total == 1 && pauses == 1);
    reset(); short_count = 1; check_read(); assert(retries_total == 1 && pauses == 1);
    reset(); stalled = true;
    uint32_t start = now;
    check_read(); assert(retries_total == 1 && queries < 25 && now - start < 6000000);
    reset(); gap_count = 1; check_read(); assert(retries_total == 1 && !drops);
    reset(); error_once = true; check_read(); assert(retries_total == 1 && pauses == 1);
    reset(); data_delay = 2200000;
    assert(!read_sectors(1000, output, 8, 1008));
    assert(retries_total == 3 && ends == 4 && fails_total == 1 && !transfer_active);
    reset(); no_media = true;
    assert(!read_sectors(1000, output, 8, 1008) && fails_total == 1 && !retries_total);
    reset(); always_wait = true; start = now;
    assert(!read_sectors(1000, output, 8, 1008));
    assert(fails_total == 1 && retries_total == 3 && !ends && now - start < 10000000);
    reset(); now = UINT32_MAX - 40000; next_periodic = now + 16700;
    check_read();
    puts("PASS: cached aligned reads, command rate, delayed ESEL/EHST, WAIT, REJECT, short transfers, sector stalls/gaps, drive errors, absent media, retry limits, timer wrap");
}
