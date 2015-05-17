 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Save/restore emulator state
  *
  * (c) 1999-2001 Toni Wilen
  *
  * see below for ASF-structure
  */

 /* Features:
  *
  * - full CPU state (68000/68010/68020)
  * - FPU
  * - full CIA-A and CIA-B state (with all internal registers)
  * - saves all custom registers and audio internal state.
  * - Chip, Bogo, Fast, Z3 and Picasso96 RAM supported
  * - disk drive type, imagefile, track and motor state
  * - Kickstart ROM version, address and size is saved. This data is not used during restore yet.
  * - Action Replay state is saved
  */

 /* Notes:
  *
  * - blitter state is not saved, blitter is forced to finish immediately if it
  *   was active
  * - disk DMA state is completely saved 
  * - does not ask for statefile name and description. Currently uses DF0's disk
  *   image name (".adf" is replaced with ".asf")
  * - only Amiga state is restored, harddisk support, autoconfig, expansion boards etc..
  *   are not saved/restored (and probably never will).
  * - use this for saving games that can't be saved to disk
  */

 /* Usage :
  *
  * save:
  * 
  * set savestate_state = STATE_DOSAVE, savestate_filename = "..."
  *
  * restore:
  * 
  * set savestate_state = STATE_DORESTORE, savestate_filename = "..."
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "zfile.h"
#include "autoconf.h"
#include "custom.h"
#include "sounddep/sound.h"
#include "newcpu.h"
#include "savestate.h"
#include "audio.h"

int savestate_state = 0;

struct zfile *savestate_file;
static int savestate_docompress, savestate_specialdump;

char savestate_fname[MAX_DPATH]={
	'/', 't', 'm', 'p', '/', 'n', 'u', 'l', 'l', '.', 'a', 's', 'f', '\0'
};

/* functions for reading/writing bytes, shorts and longs in big-endian
 * format independent of host machine's endianess */

void save_u32_func (uae_u8 **dstp, uae_u32 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = (uae_u8)(v >> 24);
    *dst++ = (uae_u8)(v >> 16);
    *dst++ = (uae_u8)(v >> 8);
    *dst++ = (uae_u8)(v >> 0);
    *dstp = dst;
}
void save_u64_func (uae_u8 **dstp, uae_u64 v)
{
    save_u32_func (dstp, (uae_u32)(v >> 32));
    save_u32_func (dstp, (uae_u32)v);
}
void save_u16_func (uae_u8 **dstp, uae_u16 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = (uae_u8)(v >> 8);
    *dst++ = (uae_u8)(v >> 0);
    *dstp = dst;
}
void save_u8_func (uae_u8 **dstp, uae_u8 v)
{
    uae_u8 *dst = *dstp;
    *dst++ = v;
    *dstp = dst;
}
void save_string_func (uae_u8 **dstp, const char *from)
{
    uae_u8 *dst = *dstp;
    while(from && *from)
	*dst++ = *from++;
    *dst++ = 0;
    *dstp = dst;
}

uae_u32 restore_u32_func (uae_u8 **dstp)
{
    uae_u32 v;
    uae_u8 *dst = *dstp;
    v = (dst[0] << 24) | (dst[1] << 16) | (dst[2] << 8) | (dst[3]);
    *dstp = dst + 4;
    return v;
}
uae_u64 restore_u64_func (uae_u8 **dstp)
{
    uae_u64 v;
    
    v = restore_u32_func (dstp);
    v <<= 32;
    v |= restore_u32_func (dstp);
    return v;
}
uae_u16 restore_u16_func (uae_u8 **dstp)
{
    uae_u16 v;
    uae_u8 *dst = *dstp;
    v=(dst[0] << 8) | (dst[1]);
    *dstp = dst + 2;
    return v;
}
uae_u8 restore_u8_func (uae_u8 **dstp)
{
    uae_u8 v;
    uae_u8 *dst = *dstp;
    v = dst[0];
    *dstp = dst + 1;
    return v;
}
char *restore_string_func (uae_u8 **dstp)
{
    int len;
    uae_u8 v;
    uae_u8 *dst = *dstp;
    char *top, *to;
    len = strlen((char *)dst) + 1;
    top = to = (char *)malloc (len);
    do {
	v = *dst++;
	*top++ = v;
    } while(v);
    *dstp = dst;
    return to; 
}

/* read and write IFF-style hunks */

static void save_chunk (struct zfile *f, uae_u8 *chunk, size_t len, char *name, int compress)
{
    uae_u8 tmp[8], *dst;
    uae_u8 zero[4]= { 0, 0, 0, 0 };
    uae_u32 flags;
    size_t pos;
    size_t chunklen, len2;

    if (!chunk)
	return;

    if (compress < 0) {
	zfile_fwrite (chunk, 1, len, f);
	return;
    }
    /* chunk name */
    zfile_fwrite (name, 1, 4, f);
    pos = zfile_ftell (f);
    /* chunk size */
    dst = &tmp[0];
    chunklen = len + 4 + 4 + 4;
    save_u32 (chunklen);
    zfile_fwrite (&tmp[0], 1, 4, f);
    /* chunk flags */
    flags = 0;
    dst = &tmp[0];
    save_u32 (flags | compress);
    zfile_fwrite (&tmp[0], 1, 4, f);
    /* chunk data */
    if (compress) {
	int tmplen = len;
	dst = &tmp[0];
	save_u32 (len);
	zfile_fwrite (&tmp[0], 1, 4, f);
	len = zfile_zcompress (f, chunk, len);
	if (len > 0) {
	    zfile_fseek (f, pos, SEEK_SET);
	    dst = &tmp[0];
	    save_u32 (len + 4 + 4 + 4 + 4);
	    zfile_fwrite (&tmp[0], 1, 4, f);
	    zfile_fseek (f, 0, SEEK_END);
	} else {
	    len = tmplen;
	    compress = 0;
	    zfile_fseek (f, -8, SEEK_CUR);
	    dst = &tmp[0];
	    save_u32 (flags);
	    zfile_fwrite (&tmp[0], 1, 4, f);
	}
    }
    if (!compress)
	zfile_fwrite (chunk, 1, len, f);
    /* alignment */
    len2 = 4 - (len & 3);
    if (len2)
	zfile_fwrite (zero, 1, len2, f);

    write_log ("Chunk '%s' chunk size %d (%d)\n", name, chunklen, len);
}

static uae_u8 *restore_chunk (struct zfile *f, char *name, size_t *len, size_t *totallen, size_t *filepos)
{
    uae_u8 tmp[4], dummy[4], *mem, *src;
    uae_u32 flags;
    int len2;

    *totallen = 0;
    /* chunk name */
    zfile_fread (name, 1, 4, f);
    name[4] = 0;
    /* chunk size */
    zfile_fread (tmp, 1, 4, f);
    src = tmp;
    len2 = restore_u32 () - 4 - 4 - 4;
    if (len2 < 0)
	len2 = 0;
    *len = len2;
    if (len2 == 0)
	return 0;

    /* chunk flags */
    zfile_fread (tmp, 1, 4, f);
    src = tmp;
    flags = restore_u32 ();

    *totallen = *len;
    if (flags & 1) {
	zfile_fread (tmp, 1, 4, f);
	src = tmp;
	*totallen = restore_u32();
	*filepos = zfile_ftell (f) - 4 - 4 - 4;
	len2 -= 4;
    } else {
        *filepos = zfile_ftell (f) - 4 - 4;
    }
    /* chunk data.  RAM contents will be loaded during the reset phase,
       no need to malloc multiple megabytes here.  */
    if (strcmp (name, "CRAM") != 0
	&& strcmp (name, "BRAM") != 0
	&& strcmp (name, "FRAM") != 0
	&& strcmp (name, "ZRAM") != 0
	&& strcmp (name, "PRAM") != 0)
    {
	/* without zeros at the end old state files may not work */
	mem = (uae_u8 *)calloc (1, len2 + 32); 
	zfile_fread (mem, 1, len2, f);
    } else {
	mem = 0;
	zfile_fseek (f, len2, SEEK_CUR);
    }

    /* alignment */
    len2 = 4 - (len2 & 3);
    if (len2)
	zfile_fread (dummy, 1, len2, f);
    return mem;
}

void restore_ram (size_t filepos, uae_u8 *memory)
{
    uae_u8 tmp[8];
    uae_u8 *src = tmp;
    int size, fullsize;
    uae_u32 flags;
    
    zfile_fseek (savestate_file, filepos, SEEK_SET);
    zfile_fread (tmp, 1, sizeof(tmp), savestate_file);
    size = restore_u32();
    flags = restore_u32();
    size -= 4 + 4 + 4;
    if (flags & 1) {
        zfile_fread (tmp, 1, 4, savestate_file);
        src = tmp;
	      fullsize = restore_u32();
	      size -= 4;
	      zfile_zuncompress (memory, fullsize, savestate_file, size);
    } else {
	      zfile_fread (memory, 1, size, savestate_file);
    }
}

static void restore_header (uae_u8 *src)
{
    char *emuname, *emuversion, *description;

    restore_u32();
    emuname = restore_string ();
    emuversion = restore_string ();
    description = restore_string ();
    write_log ("Saved with: '%s %s', description: '%s'\n",
	emuname,emuversion,description);
    xfree (description);
    xfree (emuversion);
    xfree (emuname);
}

/* restore all subsystems */

void restore_state (char *filename)
{
    struct zfile *f;
    uae_u8 *chunk,*end;
    char name[5];
    size_t len, totallen;
    size_t filepos;

    chunk = 0;
    f = zfile_fopen (filename, "rb");
    if (!f)
	goto error;

    chunk = restore_chunk (f, name, &len, &totallen, &filepos);
    if (!chunk || memcmp (name, "ASF ", 4)) {
	write_log ("%s is not an AmigaStateFile\n",filename);
	goto error;
    }
    savestate_file = f;
    restore_header (chunk);
    xfree (chunk);
    changed_prefs.bogomem_size = 0;
    changed_prefs.chipmem_size = 0;
    changed_prefs.fastmem_size = 0;
    savestate_state = STATE_RESTORE;
    for (;;) {
	name[0] = 0;
	chunk = end = restore_chunk (f, name, &len, &totallen, &filepos);
	write_log ("Chunk '%s' size %d (%d)\n", name, len, totallen);
	if (!strcmp (name, "END "))
	    break;
	if (!strcmp (name, "CRAM")) {
	    restore_cram (totallen, filepos);
	    continue;
	} else if (!strcmp (name, "BRAM")) {
	    restore_bram (totallen, filepos);
	    continue;
#ifdef AUTOCONFIG
	} else if (!strcmp (name, "FRAM")) {
	    restore_fram (totallen, filepos);
	    continue;
	} else if (!strcmp (name, "ZRAM")) {
	    restore_zram (totallen, filepos);
	    continue;
#endif
#ifdef PICASSO96
	} else if (!strcmp (name, "PRAM")) {
	    restore_pram (totallen, filepos);
	    continue;
#endif
	} else if (!strcmp (name, "CPU "))
	    end = restore_cpu (chunk);
#ifdef FPUEMU
	else if (!strcmp (name, "FPU "))
	    end = restore_fpu (chunk);
#endif
	else if (!strcmp (name, "AGAC"))
	    end = restore_custom_agacolors (chunk);
	else if (!strcmp (name, "SPR0"))
	    end = restore_custom_sprite (0, chunk);
	else if (!strcmp (name, "SPR1"))
	    end = restore_custom_sprite (1, chunk);
	else if (!strcmp (name, "SPR2"))
	    end = restore_custom_sprite (2, chunk);
	else if (!strcmp (name, "SPR3"))
	    end = restore_custom_sprite (3, chunk);
	else if (!strcmp (name, "SPR4"))
	    end = restore_custom_sprite (4, chunk);
	else if (!strcmp (name, "SPR5"))
	    end = restore_custom_sprite (5, chunk);
	else if (!strcmp (name, "SPR6"))
	    end = restore_custom_sprite (6, chunk);
	else if (!strcmp (name, "SPR7"))
	    end = restore_custom_sprite (7, chunk);
	else if (!strcmp (name, "CIAA"))
	    end = restore_cia (0, chunk);
	else if (!strcmp (name, "CIAB"))
	    end = restore_cia (1, chunk);
	else if (!strcmp (name, "CHIP"))
	    end = restore_custom (chunk);
	else if (!strcmp (name, "AUD0"))
	    end = restore_audio (0, chunk);
	else if (!strcmp (name, "AUD1"))
	    end = restore_audio (1, chunk);
	else if (!strcmp (name, "AUD2"))
	    end = restore_audio (2, chunk);
	else if (!strcmp (name, "AUD3"))
	    end = restore_audio (3, chunk);
	else if (!strcmp (name, "BLIT"))
	    end = restore_blitter (chunk);
	else if (!strcmp (name, "DISK"))
	    end = restore_floppy (chunk);
	else if (!strcmp (name, "DSK0"))
	    end = restore_disk (0, chunk);
	else if (!strcmp (name, "DSK1"))
	    end = restore_disk (1, chunk);
	else if (!strcmp (name, "DSK2"))
	    end = restore_disk (2, chunk);
	else if (!strcmp (name, "DSK3"))
	    end = restore_disk (3, chunk);
	else if (!strcmp (name, "KEYB"))
	    end = restore_keyboard (chunk);
#ifdef AUTOCONFIG
	else if (!strcmp (name, "EXPA"))
	    end = restore_expansion (chunk);
#endif
	else if (!strcmp (name, "ROM "))
	    end = restore_rom (chunk);
#ifdef PICASSO96
	else if (!strcmp (name, "P96 "))
	    end = restore_p96 (chunk);
#endif
#ifdef FILESYS
	else if (!strcmp (name, "FSYS"))
	    end = restore_filesys (chunk);
#endif
	else
	    write_log ("unknown chunk '%s' size %d bytes\n", name, len);
	if (len != end - chunk)
	    write_log ("Chunk '%s' total size %d bytes but read %d bytes!\n",
		       name, len, end - chunk);
	xfree (chunk);
    }
    restore_blitter_finish();
    return;

  error:
    savestate_state = 0;
    savestate_file = 0;
    if (chunk)
	xfree (chunk);
    if (f)
	zfile_fclose (f);
}

void savestate_restore_finish (void)
{
    if (savestate_state != STATE_RESTORE)
	return;
    zfile_fclose (savestate_file);
    savestate_file = 0;
    savestate_state = 0;
    restore_cpu_finish();
}

/* 1=compressed,2=not compressed,3=ram dump,4=audio dump */
void savestate_initsave (char *filename, int mode)
{
    strcpy (savestate_fname, filename);
    savestate_docompress = (mode == 1) ? 1 : 0;
    savestate_specialdump = (mode == 3) ? 1 : (mode == 4) ? 2 : 0;
}

static void save_rams (struct zfile *f, int comp)
{
    uae_u8 *dst;
    int len;

    dst = save_cram (&len);
    save_chunk (f, dst, len, (char *)"CRAM", comp);
    dst = save_bram (&len);
    save_chunk (f, dst, len, (char *)"BRAM", comp);
#ifdef AUTOCONFIG
    dst = save_fram (&len);
    save_chunk (f, dst, len, (char *)"FRAM", comp);
    dst = save_zram (&len);
    save_chunk (f, dst, len, "ZRAM", comp);
#endif
#ifdef PICASSO96
    dst = save_pram (&len);
    save_chunk (f, dst, len, "PRAM", comp);
    dst = save_p96 (&len, 0);
    save_chunk (f, dst, len, "P96 ", comp);
#endif
}

/* Save all subsystems  */

void save_state (char *filename, const char *description)
{
    uae_u8 header[1000];
    char tmp[100];
    uae_u8 *dst;
    struct zfile *f;
    int len,i;
    char name[5];
    int comp = savestate_docompress;
    static int warned;

#ifdef FILESYS
//    if (nr_units (currprefs.mountinfo) && !warned) {
//    	warned = 1;
//    	gui_message("WARNING: State saves do not support harddrive emulation");
//    }
#endif
    custom_prepare_savestate ();

    f = zfile_fopen (filename, "w+b");
    if (!f)
	return;
    if (savestate_specialdump) {
	    size_t pos;
//	    if (savestate_specialdump == 2)
//	      write_wavheader(f, 0, 22050);
	    pos = zfile_ftell(f);
	    save_rams (f, -1);
//	if (savestate_specialdump == 2) {
//	    int len, len2, i;
//	    uae_u8 *tmp;
//	    len = zfile_ftell(f) - pos;
//	    tmp = xmalloc(len);
//	    zfile_fseek(f, pos, SEEK_SET);
//	    len2 = zfile_fread(tmp, 1, len, f);
//	    for (i = 0; i < len2; i++)
//		tmp[i] += 0x80;
//	    write_wavheader(f, len, 22050);
//	    zfile_fwrite(tmp, len2, 1, f);	    
//	    xfree(tmp);
//	}
      zfile_fclose (f);
	    return;
    }

    dst = header;
    save_u32 (0);
    save_string("UAE");
    snprintf (tmp, 32, "%d.%d.%d", UAEMAJOR, UAEMINOR, UAESUBREV);
    save_string (tmp);
    save_string (description);
    save_chunk (f, header, dst-header, (char *)"ASF ", 0);

    dst = save_cpu (&len, 0);
    save_chunk (f, dst, len, (char *)"CPU ", 0);
    xfree (dst);
#ifdef FPUEMU
    dst = save_fpu (&len, 0);
    save_chunk (f, dst, len, "FPU ", 0);
    xfree (dst);
#endif

    strcpy(name, "DSKx");
    for (i = 0; i < 4; i++) {
	dst = save_disk (i, &len, 0);
	if (dst) {
	    name[3] = i + '0';
	    save_chunk (f, dst, len, name, 0);
	    xfree (dst);
	}
    }
    dst = save_floppy (&len, 0);
    save_chunk (f, dst, len, (char *)"DISK", 0);
    xfree (dst);

    dst = save_blitter (&len, 0);
    save_chunk (f, dst, len, (char *)"BLIT", 0);
    xfree (dst);

    dst = save_custom (&len, 0, 0);
    save_chunk (f, dst, len, (char *)"CHIP", 0);
    xfree (dst);

    dst = save_custom_agacolors (&len, 0);
    save_chunk (f, dst, len, (char *)"AGAC", 0);
    xfree (dst);

    strcpy (name, (char *)"SPRx");
    for (i = 0; i < 8; i++) {
	dst = save_custom_sprite (i, &len, 0);
	name[3] = i + '0';
	save_chunk (f, dst, len, name, 0);
	xfree (dst);
    }

    strcpy (name, (char *)"AUDx");
    for (i = 0; i < 4; i++) {
	dst = save_audio (i, &len, 0);
	name[3] = i + '0';
	save_chunk (f, dst, len, name, 0);
	xfree (dst);
    }

    dst = save_cia (0, &len, 0);
    save_chunk (f, dst, len, (char *)"CIAA", 0);
    xfree (dst);

    dst = save_cia (1, &len, 0);
    save_chunk (f, dst, len, (char *)"CIAB", 0);
    xfree (dst);

    dst = save_keyboard (&len);
    save_chunk (f, dst, len, (char *)"KEYB", 0);
    xfree (dst);

#ifdef AUTOCONFIG
    dst = save_expansion (&len, 0);
    save_chunk (f, dst, len, (char *)"EXPA", 0);
    xfree (dst);
#endif

    save_rams (f, comp);

    dst = save_rom (1, &len, 0);
    do {
	if (!dst)
	    break;
	save_chunk (f, dst, len, (char *)"ROM ", 0);
	xfree (dst);
    } while ((dst = save_rom (0, &len, 0)));

#ifdef FILESYS
    for (i = 0; i < nr_units (currprefs.mountinfo); i++) {
	dst = save_filesys (i, &len);
	if (dst) {
	    save_chunk (f, dst, len, (char *)"FSYS", 0);
	    xfree (dst);
	}
    }
#endif

    zfile_fwrite ((void *)"END ", 1, 4, f);
    zfile_fwrite ((void *)"\0\0\0\08", 1, 4, f);
    write_log ("Save of '%s' complete\n", filename);
    zfile_fclose (f);
    savestate_state = 0;
}

void savestate_quick (int slot, int save)
{
    int i, len = strlen (savestate_fname);
    i = len - 1;
    while (i >= 0 && savestate_fname[i] != '_')
	i--;
    if (i < len - 6 || i <= 0) { /* "_?.uss" */
	i = len - 1;
	while (i >= 0 && savestate_fname[i] != '.')
	    i--;
	if (i <= 0) {
	    write_log ("savestate name skipped '%s'\n", savestate_fname);
	    return;
	}
    }
    strcpy (savestate_fname + i, ".uss");
    if (slot > 0)
	sprintf (savestate_fname + i, "_%d.uss", slot);
    if (save) {
	write_log ("saving '%s'\n", savestate_fname);
	savestate_docompress = 1;
	save_state (savestate_fname, "");
    } else {
	if (!zfile_exists (savestate_fname)) {
	    write_log ("staterestore, file '%s' not found\n", savestate_fname);
	    return;
	}
	savestate_state = STATE_DORESTORE;
	write_log ("staterestore starting '%s'\n", savestate_fname);
    }
}

/*

My (Toni Wilen <twilen@arabuusimiehet.com>)
proposal for Amiga-emulators' state-save format

Feel free to comment...

This is very similar to IFF-fileformat
Every hunk must end to 4 byte boundary,
fill with zero bytes if needed

version 0.8

HUNK HEADER (beginning of every hunk)

        hunk name (4 ascii-characters)
        hunk size (including header)
        hunk flags             

        bit 0 = chunk contents are compressed with zlib (maybe RAM chunks only?)

HEADER

        "ASF " (AmigaStateFile)
        
	statefile version
        emulator name ("uae", "fellow" etc..)
        emulator version string (example: "0.8.15")
        free user writable comment string

CPU

         "CPU "

        CPU model               4 (68000,68010 etc..)
        CPU typeflags           bit 0=EC-model or not
        D0-D7                   8*4=32
        A0-A6                   7*4=32
        PC                      4
	unused			4
	68000 prefetch (IRC)    2
	68000 prefetch (IR)     2
        USP                     4
        ISP                     4
        SR/CCR                  2
        flags                   4 (bit 0=CPU was HALTed)

        CPU specific registers

        68000: SR/CCR is last saved register
        68010: save also DFC,SFC and VBR
        68020: all 68010 registers and CAAR,CACR and MSP
        etc..

        DFC                     4 (010+)
        SFC                     4 (010+)
        VBR                     4 (010+)

        CAAR                    4 (020-030)
        CACR                    4 (020+)
        MSP                     4 (020+)

FPU (only if used)

	"FPU "

        FPU model               4 (68881/68882/68040)
        FPU typeflags           4 (keep zero)

        FP0-FP7                 4+4+2 (80 bits)
        FPCR                    4
        FPSR                    4
        FPIAR                   4
MMU (when and if MMU is supported in future..)

        MMU model               4 (68851,68030,68040)

        // 68040 fields

        ITT0                    4
        ITT1                    4
        DTT0                    4
        DTT1                    4
        URP                     4
        SRP                     4
        MMUSR                   4
        TC                      2

		
CUSTOM CHIPS

        "CHIP"

        chipset flags   4      OCS=0,ECSAGNUS=1,ECSDENISE=2,AGA=4
                               ECSAGNUS and ECSDENISE can be combined

        DFF000-DFF1FF   352    (0x120 - 0x17f and 0x0a0 - 0xdf excluded)

        sprite registers (0x120 - 0x17f) saved with SPRx chunks
        audio registers (0x0a0 - 0xdf) saved with AUDx chunks

AGA COLORS

        "AGAC"

        AGA color               8 banks * 32 registers *
        registers               LONG (XRGB) = 1024

SPRITE

        "SPR0" - "SPR7"


        SPRxPT                  4
        SPRxPOS                 2
        SPRxCTL                 2
        SPRxDATA                2
        SPRxDATB                2
        AGA sprite DATA/DATB    3 * 2 * 2
        sprite "armed" status   1

        sprites maybe armed in non-DMA mode
        use bit 0 only, other bits are reserved


AUDIO
        "AUD0" "AUD1" "AUD2" "AUD3"

        audio state             1
        machine mode
        AUDxVOL                 1
	irq?                    1
	data_written?           1
        internal AUDxLEN        2
        AUDxLEN                 2
	internal AUDxPER        2
	AUDxPER                 2
        internal AUDxLC         4
	AUDxLC                  4
	evtime?                 4

BLITTER

        "BLIT"

        internal blitter state

        flags                   4
        bit 0=blitter active
        bit 1=fill carry bit
        internal ahold          4
	      internal bhold          4
	      internal hsize          2
	      internal vsize          2

CIA

        "CIAA" and "CIAB"

        BFE001-BFEF01   16*1 (CIAA)
        BFD000-BFDF00   16*1 (CIAB)

        internal registers

        IRQ mask (ICR)  1 BYTE
        timer latches   2 timers * 2 BYTES (LO/HI)
        latched tod     3 BYTES (LO/MED/HI)
        alarm           3 BYTES (LO/MED/HI)
        flags           1 BYTE
                        bit 0=tod latched (read)
                        bit 1=tod stopped (write)
	div10 counter	1 BYTE

FLOPPY DRIVES

        "DSK0" "DSK1" "DSK2" "DSK3"

        drive state

        drive ID-word           4
        state                   1 (bit 0: motor on, bit 1: drive disabled, bit 2: current id bit)
        rw-head track           1
        dskready                1
        id-mode                 1 (ID mode bit number 0-31)
        floppy information

        bits from               4
        beginning of track
        CRC of disk-image       4 (used during restore to check if image
                                  is correct)
        disk-image              null-terminated
        file name

INTERNAL FLOPPY CONTROLLER STATUS

        "DISK"

        current DMA word        2
        DMA word bit offset     1
        WORDSYNC found          1 (no=0,yes=1)
        hpos of next bit        1
        DSKLENGTH status        0=off,1=written once,2=written twice
	      unused                  2

RAM SPACE 

        "xRAM" (CRAM = chip, BRAM = bogo, FRAM = fast, ZFRAM = Z3)

        start address           4 ("bank"=chip/slow/fast etc..)
        of RAM "bank"
        RAM "bank" size         4
        RAM flags               4 (bit 0 = zlib compressed)
        RAM "bank" contents

ROM SPACE

        "ROM "

        ROM start               4
        address
        size of ROM             4
        ROM type                4 KICK=0
        ROM flags               4
        ROM version             2
        ROM revision            2
        ROM CRC                 4 see below
        ROM-image ID-string     null terminated, see below
        path to rom image
        ROM contents            (Not mandatory, use hunk size to check if
                                this hunk contains ROM data or not)

        Kickstart ROM:
         ID-string is "Kickstart x.x"
         ROM version: version in high word and revision in low word
         Kickstart ROM version and revision can be found from ROM start
         + 12 (version) and +14 (revision)

        ROM version and CRC is only meant for emulator to automatically
        find correct image from its ROM-directory during state restore.

        Usually saving ROM contents is not good idea.

ACTION REPLAY

	"ACTR"

	Model (1,2,3)		4
	path to rom image
	RAM space		(depends on model)
	ROM CRC                 4

END
        hunk "END " ends, remember hunk size 8!


EMULATOR SPECIFIC HUNKS

Read only if "emulator name" in header is same as used emulator.
Maybe useful for configuration?

misc:

- save only at position 0,0 before triggering VBLANK interrupt
- all data must be saved in bigendian format
- should we strip all paths from image file names?

*/
