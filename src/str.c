/* str.c -- STR/MDEC player
 *
 * The original player kept only two complete STR frames. That is fragile on
 * real hardware: CD reading continues while the CPU is doing VLC decoding and
 * the MDEC is doing IDCT, so two frame buffers can be overrun by interleaved
 * XA sectors before the main loop gets back to them.
 *
 * This version keeps the VN engine on PSn00bSDK, but uses the streaming
 * architecture of Sony's Psy-Q MOVIE/TUTO3 example as the reference:
 *
 *   CD -> sector ring -> frame assembler -> VLC -> MDEC -> VRAM
 *
 * The important difference is that the Psy-Q St* streaming library itself is
 * not ABI-compatible with PSn00bSDK, so we implement the small part we need
 * here instead of linking Psy-Q libraries into a PSn00bSDK program.
 *
 * XA sectors remain in the CD stream and are consumed by the CD/SPU hardware;
 * only MDEC sectors are copied into the frame bitstream. A large ring gives
 * the CPU several frames of slack while the drive continues streaming.
 */

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <psxpress.h>
#include "vn.h"

#define BLOCK_SIZE          16
#define STR_DATA_SIZE       2016
#define STR_SECTOR_SIZE     2048
#define STR_RING_SECTORS    128       /* 256 KiB of CD user data */
#define STR_BS_WORDS        0x8000    /* 128 KiB */
#define STR_MDEC_WORDS      0x8000    /* 128 KiB */
#define STR_SPIN_LIMIT      18000000
#define STR_PAD_EVERY       8192

/* STR header in the 2048-byte CD user-data area. */
typedef struct {
    uint16_t magic;            /* 0x0160 */
    uint16_t type;             /* 0x8001 = MDEC */
    uint16_t sector_id;        /* chunk number within frame */
    uint16_t sector_count;     /* number of MDEC chunks in frame */
    uint32_t frame_id;
    uint32_t bs_length;
    uint16_t width, height;
    uint8_t  bs_header[8];
    uint32_t reserved;
} STR_Header;

/* The CD callback writes whole 2048-byte sectors into this ring. */
static uint8_t sector_ring[STR_RING_SECTORS][STR_SECTOR_SIZE]
    __attribute__((aligned(4)));

/* One contiguous frame bitstream and one MDEC input/output buffer. Once VLC
   has finished, bs_data can be reused while MDEC consumes mdec_data. */
static uint32_t bs_data[STR_BS_WORDS] __attribute__((aligned(4)));
static uint32_t mdec_data[STR_MDEC_WORDS] __attribute__((aligned(4)));
static uint32_t slices[2][BLOCK_SIZE * SCR_H / 2]
    __attribute__((aligned(4)));
static uint8_t sector_copy[STR_SECTOR_SIZE] __attribute__((aligned(4)));

static volatile uint32_t ring_head;
static volatile uint32_t ring_tail;
static volatile int sector_pending;
static volatile int cd_error;
static volatile int cd_other;
static volatile int cd_callbacks;
static volatile int ring_overrun;

static int frame_no;
static int last_frame_id;      /* для распознавания начала следующего файла */
static int dropped_frames;
static int frame_width;
static int frame_height;
static int slice_id;
static int active_fb;
static int frame_decode_started;
static RECT slice_pos;
static char str_report[96];

/* Frame assembler state. It is deliberately kept outside the interrupt
   handler: copying/reordering 2016-byte chunks is too much work for IRQ time. */
static int asm_active;
static volatile uint32_t stream_epoch;
static uint32_t asm_frame_id;
static uint16_t asm_sector_count;
static uint16_t asm_seen;
static uint32_t asm_bs_length;
static uint16_t asm_width, asm_height;
static uint8_t asm_seen_map[256];
static uint32_t parse_pos;

static void mdec_dma_handler(void);

/* ------------------------------------------------------------------------- */
/* CD streaming                                                             */

static unsigned ring_index(uint32_t seq)
{
    return (unsigned) (seq & (STR_RING_SECTORS - 1));
}

static uint32_t ring_used(void)
{
    return ring_tail - ring_head;
}

static void cd_sector_handler(void)
{
    /* This runs in the CD IRQ context. Keep it short: transfer exactly one
       sector and update the ring indices. Frame parsing stays in main(). */
    if (ring_used() >= STR_RING_SECTORS) {
        /* We have fallen behind. Discard the oldest sector and let the frame
           assembler resynchronize on the next sector_id == 0. This is the
           same basic policy as a streaming player dropping late frames rather
           than blocking the CD indefinitely. */
        ring_head++;
        ring_overrun++;
        stream_epoch++;
    }

    CdGetSector(sector_ring[ring_index(ring_tail)], STR_SECTOR_SIZE / 4);
    ring_tail++;
    cd_callbacks++;
}

static void cd_event_handler(CdlIntrResult event, uint8_t *payload)
{
    (void) payload;

    if (event != CdlDataReady) {
        if (event == CdlDiskError) {
            cd_error = 1;
        } else {
            cd_other++;
        }
        return;
    }

    /* CD-ROM and MDEC output use DMA resources that cannot be allowed to run
       simultaneously. This is the same hardware workaround used by the
       PSn00bSDK STR reference example. */
    if (DecDCTinSync(1))
        sector_pending = 1;
    else
        cd_sector_handler();
}

static void init_stream(void)
{
    EnterCriticalSection();
    DMACallback(1, &mdec_dma_handler);
    CdReadyCallback(&cd_event_handler);
    ExitCriticalSection();

    /* Version-3 STR is supported by the current encoder. The scratchpad table
       is the fast Psy-Q/PSn00b-style VLC path and avoids rebuilding a 34 KiB
       table for every frame. */
    DecDCTvlcCopyTableV3((VLC_TableV3 *) 0x1f800000);
}

static void stop_stream(void)
{
    CdControlB(CdlPause, 0, 0);

    EnterCriticalSection();
    CdReadyCallback(0);
    DMACallback(1, 0);
    ExitCriticalSection();

    sector_pending = 0;
}

static void start_stream(CdlFILE *file)
{
    uint8_t mode = CdlModeRT | CdlModeSpeed;

    ring_head = ring_tail = 0;
    sector_pending = 0;
    cd_error = 0;
    cd_other = 0;
    cd_callbacks = 0;
    ring_overrun = 0;

    frame_no = -1;
    last_frame_id = -1;
    dropped_frames = 0;
    parse_pos = 0;
    asm_active = 0;
    memset(asm_seen_map, 0, sizeof(asm_seen_map));

    CdSync(0, 0);
    CdControl(CdlSetmode, &mode, 0);
    CdControl(CdlReadS, (const uint8_t *) &file->pos, 0);
}

/* ------------------------------------------------------------------------- */
/* Frame assembler                                                          */

static int frame_start(const STR_Header *h)
{
    if (h->sector_id != 0 || h->sector_count == 0 || h->sector_count > 255)
        return 0;
    if (h->bs_length == 0 || h->bs_length > STR_BS_WORDS * 4)
        return 0;
    if (h->width == 0 || h->height == 0 ||
        h->width > 640 || h->height > 512)
        return 0;

    asm_active = 1;
    asm_frame_id = h->frame_id;
    asm_sector_count = h->sector_count;
    asm_seen = 0;
    asm_bs_length = h->bs_length;
    asm_width = h->width;
    asm_height = h->height;
    memset(asm_seen_map, 0, sizeof(asm_seen_map));
    return 1;
}

/* Copy one MDEC sector into its proper position. The bitstream chunks are
   2016 bytes long, exactly as in Sony's STR format. */
static void frame_add_sector(const STR_Header *h, const uint8_t *sector)
{
    uint32_t offset;

    if (h->sector_id >= h->sector_count || h->sector_id >= 256)
        return;
    if (asm_seen_map[h->sector_id])
        return;

    offset = (uint32_t) h->sector_id * STR_DATA_SIZE;
    if (offset >= STR_BS_WORDS * 4)
        return;

    if (offset + STR_DATA_SIZE > STR_BS_WORDS * 4) {
        /* Last sector can be shorter logically; copying the unused tail is
           harmless as long as the declared bs_length stays authoritative. */
        uint32_t n = STR_BS_WORDS * 4 - offset;
        memcpy((uint8_t *)bs_data + offset, sector + 32, n);
    } else {
        memcpy((uint8_t *)bs_data + offset, sector + 32, STR_DATA_SIZE);
    }

    asm_seen_map[h->sector_id] = 1;
    asm_seen++;
}

/* Try to obtain one complete frame from the ring. Return 1 when ready, 0
   when more sectors are needed, and -1 when the stream ended or failed. */
static int stream_next_frame(void)
{
    /* If the producer had to discard old sectors, parse_pos may point into
       data that no longer exists. Resynchronize at the new head and wait for
       a fresh frame boundary. */
    if (parse_pos < ring_head) {
        parse_pos = ring_head;
        asm_active = 0;
        dropped_frames++;
    }

    while (ring_used()) {
        uint8_t *sector;
        STR_Header h;
        int is_mdec;

        /* The producer is an IRQ handler. Keep the slot stable while copying
           it to a private scratch sector; frame assembly then happens outside
           the critical section so a first-sector header can initialize the
           assembler before its payload is copied. */
        EnterCriticalSection();
        sector = sector_ring[ring_index(parse_pos)];
        memcpy(sector_copy, sector, STR_SECTOR_SIZE);
        parse_pos++;
        ring_head++;
        ExitCriticalSection();

        memcpy(&h, sector_copy, sizeof(h));
        is_mdec = (h.magic == 0x0160 && h.type == 0x8001);

        if (!is_mdec) {
            /* After the last frame, the next file/sector will normally not
               have STR magic. That is our end-of-stream marker. If we are
               midway through a frame, treat it as a broken stream instead. */
            if (asm_active) {
                asm_active = 0;
                return -1;
            }
            return -1;
        }

        /* Ролики лежат на диске вплотную, и после последнего кадра привод
           сразу попадает в секторы СЛЕДУЮЩЕГО файла: сигнатура там законная,
           тип верный, и по ней конец не определить. Отличает их номер кадра —
           он начинается заново, то есть перестаёт расти. */
        if ((int) h.frame_id < last_frame_id) {
            asm_active = 0;
            return -1;
        }

        if (!asm_active) {
            if (!frame_start(&h))
                continue;
        } else if (h.frame_id != asm_frame_id) {
            /* A new frame arrived before all chunks of the previous frame.
               Drop the incomplete one and resynchronize here. */
            dropped_frames++;
            asm_active = 0;
            if (!frame_start(&h))
                continue;
        }

        frame_add_sector(&h, sector_copy);

        if (asm_active && asm_seen == asm_sector_count) {
            /* Header's bs_length is the authoritative bitstream length. */
            frame_no = (int) asm_frame_id;
            last_frame_id = (int) asm_frame_id;
            frame_width = (asm_width + 15) & ~15;
            frame_height = (asm_height + 15) & ~15;
            asm_active = 0;
            return 1;
        }
    }

    if (cd_error && !ring_used())
        return -1;
    return 0;
}

/* Wait for a frame, allowing the CD IRQ to keep filling the ring. */
static int wait_frame(int skippable)
{
    int spin = 0;

    for (;;) {
        int r = stream_next_frame();
        if (r != 0)
            return r;

        if (++spin > STR_SPIN_LIMIT)
            return -1;

        if (skippable && ((spin & (STR_PAD_EVERY - 1)) == 0) &&
            (pad_pressed() & (PAD_CROSS | PAD_START)))
            return -2;

        /* Give the exception handler a chance to service CD/MDEC IRQs. */
        __asm__ volatile("nop");
    }
}

/* ------------------------------------------------------------------------- */
/* MDEC output                                                              */

static void mdec_dma_handler(void)
{
    if (sector_pending) {
        cd_sector_handler();
        sector_pending = 0;
    }

    LoadImage(&slice_pos, slices[slice_id]);
    slice_id ^= 1;

    slice_pos.x += BLOCK_SIZE;
    if (slice_pos.x < frame_width)
        DecDCTout(slices[slice_id], BLOCK_SIZE * slice_pos.h / 2);
}

static int decode_frame(void)
{
    VLC_Context vlc;
    int r;

    r = DecDCTvlcStart(&vlc, mdec_data, STR_MDEC_WORDS, bs_data);
    if (r != 0)
        return r;

    /* Do not touch the MDEC input/output buffers until the previous frame has
       completely left the MDEC. VLC decoding above is CPU-side and can happen
       while the previous frame is being displayed/decoded. */
    DecDCTinSync(0);
    DecDCToutSync(0);

    DecDCTin(mdec_data, DECDCT_MODE_16BPP);

    {
        int x_off = (SCR_W - frame_width) / 2;
        int y_off = (SCR_H - frame_height) / 2;
        if (x_off < 0) x_off = 0;
        if (y_off < 0) y_off = 0;

        slice_pos.x = x_off;
        slice_pos.y = (active_fb ? SCR_H : 0) + y_off;
        slice_pos.w = BLOCK_SIZE;
        slice_pos.h = frame_height;
        slice_id = 0;

        DecDCTout(slices[slice_id], BLOCK_SIZE * slice_pos.h / 2);
    }

    frame_decode_started = 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                               */

int str_play(const char *path, int nframes, int ticks, int skippable)
{
    CdlFILE file;
    int skipped = 0;
    int shown = 0;
    int decode_errors = 0;
    int target;

    if (ticks < 1)
        ticks = 4;

    if (!CdSearchFile(&file, (char *) path)) {
        LOG("str: file not found %s\n", path);
        return -1;
    }

    voice_stop();
    voice_mode_dirty();   /* видео тоже перенастраивает привод */
    gfx_clear_all();
    DecDCTReset(0);
    init_stream();
    start_stream(&file);

    /* The first frame is allowed to fill the ring before decoding starts. */
    target = VSync(-1);

    while (1) {
        int r = wait_frame(skippable);
        if (r == -2) {
            skipped = 1;
            break;
        }
        if (r < 0)
            break;

        if (skippable && (pad_pressed() & (PAD_CROSS | PAD_START))) {
            skipped = 1;
            break;
        }

        if (decode_frame() != 0) {
            decode_errors++;
            /* A single malformed/too-large frame should not kill the whole
               movie. Continue consuming the stream and try the next frame. */
            if (decode_errors > 30)
                break;
            continue;
        }

        /* The MDEC is still filling active_fb. Wait until its last slice has
           reached VRAM before exposing that buffer. The CD IRQ continues to
           fill the sector ring while this wait is in progress. */
        DecDCToutSync(0);
        frame_decode_started = 0;

        if (target < VSync(-1))
            target = VSync(-1);
        while (VSync(-1) < target)
            VSync(0);

        gfx_show_raw(active_fb);
        active_fb ^= 1;
        target += ticks;
        shown++;

        /* Вторая страховка: компилятор посчитал кадры по самому файлу,
           так что дальше этого числа читать нечего в принципе. */
        if (nframes > 0 && shown >= nframes)
            break;
    }

    stop_stream();
    DecDCTReset(0);
    DecDCTvlcCopyTableV3(0);

    sprintf(str_report,
            "f%d drop%d decerr%d over%d cb%d cd%d other%d %s",
            shown, dropped_frames, decode_errors, ring_overrun,
            cd_callbacks, cd_error, cd_other,
            skipped ? "skip" : (cd_error ? "CDERR" : "end"));
    LOG("str: %s\n", str_report);

    gfx_restore_display();
    return skipped;
}

const char *str_last_report(void)
{
    return str_report;
}
