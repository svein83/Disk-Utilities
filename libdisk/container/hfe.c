/*
 * libdisk/container/hfe.c
 * 
 * Read/write HxC Floppy Emulator (HFE) images.
 * 
 * Written in 2015 by Keir Fraser
 */

#include <libdisk/util.h>
#include <private/disk.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* NB. Fields are little endian. */
struct disk_header {
    char sig[8];
    uint8_t formatrevision;
    uint8_t nr_tracks, nr_sides;
    uint8_t track_encoding;
    uint16_t bitrate; /* kB/s, approx */
    uint16_t rpm; /* unused, can be zero */
    uint8_t interface_mode;
    uint8_t rsvd; /* set to 1? */
    uint16_t track_list_offset;
    /* from here can write 0xff to end of block... */
    uint8_t write_allowed;
    uint8_t single_step;
    uint8_t t0s0_altencoding, t0s0_encoding;
    uint8_t t0s1_altencoding, t0s1_encoding;
};

/* track_encoding */
enum {
    ENC_ISOIBM_MFM,
    ENC_Amiga_MFM,
    ENC_ISOIBM_FM,
    ENC_Emu_FM,
    ENC_Unknown = 0xff
};

/* interface_mode */
enum {
    IFM_IBMPC_DD,
    IFM_IBMPC_HD,
    IFM_AtariST_DD,
    IFM_AtariST_HD,
    IFM_Amiga_DD,
    IFM_Amiga_HD,
    IFM_CPC_DD,
    IFM_GenericShugart_DD,
    IFM_IBMPC_ED,
    IFM_MSX2_DD,
    IFM_C64_DD,
    IFM_EmuShugart_DD,
    IFM_S950_DD,
    IFM_S950_HD,
    IFM_Disable = 0xfe
};

struct track_header {
    uint16_t offset;
    uint16_t len;
};

static void hfe_init(struct disk *d)
{
    _dsk_init(d, 166);
}

/* HFE dat bit order is LSB first. Switch to/from MSB first.  */
static void bit_reverse(uint8_t *block, unsigned int len)
{
    while (len--) {
        uint8_t x = *block, y, k;
        for (k = y = 0; k < 8; k++) {
            y <<= 1;
            y |= x&1;
            x >>= 1;
        }
        *block++ = y;
    }
}

static struct container *hfe_open(struct disk *d)
{
    struct disk_header dhdr;
    struct track_header thdr;
    struct disk_info *di;
    struct track_info *ti;
    unsigned int i, j, len;
    uint8_t *tbuf;

    lseek(d->fd, 0, SEEK_SET);

    read_exact(d->fd, &dhdr, sizeof(dhdr));
    if (strncmp(dhdr.sig, "HXCPICFE", sizeof(dhdr.sig))
        || (dhdr.formatrevision != 0))
        return NULL;

    dhdr.track_list_offset = le16toh(dhdr.track_list_offset);

    d->di = di = memalloc(sizeof(*di));
    di->nr_tracks = dhdr.nr_tracks * 2;
    di->track = memalloc(di->nr_tracks * sizeof(struct track_info));

    for (i = 0; i < dhdr.nr_tracks; i++) {
        lseek(d->fd, dhdr.track_list_offset*512 + i*4, SEEK_SET);
        read_exact(d->fd, &thdr, 4);
        thdr.offset = le16toh(thdr.offset);
        thdr.len = le16toh(thdr.len);

        /* Read into track buffer, padded up to 512-byte boundary. */
        len = (thdr.len + 0x1ff) & ~0x1ff;
        tbuf = memalloc(len);
        lseek(d->fd, thdr.offset*512, SEEK_SET);
        read_exact(d->fd, tbuf, len);
        bit_reverse(tbuf, len);

        /* Allocate internal track buffers and demux the data. */
        ti = &di->track[i*2];
        init_track_info(ti, TRKTYP_raw_dd);
        ti->len = len/2;
        ti->total_bits = thdr.len*4;
        ti->data_bitoff = 0;
        memcpy(&ti[1], &ti[0], sizeof(*ti));
        ti[0].dat = memalloc(ti->len);
        ti[1].dat = memalloc(ti->len);
        for (j = 0; j < len; j += 512) {
            memcpy(&ti[0].dat[j/2], &tbuf[j+  0], 256);
            memcpy(&ti[1].dat[j/2], &tbuf[j+256], 256);
        }

        memfree(tbuf);
    }

    return &container_hfe;
}

static void write_bits(
    struct track_info *ti,
    struct track_raw *raw,
    uint8_t *dst,
    unsigned int len)
{
    unsigned int i, bit;
    uint8_t x;

    /* Rotate the track so gap is at index. */
    bit = max_t(int, ti->data_bitoff - 128, 0);

    i = x = 0;
    while (i < len*8) {
        /* Consume a bit. */
        x <<= 1;
        x |= !!(raw->bits[bit>>3] & (0x80 >> (bit & 7)));
        /* Deal with byte and block boundaries. */
        if (!(++i & 7)) {
            *dst++ = x;
            /* Only half of each 512-byte block belongs to this track. */
            if (!(i & (256*8-1)))
                dst += 256;
        }
        /* Deal with wrap. If we have consumed all bits then make up some 
         * padding for the track gap. */
        if (++bit >= raw->bitlen)
            bit = (i < raw->bitlen) ? 0 : (raw->bitlen - 16);
    }
}

static void hfe_close(struct disk *d)
{
    uint32_t block[128];
    struct disk_info *di = d->di;
    struct disk_header *dhdr;
    struct track_header *thdr;
    struct track_raw *_raw[di->nr_tracks], **raw = _raw;
    unsigned int i, j, off, bitlen, bytelen, len;
    uint8_t *tbuf;

    for (i = 0; i < di->nr_tracks; i++) {
        raw[i] = track_alloc_raw_buffer(d);
        track_read_raw(raw[i], i);
        /* Unformatted tracks are random density, so skip speed check. 
         * Also they are random length so do not share the track buffer 
         * well with their neighbouring track on the same cylinder. Truncate 
         * the random data to a default length. */
        if (di->track[i].type == TRKTYP_unformatted) {
            raw[i]->bitlen = min(raw[i]->bitlen, DEFAULT_BITS_PER_TRACK(d));
            continue;
        }
        /* HFE tracks are uniform density. */
        for (j = 0; j < raw[i]->bitlen; j++) {
            if (raw[i]->speed[j] == 1000)
                continue;
            printf("*** T%u.%u: Variable-density track cannot be "
                   "correctly written to an Ext-ADF file\n", i/2, i&1);
            break;
        }
    }

    lseek(d->fd, 0, SEEK_SET);
    if (ftruncate(d->fd, 0) < 0)
        err(1, NULL);

    /* Block 0: Disk info. */
    memset(block, 0xff, 512);
    dhdr = (struct disk_header *)block;
    strncpy(dhdr->sig, "HXCPICFE", sizeof(dhdr->sig));
    dhdr->formatrevision = 0;
    dhdr->nr_tracks = di->nr_tracks / 2;
    dhdr->nr_sides = 2;
    dhdr->track_encoding = ENC_Amiga_MFM; /* XXX used? */
    dhdr->bitrate = htole16(250); /* XXX used? */
    dhdr->rpm = htole16(0);
    dhdr->interface_mode = IFM_Amiga_DD; /* XXX */
    dhdr->rsvd = 1;
    dhdr->track_list_offset = htole16(1);
    write_exact(d->fd, block, 512);

    /* Block 1: Track LUT. */
    memset(block, 0xff, 512);
    thdr = (struct track_header *)block;
    off = 2;
    for (i = 0; i < di->nr_tracks/2; i++) {
        bitlen = max(raw[i*2]->bitlen, raw[i*2+1]->bitlen);
        bytelen = ((bitlen + 7) / 8) * 2;
        thdr->offset = htole16(off);
        thdr->len = htole16(bytelen);
        off += (bytelen + 0x1ff) >> 9;
        thdr++;
    }
    write_exact(d->fd, block, 512);

    for (i = 0; i < di->nr_tracks/2; i++) {
        bitlen = max(raw[0]->bitlen, raw[1]->bitlen);
        bytelen = ((bitlen + 7) / 8) * 2;
        len = (bytelen + 0x1ff) & ~0x1ff;
        tbuf = memalloc(len);

        write_bits(&di->track[i*2], raw[0], &tbuf[0], len/2);
        write_bits(&di->track[i*2+1], raw[1], &tbuf[256], len/2);

        bit_reverse(tbuf, len);
        write_exact(d->fd, tbuf, len);
        memfree(tbuf);

        track_free_raw_buffer(raw[0]);
        track_free_raw_buffer(raw[1]);
        raw += 2;
    }
}

struct container container_hfe = {
    .init = hfe_init,
    .open = hfe_open,
    .close = hfe_close,
    .write_raw = dsk_write_raw
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
