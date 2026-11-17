/*
    32-bit-boundary crossing test for minimp4.

    Constructs MP4 files whose media timeline crosses the 2^32 boundary in
    three dimensions at once:
      - total duration: 6 samples x 1e9 ticks = 6e9 > 2^32 (timescale 1000,
        so the movie-timescale duration crosses 2^32 too) -> version-1
        mvhd/tkhd/mdhd boxes with 64-bit durations;
      - media data: 6 samples x 800 MB = 4.8 GB > 4 GB -> 64-bit 'largesize'
        mdat header and co64 chunk offsets;
      - per-sample timestamps: last sample dts = 5e9 > 2^32.

    The big media writes are sparsified in write_callback (seek + 1 byte),
    so the 4.8 GB file costs almost no disk or time.

    Also verifies that a small file still produces version-0 boxes
    (no regression for the common case).
*/
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include <inttypes.h>

#define NSAMPLES     7                   /* 7 x 800 MB: last chunk offset 4.8 GB > 2^32 */
#define SAMPLE_BYTES 800000000           /* 800 MB; total 4.8 GB > 2^32 */
#define SAMPLE_DUR   1000000000          /* 1e9; total 6e9 > 2^32 */
#define TIME_SCALE   1000                /* movie duration == media duration */

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   : "); } else { printf("FAIL : "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

/* Sparse-file writer: media payloads > 1 MB are not actually written. */
static int write_cb(int64_t offset, const void *buffer, size_t size, void *token)
{
    FILE *f = (FILE*)token;
    if (fseek(f, (long)offset, SEEK_SET))
        return 1;
    if (size > 1024*1024)
    {
        if (fseek(f, (long)size - 1, SEEK_CUR))
            return 1;
        return fputc(0, f) == EOF;
    }
    return fwrite(buffer, 1, size, f) != size;
}

static int read_cb(int64_t offset, void *buffer, size_t size, void *token)
{
    FILE *f = (FILE*)token;
    if (fseek(f, (long)offset, SEEK_SET))
        return 1;
    return fread(buffer, 1, size, f) != size;
}

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Find a 4CC inside the moov box and return the version byte of the fullbox. */
static int box_version(const unsigned char *moov, size_t moov_bytes, const char *fourcc)
{
    size_t i;
    for (i = 0; i + 8 <= moov_bytes; i++)
    {
        if (!memcmp(moov + i, fourcc, 4))
            return moov[i + 4]; /* version byte right after the 4CC */
    }
    return -1;
}

static void mux_track(MP4E_mux_t *mux, int nsamples, int sample_bytes, int sample_dur)
{
    MP4E_track_t tr;
    unsigned char *buf;
    int i, track;
    memset(&tr, 0, sizeof(tr));
    tr.track_media_kind = e_video;
    tr.language[0] = 'u'; tr.language[1] = 'n'; tr.language[2] = 'd';
    tr.object_type_indication = MP4_OBJECT_TYPE_AVC;
    tr.time_scale = TIME_SCALE;
    tr.default_duration = 0;
    tr.u.v.width = 16;
    tr.u.v.height = 16;
    track = MP4E_add_track(mux, &tr);

    buf = (unsigned char*)malloc((size_t)sample_bytes);
    memset(buf, 0xab, (size_t)sample_bytes);
    for (i = 0; i < nsamples; i++)
    {
        if (MP4E_STATUS_OK != MP4E_put_sample(mux, track, buf, sample_bytes, sample_dur,
                                              i == 0 ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT))
        {
            printf("FAIL : MP4E_put_sample %d\n", i);
            failures++;
            break;
        }
    }
    free(buf);
}

int main(void)
{
    const uint64_t expect_media_dur = (uint64_t)NSAMPLES * SAMPLE_DUR;   /* 6e9 */
    const uint64_t expect_mdat_payload = (uint64_t)NSAMPLES * SAMPLE_BYTES;
    FILE *f;
    MP4E_mux_t *mux;
    int64_t file_size;
    unsigned char hdr[16];
    uint64_t largesize, mdat_end, moov_off;
    MP4D_demux_t mp4;
    MP4D_track_t *tr;
    int i;

    /* ---------------- mux the boundary-crossing file ---------------- */
    f = fopen("boundary64.mp4", "wb+");
    if (!f) { printf("cannot create file\n"); return 1; }
    mux = MP4E_open(0, 0, f, write_cb);
    mux_track(mux, NSAMPLES, SAMPLE_BYTES, SAMPLE_DUR);
    if (MP4E_STATUS_OK != MP4E_close(mux)) { printf("FAIL : MP4E_close\n"); failures++; }
    fflush(f);
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);

    /* ---------------- mdat range: 64-bit largesize header ------------ */
    fseek(f, 24, SEEK_SET);              /* right after ftyp */
    if (fread(hdr, 1, 16, f) != 16) { printf("cannot read mdat header\n"); return 1; }
    CHECK(rd32(hdr) == 1, "mdat uses 64-bit largesize encoding (size==1), got %u", rd32(hdr));
    CHECK(!memcmp(hdr + 4, "mdat", 4), "mdat fourcc at offset 28");
    largesize = ((uint64_t)rd32(hdr + 8) << 32) | rd32(hdr + 12);
    CHECK(largesize == 16 + expect_mdat_payload,
          "mdat 64-bit size %" PRIu64 " == header(16) + payload(%" PRIu64 ")", largesize, expect_mdat_payload);
    mdat_end = 24 + largesize;
    CHECK(mdat_end == (uint64_t)file_size - 0 || mdat_end <= (uint64_t)file_size,
          "mdat range [24, %" PRIu64 ") within file size %" PRId64, mdat_end, file_size);
    moov_off = mdat_end;

    /* ---------------- moov box versions (raw scan) ------------------- */
    {
        unsigned char moov_hdr[8];
        uint64_t moov_size;
        unsigned char *moov;
        fseek(f, (long)moov_off, SEEK_SET);
        if (fread(moov_hdr, 1, 8, f) != 8) { printf("cannot read moov header\n"); return 1; }
        moov_size = rd32(moov_hdr);
        CHECK(!memcmp(moov_hdr + 4, "moov", 4), "moov follows mdat at offset %" PRIu64, moov_off);
        moov = (unsigned char*)malloc((size_t)moov_size);
        fseek(f, (long)moov_off, SEEK_SET);
        if (fread(moov, 1, (size_t)moov_size, f) != moov_size) { printf("cannot read moov\n"); return 1; }
        CHECK(box_version(moov, (size_t)moov_size, "mvhd") == 1, "mvhd is version 1 (64-bit duration)");
        CHECK(box_version(moov, (size_t)moov_size, "tkhd") == 1, "tkhd is version 1 (64-bit duration)");
        CHECK(box_version(moov, (size_t)moov_size, "mdhd") == 1, "mdhd is version 1 (64-bit duration)");
        free(moov);
    }

    /* ---------------- demux: probe duration/timestamps/offsets ------- */
    if (!MP4D_open(&mp4, read_cb, f, file_size)) { printf("FAIL : MP4D_open\n"); return 1; }
    CHECK(mp4.timescale == TIME_SCALE, "movie timescale %u", mp4.timescale);
    {
        uint64_t mdur = ((uint64_t)mp4.duration_hi << 32) | mp4.duration_lo;
        CHECK(mdur == expect_media_dur, "movie duration %" PRIu64 " == %" PRIu64 " (no truncation)",
              mdur, expect_media_dur);
    }
    CHECK(mp4.track_count == 1, "track count %u", mp4.track_count);
    tr = mp4.track;
    CHECK(tr->timescale == TIME_SCALE, "track timescale %u", tr->timescale);
    {
        uint64_t tdur = ((uint64_t)tr->duration_hi << 32) | tr->duration_lo;
        CHECK(tdur == expect_media_dur, "track duration %" PRIu64 " == %" PRIu64 " (no truncation)",
              tdur, expect_media_dur);
    }
    CHECK(tr->sample_count == NSAMPLES, "sample count %u", tr->sample_count);
    CHECK(tr->chunk_count == NSAMPLES && tr->chunk_offset[NSAMPLES - 1] > 0xffffffffULL,
          "co64 chunk offsets used, last chunk offset %" PRIu64, (uint64_t)tr->chunk_offset[NSAMPLES - 1]);

    for (i = 0; i < NSAMPLES; i++)
    {
        unsigned frame_bytes = 0, dur = 0;
        uint64_t dts = 0, pts = 0;
        int is_sync = 0;
        uint64_t expect_ofs = 24 + 16 + (uint64_t)i * SAMPLE_BYTES;
        MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, 0, i, &frame_bytes, &dts, &pts, &dur, &is_sync);
        CHECK(dts == (uint64_t)i * SAMPLE_DUR, "sample %d dts %" PRIu64 " (64-bit, no wrap)", i, dts);
        CHECK(pts == (uint64_t)i * SAMPLE_DUR, "sample %d pts %" PRIu64, i, pts);
        CHECK(dur == SAMPLE_DUR, "sample %d duration %u", i, dur);
        CHECK(frame_bytes == SAMPLE_BYTES, "sample %d bytes %u", i, frame_bytes);
        CHECK((uint64_t)ofs == expect_ofs, "sample %d offset %" PRIu64 " == %" PRIu64,
              i, (uint64_t)ofs, expect_ofs);
        CHECK((uint64_t)ofs + frame_bytes <= mdat_end, "sample %d [%#" PRIx64 ", %#" PRIx64 ") inside mdat",
              i, (uint64_t)ofs, (uint64_t)ofs + frame_bytes);
    }
    MP4D_close(&mp4);
    fclose(f);
    remove("boundary64.mp4");

    /* ---------------- small file: version-0 boxes must be kept ------- */
    f = fopen("small.mp4", "wb+");
    if (!f) { printf("cannot create file\n"); return 1; }
    mux = MP4E_open(0, 0, f, write_cb);
    mux_track(mux, 3, 100, 3000);
    MP4E_close(mux);
    fflush(f);
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    {
        unsigned char *whole = (unsigned char*)malloc((size_t)file_size);
        uint64_t tdur;
        fseek(f, 0, SEEK_SET);
        if (fread(whole, 1, (size_t)file_size, f) != (size_t)file_size) { printf("read fail\n"); return 1; }
        CHECK(box_version(whole, (size_t)file_size, "mvhd") == 0, "small file: mvhd stays version 0");
        CHECK(box_version(whole, (size_t)file_size, "tkhd") == 0, "small file: tkhd stays version 0");
        CHECK(box_version(whole, (size_t)file_size, "mdhd") == 0, "small file: mdhd stays version 0");
        free(whole);
        if (!MP4D_open(&mp4, read_cb, f, file_size)) { printf("FAIL : MP4D_open small\n"); return 1; }
        tdur = ((uint64_t)mp4.track[0].duration_hi << 32) | mp4.track[0].duration_lo;
        CHECK(tdur == 9000, "small file: track duration %" PRIu64 " == 9000", tdur);
        {
            unsigned frame_bytes = 0, dur = 0;
            uint64_t dts = 0;
            MP4D_frame_offset(&mp4, 0, 2, &frame_bytes, &dts, NULL, &dur, NULL);
            CHECK(dts == 6000 && dur == 3000, "small file: sample 2 dts %" PRIu64 " dur %u", dts, dur);
        }
        MP4D_close(&mp4);
    }
    fclose(f);
    remove("small.mp4");

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall boundary checks passed\n", failures);
    return failures != 0;
}
