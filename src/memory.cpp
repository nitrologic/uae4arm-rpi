 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Memory management
  *
  * (c) 1995 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "ersatz.h"
#include "zfile.h"
#include "memory.h"
#include "rommgr.h"
#include "newcpu.h"
#include "custom.h"
#include "autoconf.h"
#include "savestate.h"
#include "crc32.h"
#include "gui.h"

#ifdef JIT
/* Set by each memory handler that does not simply access real memory. */
int special_mem;
#endif

uae_u32 allocated_chipmem;
uae_u32 allocated_fastmem;
uae_u32 allocated_bogomem;
uae_u32 allocated_gfxmem;
uae_u32 allocated_z3fastmem;

uae_u32 max_z3fastmem = 128 * 1024 * 1024;

static size_t bootrom_filepos, chip_filepos, bogo_filepos, rom_filepos, a3000lmem_filepos, a3000hmem_filepos;

/* Set if we notice during initialization that settings changed,
   and we must clear all memory to prevent bogus contents from confusing
   the Kickstart.  */
static bool need_hardreset;

/* The address space setting used during the last reset.  */
static bool last_address_space_24;

addrbank *mem_banks[MEMORY_BANKS];


int addr_valid(const TCHAR *txt, uaecptr addr, uae_u32 len)
{
  addrbank *ab = &get_mem_bank(addr);
  if (ab == 0 || !(ab->flags & (ABFLAG_RAM | ABFLAG_ROM)) || addr < 0x100 || len < 0 || len > 16777215 || !valid_address(addr, len)) {
		write_log (_T("corrupt %s pointer %x (%d) detected!\n"), txt, addr, len);
  	return 0;
  }
  return 1;
}

uae_u32	chipmem_mask, chipmem_full_mask;
uae_u32 kickmem_mask, extendedkickmem_mask, extendedkickmem2_mask, bogomem_mask;
uae_u32 a3000lmem_mask, a3000hmem_mask, cardmem_mask;

/* A dummy bank that only contains zeros */

static uae_u32 REGPARAM3 dummy_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 dummy_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 dummy_bget (uaecptr) REGPARAM;
static void REGPARAM3 dummy_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 dummy_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 dummy_bput (uaecptr, uae_u32) REGPARAM;
static int REGPARAM3 dummy_check (uaecptr addr, uae_u32 size) REGPARAM;

#define NONEXISTINGDATA 0
//#define NONEXISTINGDATA 0xffffffff

static uae_u32 REGPARAM2 dummy_lget (uaecptr addr)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  if (currprefs.cpu_model >= 68020)
    return NONEXISTINGDATA;
  return (regs.irc << 16) | regs.irc;
}
uae_u32 REGPARAM2 dummy_lgeti (uaecptr addr)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  if (currprefs.cpu_model >= 68020)
  	return NONEXISTINGDATA;
  return (regs.irc << 16) | regs.irc;
}

static uae_u32 REGPARAM2 dummy_wget (uaecptr addr)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  if (currprefs.cpu_model >= 68020)
    return NONEXISTINGDATA;
  return regs.irc;
}
uae_u32 REGPARAM2 dummy_wgeti (uaecptr addr)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  if (currprefs.cpu_model >= 68020)
  	return NONEXISTINGDATA;
  return regs.irc;
}

static uae_u32 REGPARAM2 dummy_bget (uaecptr addr)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  if (currprefs.cpu_model >= 68020)
    return NONEXISTINGDATA;
  return (addr & 1) ? (regs.irc & 0xff) : ((regs.irc >> 8) & 0xff);
}

static void REGPARAM2 dummy_lput (uaecptr addr, uae_u32 l)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static void REGPARAM2 dummy_wput (uaecptr addr, uae_u32 w)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static void REGPARAM2 dummy_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static int REGPARAM2 dummy_check (uaecptr addr, uae_u32 size)
{
#ifdef JIT
  special_mem |= S_READ;
#endif
  return 0;
}

/* Chip memory */

uae_u8 *chipmemory;

static int REGPARAM3 chipmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *REGPARAM3 chipmem_xlate (uaecptr addr) REGPARAM;

uae_u32 REGPARAM2 chipmem_lget (uaecptr addr)
{
  uae_u32 *m;

  addr &= chipmem_mask;
  m = (uae_u32 *)(chipmemory + addr);
  return do_get_mem_long (m);
}

static uae_u32 REGPARAM2 chipmem_wget (uaecptr addr)
{
  uae_u16 *m, v;

  addr &= chipmem_mask;
  m = (uae_u16 *)(chipmemory + addr);
  v = do_get_mem_word (m);
  return v;
}

static uae_u32 REGPARAM2 chipmem_bget (uaecptr addr)
{
	uae_u8 v;
  addr &= chipmem_mask;
	v = chipmemory[addr];
	return v;
}

void REGPARAM2 chipmem_lput (uaecptr addr, uae_u32 l)
{
  uae_u32 *m;
  addr &= chipmem_mask;
  m = (uae_u32 *)(chipmemory + addr);
  do_put_mem_long(m, l);
}

void REGPARAM2 chipmem_wput (uaecptr addr, uae_u32 w)
{
 uae_u16 *m;
 addr &= chipmem_mask;
 m = (uae_u16 *)(chipmemory + addr);
 do_put_mem_word (m, w);
}

void REGPARAM2 chipmem_bput (uaecptr addr, uae_u32 b)
{
  addr &= chipmem_mask;
	chipmemory[addr] = b;
}

static uae_u32 REGPARAM2 chipmem_agnus_lget (uaecptr addr)
{
  uae_u32 *m;

  addr &= chipmem_full_mask;
  m = (uae_u32 *)(chipmemory + addr);
  return do_get_mem_long (m);
}

uae_u32 REGPARAM2 chipmem_agnus_wget (uaecptr addr)
{
  uae_u16 *m;

  addr &= chipmem_full_mask;
  m = (uae_u16 *)(chipmemory + addr);
  return do_get_mem_word (m);
}

static uae_u32 REGPARAM2 chipmem_agnus_bget (uaecptr addr)
{
  addr &= chipmem_full_mask;
	return chipmemory[addr];
}

static void REGPARAM2 chipmem_agnus_lput (uaecptr addr, uae_u32 l)
{
  uae_u32 *m;

  addr &= chipmem_full_mask;
  if (addr >= allocated_chipmem)
  	return;
  m = (uae_u32 *)(chipmemory + addr);
  do_put_mem_long (m, l);
}

void REGPARAM2 chipmem_agnus_wput (uaecptr addr, uae_u32 w)
{
  uae_u16 *m;

  addr &= chipmem_full_mask;
  if (addr >= allocated_chipmem)
	  return;
  m = (uae_u16 *)(chipmemory + addr);
  do_put_mem_word (m, w);
}

static void REGPARAM2 chipmem_agnus_bput (uaecptr addr, uae_u32 b)
{
  addr &= chipmem_full_mask;
  if (addr >= allocated_chipmem)
  	return;
	chipmemory[addr] = b;
}

static int REGPARAM2 chipmem_check (uaecptr addr, uae_u32 size)
{
  addr &= chipmem_mask;
  return (addr + size) <= allocated_chipmem;
}

static uae_u8 *REGPARAM2 chipmem_xlate (uaecptr addr)
{
	addr &= chipmem_mask;
  return chipmemory + addr;
}

/* Slow memory */

static uae_u8 *bogomemory;

static uae_u32 REGPARAM3 bogomem_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 bogomem_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 bogomem_bget (uaecptr) REGPARAM;
static void REGPARAM3 bogomem_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 bogomem_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 bogomem_bput (uaecptr, uae_u32) REGPARAM;
static int REGPARAM3 bogomem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *REGPARAM3 bogomem_xlate (uaecptr addr) REGPARAM;

static uae_u32 REGPARAM2 bogomem_lget (uaecptr addr)
{
  uae_u32 *m;
  addr &= bogomem_mask;
  m = (uae_u32 *)(bogomemory + addr);
  return do_get_mem_long (m);
}

static uae_u32 REGPARAM2 bogomem_wget (uaecptr addr)
{
  uae_u16 *m;
  addr &= bogomem_mask;
  m = (uae_u16 *)(bogomemory + addr);
  return do_get_mem_word (m);
}

static uae_u32 REGPARAM2 bogomem_bget (uaecptr addr)
{
  addr &= bogomem_mask;
	return bogomemory[addr];
}

static void REGPARAM2 bogomem_lput (uaecptr addr, uae_u32 l)
{
  uae_u32 *m;
  addr &= bogomem_mask;
  m = (uae_u32 *)(bogomemory + addr);
  do_put_mem_long (m, l);
}

static void REGPARAM2 bogomem_wput (uaecptr addr, uae_u32 w)
{
  uae_u16 *m;
  addr &= bogomem_mask;
  m = (uae_u16 *)(bogomemory + addr);
  do_put_mem_word (m, w);
}

static void REGPARAM2 bogomem_bput (uaecptr addr, uae_u32 b)
{
  addr &= bogomem_mask;
	bogomemory[addr] = b;
}

static int REGPARAM2 bogomem_check (uaecptr addr, uae_u32 size)
{
  addr &= bogomem_mask;
  return (addr + size) <= allocated_bogomem;
}

static uae_u8 *REGPARAM2 bogomem_xlate (uaecptr addr)
{
  addr &= bogomem_mask;
  return bogomemory + addr;
}

/* Kick memory */

uae_u8 *kickmemory;
uae_u16 kickstart_version;
static int kickmem_size;

static uae_u32 REGPARAM3 kickmem_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 kickmem_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 kickmem_bget (uaecptr) REGPARAM;
static void REGPARAM3 kickmem_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 kickmem_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 kickmem_bput (uaecptr, uae_u32) REGPARAM;
static int REGPARAM3 kickmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *REGPARAM3 kickmem_xlate (uaecptr addr) REGPARAM;

static uae_u32 REGPARAM2 kickmem_lget (uaecptr addr)
{
  uae_u32 *m;
  addr &= kickmem_mask;
  m = (uae_u32 *)(kickmemory + addr);
  return do_get_mem_long(m);
}

static uae_u32 REGPARAM2 kickmem_wget (uaecptr addr)
{
  uae_u16 *m;
  addr &= kickmem_mask;
  m = (uae_u16 *)(kickmemory + addr);
  return do_get_mem_word (m);
}

static uae_u32 REGPARAM2 kickmem_bget (uaecptr addr)
{
  addr &= kickmem_mask;
	return kickmemory[addr];
}

static void REGPARAM2 kickmem_lput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static void REGPARAM2 kickmem_wput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static void REGPARAM2 kickmem_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}

static int REGPARAM2 kickmem_check (uaecptr addr, uae_u32 size)
{
  addr &= kickmem_mask;
  return (addr + size) <= kickmem_size;
}

static uae_u8 *REGPARAM2 kickmem_xlate (uaecptr addr)
{
  addr &= kickmem_mask;
  return kickmemory + addr;
}

/* CD32/CDTV extended kick memory */

uae_u8 *extendedkickmemory, *extendedkickmemory2;
static int extendedkickmem_size, extendedkickmem2_size;
static uae_u32 extendedkickmem_start, extendedkickmem2_start;
static int extendedkickmem_type;

#define EXTENDED_ROM_CD32 1
#define EXTENDED_ROM_CDTV 2
#define EXTENDED_ROM_KS 3
#define EXTENDED_ROM_ARCADIA 4

static uae_u32 REGPARAM3 extendedkickmem_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 extendedkickmem_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 extendedkickmem_bget (uaecptr) REGPARAM;
static void REGPARAM3 extendedkickmem_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 extendedkickmem_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 extendedkickmem_bput (uaecptr, uae_u32) REGPARAM;
static int REGPARAM3 extendedkickmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *REGPARAM3 extendedkickmem_xlate (uaecptr addr) REGPARAM;
static uae_u32 REGPARAM2 extendedkickmem_lget (uaecptr addr)
{
  uae_u32 *m;
  addr -= extendedkickmem_start;
  addr &= extendedkickmem_mask;
  m = (uae_u32 *)(extendedkickmemory + addr);
  return do_get_mem_long (m);
}
static uae_u32 REGPARAM2 extendedkickmem_wget (uaecptr addr)
{
  uae_u16 *m;
  addr -= extendedkickmem_start;
  addr &= extendedkickmem_mask;
  m = (uae_u16 *)(extendedkickmemory + addr);
  return do_get_mem_word (m);
}
static uae_u32 REGPARAM2 extendedkickmem_bget (uaecptr addr)
{
  addr -= extendedkickmem_start;
  addr &= extendedkickmem_mask;
  return extendedkickmemory[addr];
}
static void REGPARAM2 extendedkickmem_lput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static void REGPARAM2 extendedkickmem_wput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static void REGPARAM2 extendedkickmem_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static int REGPARAM2 extendedkickmem_check (uaecptr addr, uae_u32 size)
{
  addr -= extendedkickmem_start;
  addr &= extendedkickmem_mask;
  return (addr + size) <= extendedkickmem_size;
}
static uae_u8 *REGPARAM2 extendedkickmem_xlate (uaecptr addr)
{
  addr -= extendedkickmem_start;
  addr &= extendedkickmem_mask;
  return extendedkickmemory + addr;
}

static uae_u32 REGPARAM3 extendedkickmem2_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 extendedkickmem2_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 extendedkickmem2_bget (uaecptr) REGPARAM;
static void REGPARAM3 extendedkickmem2_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 extendedkickmem2_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 extendedkickmem2_bput (uaecptr, uae_u32) REGPARAM;
static int REGPARAM3 extendedkickmem2_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *REGPARAM3 extendedkickmem2_xlate (uaecptr addr) REGPARAM;
static uae_u32 REGPARAM2 extendedkickmem2_lget (uaecptr addr)
{
  uae_u32 *m;
  addr -= extendedkickmem2_start;
  addr &= extendedkickmem2_mask;
  m = (uae_u32 *)(extendedkickmemory2 + addr);
  return do_get_mem_long (m);
}
static uae_u32 REGPARAM2 extendedkickmem2_wget (uaecptr addr)
{
  uae_u16 *m;
  addr -= extendedkickmem2_start;
  addr &= extendedkickmem2_mask;
  m = (uae_u16 *)(extendedkickmemory2 + addr);
  return do_get_mem_word (m);
}
static uae_u32 REGPARAM2 extendedkickmem2_bget (uaecptr addr)
{
  addr -= extendedkickmem2_start;
  addr &= extendedkickmem2_mask;
  return extendedkickmemory2[addr];
}
static void REGPARAM2 extendedkickmem2_lput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static void REGPARAM2 extendedkickmem2_wput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static void REGPARAM2 extendedkickmem2_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
  special_mem |= S_WRITE;
#endif
}
static int REGPARAM2 extendedkickmem2_check (uaecptr addr, uae_u32 size)
{
  addr -= extendedkickmem2_start;
  addr &= extendedkickmem2_mask;
  return (addr + size) <= extendedkickmem2_size;
}
static uae_u8 *REGPARAM2 extendedkickmem2_xlate (uaecptr addr)
{
  addr -= extendedkickmem2_start;
  addr &= extendedkickmem2_mask;
  return extendedkickmemory2 + addr;
}


/* Default memory access functions */

int REGPARAM2 default_check (uaecptr a, uae_u32 b)
{
  return 0;
}

static int be_cnt;

uae_u8 *REGPARAM2 default_xlate (uaecptr a)
{
  if (quit_program == 0) {
    /* do this only in 68010+ mode, there are some tricky A500 programs.. */
    if(currprefs.cpu_model > 68000 || !currprefs.cpu_compatible) {
			if (be_cnt < 3) {
        write_log (_T("Your Amiga program just did something terribly stupid %08X PC=%08X\n"), a, M68K_GETPC);
      }
			be_cnt++;
			if (be_cnt > 1000) {
        uae_reset (0);
				be_cnt = 0;
			} else {
				regs.panic = 1;
				regs.panic_pc = m68k_getpc (regs);
				regs.panic_addr = a;
				set_special (regs, SPCFLAG_BRK);
			}
    }
  }
  return kickmem_xlate (2);	/* So we don't crash. */
}

/* Address banks */

addrbank dummy_bank = {
  dummy_lget, dummy_wget, dummy_bget,
  dummy_lput, dummy_wput, dummy_bput,
  default_xlate, dummy_check, NULL, NULL,
  dummy_lgeti, dummy_wgeti, ABFLAG_NONE
};

addrbank chipmem_bank = {
  chipmem_lget, chipmem_wget, chipmem_bget,
  chipmem_lput, chipmem_wput, chipmem_bput,
	chipmem_xlate, chipmem_check, NULL, _T("Chip memory"),
  chipmem_lget, chipmem_wget, ABFLAG_RAM
};

addrbank bogomem_bank = {
  bogomem_lget, bogomem_wget, bogomem_bget,
  bogomem_lput, bogomem_wput, bogomem_bput,
	bogomem_xlate, bogomem_check, NULL, _T("Slow memory"),
  bogomem_lget, bogomem_wget, ABFLAG_RAM
};

addrbank kickmem_bank = {
  kickmem_lget, kickmem_wget, kickmem_bget,
  kickmem_lput, kickmem_wput, kickmem_bput,
	kickmem_xlate, kickmem_check, NULL, _T("Kickstart ROM"),
  kickmem_lget, kickmem_wget, ABFLAG_ROM
};

addrbank extendedkickmem_bank = {
  extendedkickmem_lget, extendedkickmem_wget, extendedkickmem_bget,
  extendedkickmem_lput, extendedkickmem_wput, extendedkickmem_bput,
	extendedkickmem_xlate, extendedkickmem_check, NULL, _T("Extended Kickstart ROM"),
  extendedkickmem_lget, extendedkickmem_wget, ABFLAG_ROM
};
addrbank extendedkickmem2_bank = {
  extendedkickmem2_lget, extendedkickmem2_wget, extendedkickmem2_bget,
  extendedkickmem2_lput, extendedkickmem2_wput, extendedkickmem2_bput,
	extendedkickmem2_xlate, extendedkickmem2_check, NULL, _T("Extended 2nd Kickstart ROM"),
  extendedkickmem2_lget, extendedkickmem2_wget, ABFLAG_ROM
};


static uae_char *kickstring = "exec.library";
static int read_kickstart (struct zfile *f, uae_u8 *mem, int size, int dochecksum, int noalias)
{
  uae_char buffer[20];
  int i, j, oldpos;
  int cr = 0, kickdisk = 0;

  if (size < 0) {
  	zfile_fseek (f, 0, SEEK_END);
  	size = zfile_ftell (f) & ~0x3ff;
  	zfile_fseek (f, 0, SEEK_SET);
  }
  oldpos = zfile_ftell (f);
  i = zfile_fread (buffer, 1, 11, f);
  if (!memcmp(buffer, "KICK", 4)) {
    zfile_fseek (f, 512, SEEK_SET);
    kickdisk = 1;
#if 0
  } else if (size >= 524288 && !memcmp (buffer, "AMIG", 4)) {
  	/* ReKick */
  	zfile_fseek (f, oldpos + 0x6c, SEEK_SET);
  	cr = 2;
#endif
  } else if (memcmp ((uae_char*)buffer, "AMIROMTYPE1", 11) != 0) {
    zfile_fseek (f, oldpos, SEEK_SET);
  } else {
  	cloanto_rom = 1;
  	cr = 1;
  }

  memset (mem, 0, size);
  for (i = 0; i < 8; i++)
  	mem[size - 16 + i * 2 + 1] = 0x18 + i;
  mem[size - 20] = size >> 24;
  mem[size - 19] = size >> 16;
  mem[size - 18] = size >>  8;
  mem[size - 17] = size >>  0;

  i = zfile_fread (mem, 1, size, f);

  if (kickdisk && i > 262144)
    i = 262144;
#if 0
  if (i >= 262144 && (i != 262144 && i != 524288 && i != 524288 * 2 && i != 524288 * 4)) {
    notify_user (NUMSG_KSROMREADERROR);
    return 0;
  }
#endif
  if (i < size - 20)
  	kickstart_fix_checksum (mem, size);

  j = 1;
  while (j < i)
  	j <<= 1;
  i = j;

  if (!noalias && i == size / 2)
    memcpy (mem + size / 2, mem, size / 2);

  if (cr) {
    if(!decode_rom (mem, size, cr, i))
      return 0;
  }

  for (j = 0; j < 256 && i >= 262144; j++) {
  	if (!memcmp (mem + j, kickstring, strlen (kickstring) + 1))
	    break;
  }

  if (j == 256 || i < 262144)
  	dochecksum = 0;
  if (dochecksum)
  	kickstart_checksum (mem, size);
  return i;
}

static bool load_extendedkickstart (const TCHAR *romextfile, int type)
{
  struct zfile *f;
  int size, off;
	bool ret = false;

  if (_tcslen (romextfile) == 0)
    return false;
  f = read_rom_name (romextfile);
  if (!f) {
	  notify_user (NUMSG_NOEXTROM);
	  return false;
  }
  zfile_fseek (f, 0, SEEK_END);
  size = zfile_ftell (f);
	extendedkickmem_size = 524288;
  off = 0;
	if (type == 0) {
    if (size > 300000) {
    	extendedkickmem_type = EXTENDED_ROM_CD32;
    } else if (need_uae_boot_rom () != 0xf00000) {
	    extendedkickmem_type = EXTENDED_ROM_CDTV;
    } 
  } else {
		extendedkickmem_type = type;
	}
	if (extendedkickmem_type) {
    zfile_fseek (f, off, SEEK_SET);
    switch (extendedkickmem_type) {
    case EXTENDED_ROM_CDTV:
      extendedkickmem_start = 0xf00000;
      extendedkickmemory = mapped_malloc (extendedkickmem_size, _T("rom_f0"));
	    extendedkickmem_bank.baseaddr = extendedkickmemory;
	    break;
    case EXTENDED_ROM_CD32:
      extendedkickmem_start = 0xe00000;
	    extendedkickmemory = mapped_malloc (extendedkickmem_size, _T("rom_e0"));
	    extendedkickmem_bank.baseaddr = extendedkickmemory;
	    break;
    }
		if (extendedkickmemory) {
      read_kickstart (f, extendedkickmemory, extendedkickmem_size, 0, 1);
      extendedkickmem_mask = extendedkickmem_size - 1;
			ret = true;
		}
	}
  zfile_fclose (f);
  return ret;
}

/* disable incompatible drivers */
static int patch_residents (uae_u8 *kickmemory, int size)
{
  int i, j, patched = 0;
  uae_char *residents[] = { "NCR scsi.device", "scsi.device", "carddisk.device", "card.resource", 0 };
  // "scsi.device", "carddisk.device", "card.resource" };
  uaecptr base = size == 524288 ? 0xf80000 : 0xfc0000;

  for (i = 0; i < size - 100; i++) {
  	if (kickmemory[i] == 0x4a && kickmemory[i + 1] == 0xfc) {
	    uaecptr addr;
	    addr = (kickmemory[i + 2] << 24) | (kickmemory[i + 3] << 16) | (kickmemory[i + 4] << 8) | (kickmemory[i + 5] << 0);
	    if (addr != i + base)
    		continue;
	    addr = (kickmemory[i + 14] << 24) | (kickmemory[i + 15] << 16) | (kickmemory[i + 16] << 8) | (kickmemory[i + 17] << 0);
	    if (addr >= base && addr < base + size) {
    		j = 0;
    		while (residents[j]) {
  		    if (!memcmp (residents[j], kickmemory + addr - base, strlen (residents[j]) + 1)) {
      			write_log (_T("KSPatcher: '%s' at %08X disabled\n"), residents[j], i + base);
      			kickmemory[i] = 0x4b; /* destroy RTC_MATCHWORD */
      			patched++;
      			break;
  		    }
  		    j++;
    		}
	    }	
  	}
  }
  return patched;
}

static void patch_kick(void)
{
  int patched = 0;
  patched += patch_residents (kickmemory, kickmem_size);
  if (extendedkickmemory) {
    patched += patch_residents (extendedkickmemory, extendedkickmem_size);
    if (patched)
      kickstart_fix_checksum (extendedkickmemory, extendedkickmem_size);
  }
  if (patched)
  	kickstart_fix_checksum (kickmemory, kickmem_size);
}

extern unsigned char arosrom[];
extern unsigned int arosrom_len;
static bool load_kickstart_replacement (void)
{
	struct zfile *f;
	
	f = zfile_fopen_data (_T("aros.gz"), arosrom_len, arosrom);
	if (!f)
		return false;
	f = zfile_gunzip (f);
	if (!f)
		return false;
	kickmem_mask = 0x80000 - 1;
	kickmem_size = 0x80000;
	extendedkickmem_size = 0x80000;
	extendedkickmem_type = EXTENDED_ROM_KS;
	extendedkickmemory = mapped_malloc (extendedkickmem_size, _T("rom_e0"));
	extendedkickmem_bank.baseaddr = extendedkickmemory;
	read_kickstart (f, extendedkickmemory, extendedkickmem_size, 0, 1);
	extendedkickmem_mask = extendedkickmem_size - 1;
	read_kickstart (f, kickmemory, 0x80000, 1, 0);
	zfile_fclose (f);
	return true;
}

static int load_kickstart (void)
{
  struct zfile *f;
  TCHAR tmprom[MAX_DPATH], tmprom2[MAX_DPATH];
  int patched = 0;

  cloanto_rom = 0;
	if (!_tcscmp (currprefs.romfile, _T(":AROS")))
	  return load_kickstart_replacement ();
  f = read_rom_name (currprefs.romfile);
  _tcscpy (tmprom, currprefs.romfile);
  if (f == NULL) {
  	_stprintf (tmprom2, _T("%s%s"), start_path_data, currprefs.romfile);
  	f = rom_fopen (tmprom2, _T("rb"), ZFD_NORMAL);
  	if (f == NULL) {
	    _stprintf (currprefs.romfile, _T("%sroms/kick.rom"), start_path_data);
    	f = rom_fopen (currprefs.romfile, _T("rb"), ZFD_NORMAL);
    	if (f == NULL) {
    		_stprintf (currprefs.romfile, _T("%skick.rom"), start_path_data);
	      f = rom_fopen( currprefs.romfile, _T("rb"), ZFD_NORMAL);
				if (f == NULL)
					f = read_rom_name_guess (tmprom);
      }
  	} else {
    _tcscpy (currprefs.romfile, tmprom2);
    }
  }
  addkeydir (currprefs.romfile);
	if (f == NULL) /* still no luck */
	  goto err;

  if (f != NULL) {
	  int filesize, size, maxsize;
  	int kspos = 524288;
  	int extpos = 0;

	  maxsize = 524288;
	  zfile_fseek (f, 0, SEEK_END);
	  filesize = zfile_ftell (f);
	  zfile_fseek (f, 0, SEEK_SET);
	  if (filesize == 1760 * 512) {
      filesize = 262144;
      maxsize = 262144;
	  }
	  if (filesize == 524288 + 8) {
	    /* GVP 0xf0 kickstart */
	    zfile_fseek (f, 8, SEEK_SET);
	  }
	  if (filesize >= 524288 * 2) {
      struct romdata *rd = getromdatabyzfile(f);
      zfile_fseek (f, kspos, SEEK_SET);
	  }
	  if (filesize >= 524288 * 4) {
	    kspos = 524288 * 3;
	    extpos = 0;
	    zfile_fseek (f, kspos, SEEK_SET);
	  }
	  size = read_kickstart (f, kickmemory, maxsize, 1, 0);
    if (size == 0)
    	goto err;
    kickmem_mask = size - 1;
  	kickmem_size = size;
  	if (filesize >= 524288 * 2 && !extendedkickmem_type) {
	    extendedkickmem_size = 0x80000;
	    extendedkickmem_type = EXTENDED_ROM_KS;
	    extendedkickmemory = mapped_malloc (extendedkickmem_size, _T("rom_e0"));
	    extendedkickmem_bank.baseaddr = extendedkickmemory;
	    zfile_fseek (f, extpos, SEEK_SET);
	    read_kickstart (f, extendedkickmemory, extendedkickmem_size, 0, 1);
	    extendedkickmem_mask = extendedkickmem_size - 1;
  	}
  	if (filesize > 524288 * 2) {
	    extendedkickmem2_size = 524288 * 2;
	    extendedkickmemory2 = mapped_malloc (extendedkickmem2_size, _T("rom_a8"));
	    extendedkickmem2_bank.baseaddr = extendedkickmemory2;
	    zfile_fseek (f, extpos + 524288, SEEK_SET);
	    read_kickstart (f, extendedkickmemory2, 524288, 0, 1);
	    zfile_fseek (f, extpos + 524288 * 2, SEEK_SET);
	    read_kickstart (f, extendedkickmemory2 + 524288, 524288, 0, 1);
	    extendedkickmem2_mask = extendedkickmem2_size - 1;
  	}
  }

  kickstart_version = (kickmemory[12] << 8) | kickmemory[13];
  if (kickstart_version == 0xffff)
  	kickstart_version = 0;
  zfile_fclose (f);
  return 1;

err:
  _tcscpy (currprefs.romfile, tmprom);
  zfile_fclose (f);
  return 0;
}


static void init_mem_banks (void)
{
  int i;
  for (i = 0; i < MEMORY_BANKS; i++) {
    mem_banks[i] = &dummy_bank;
  }
}

static void allocate_memory (void)
{
  if (allocated_chipmem != currprefs.chipmem_size) {
    int memsize;
   	if (chipmemory)
 	    mapped_free (chipmemory);
		chipmemory = 0;
	  if (currprefs.chipmem_size > 2 * 1024 * 1024)
	    free_fastmemory ();
		
	  memsize = allocated_chipmem = currprefs.chipmem_size;
  	chipmem_full_mask = chipmem_mask = allocated_chipmem - 1;
	  if (memsize > 0x100000 && memsize < 0x200000)
      memsize = 0x200000;
    chipmemory = mapped_malloc (memsize, _T("chip"));
		if (chipmemory == 0) {
			write_log (_T("Fatal error: out of memory for chipmem.\n"));
			allocated_chipmem = 0;
    } else {
	    need_hardreset = 1;
	    if (memsize > allocated_chipmem)
		    memset (chipmemory + allocated_chipmem, 0xff, memsize - allocated_chipmem);
    }
    currprefs.chipset_mask = changed_prefs.chipset_mask;
    chipmem_full_mask = allocated_chipmem - 1;
  }

  if (allocated_bogomem != currprefs.bogomem_size) {
	  if (bogomemory)
	    mapped_free (bogomemory);
		bogomemory = 0;
		
		if(currprefs.bogomem_size > 0x1c0000)
      currprefs.bogomem_size = 0x1c0000;
    if (currprefs.bogomem_size > 0x180000 && ((changed_prefs.chipset_mask & CSMASK_AGA) || (currprefs.cpu_model >= 68020)))
      currprefs.bogomem_size = 0x180000;

	  allocated_bogomem = currprefs.bogomem_size;
		if (allocated_bogomem >= 0x180000)
			allocated_bogomem = 0x200000;
		bogomem_mask = allocated_bogomem - 1;

		if (allocated_bogomem) {
			bogomemory = mapped_malloc (allocated_bogomem, _T("bogo"));
	    if (bogomemory == 0) {
				write_log (_T("Out of memory for bogomem.\n"));
    		allocated_bogomem = 0;
	    }
		}
	  need_hardreset = 1;
	}

  if (savestate_state == STATE_RESTORE) {
    if (bootrom_filepos) {
  	  restore_ram (bootrom_filepos, rtarea);
    }
    restore_ram (chip_filepos, chipmemory);
    if (allocated_bogomem > 0)
	    restore_ram (bogo_filepos, bogomemory);
  }
	chipmem_bank.baseaddr = chipmemory;
	bogomem_bank.baseaddr = bogomemory;
  bootrom_filepos = 0;
  chip_filepos = 0;
  bogo_filepos = 0;
}

void map_overlay (int chip)
{
  int size;
  addrbank *cb;
  int currPC = m68k_getpc(regs);

  size = allocated_chipmem >= 0x180000 ? (allocated_chipmem >> 16) : 32;
  cb = &chipmem_bank;
  if (chip) {
  	map_banks (cb, 0, size, allocated_chipmem);
  } else {
  	addrbank *rb = NULL;
  	if (size < 32)
	    size = 32;
  	cb = &get_mem_bank (0xf00000);
  	if (!rb && cb && (cb->flags & ABFLAG_ROM) && get_word (0xf00000) == 0x1114)
	    rb = cb;
  	cb = &get_mem_bank (0xe00000);
  	if (!rb && cb && (cb->flags & ABFLAG_ROM) && get_word (0xe00000) == 0x1114)
	    rb = cb;
  	if (!rb)
	    rb = &kickmem_bank;
  	map_banks (rb, 0, size, 0x80000);
  }
  if (!isrestore () && valid_address (regs.pc, 4))
    m68k_setpc(regs, currPC);
}

uae_s32 getz2size (struct uae_prefs *p)
{
	uae_u32 start;
	start = p->fastmem_size;
	if (p->rtgmem_size && !p->rtgmem_type) {
		while (start & (p->rtgmem_size - 1) && start < 8 * 1024 * 1024)
			start += 1024 * 1024;
		if (start + p->rtgmem_size > 8 * 1024 * 1024)
			return -1;
	}
	start += p->rtgmem_size;
	return start;
}

uae_u32 getz2endaddr (void)
{
	uae_u32 start;
	start = currprefs.fastmem_size;
	if (currprefs.rtgmem_size && !currprefs.rtgmem_type) {
		if (!start)
			start = 0x00200000;
		while (start & (currprefs.rtgmem_size - 1) && start < 4 * 1024 * 1024)
			start += 1024 * 1024;
	}
	return start + 2 * 1024 * 1024;
}

void memory_reset (void)
{
  int bnk, bnk_end;

  currprefs.chipmem_size = changed_prefs.chipmem_size;
  currprefs.bogomem_size = changed_prefs.bogomem_size;

  need_hardreset = 0;
  /* Use changed_prefs, as m68k_reset is called later.  */
  if (last_address_space_24 != changed_prefs.address_space_24)
  	need_hardreset = 1;

  last_address_space_24 = changed_prefs.address_space_24;

  init_mem_banks ();
  allocate_memory ();

  if (_tcscmp (currprefs.romfile, changed_prefs.romfile) != 0
  	|| _tcscmp (currprefs.romextfile, changed_prefs.romextfile) != 0)
  {
		write_log (_T("ROM loader.. (%s)\n"), currprefs.romfile);
    kickstart_rom = 1;

  	memcpy (currprefs.romfile, changed_prefs.romfile, sizeof currprefs.romfile);
  	memcpy (currprefs.romextfile, changed_prefs.romextfile, sizeof currprefs.romextfile);
  	need_hardreset = 1;
    mapped_free (extendedkickmemory);
  	extendedkickmemory = 0;
    extendedkickmem_size = 0;
  	extendedkickmemory2 = 0;
  	extendedkickmem2_size = 0;
  	extendedkickmem_type = 0;
    load_extendedkickstart (currprefs.romextfile, 0);
  	kickmem_mask = 524288 - 1;
  	if (!load_kickstart ()) {
	    if (_tcslen (currprefs.romfile) > 0) {
				write_log (_T("Failed to open '%s'\n"), currprefs.romfile);
    		notify_user (NUMSG_NOROM);
      }
			load_kickstart_replacement ();
  	} else {
      struct romdata *rd = getromdatabydata (kickmemory, kickmem_size);
  	  if (rd) {
				write_log (_T("Known ROM '%s' loaded\n"), rd->name);
  		  if ((rd->cpu & 3) == 3 && changed_prefs.cpu_model != 68030) {
		      notify_user (NUMSG_KS68030);
		      uae_restart (-1, NULL);
  		  } else if ((rd->cpu & 3) == 1 && changed_prefs.cpu_model < 68020) {
  	      notify_user (NUMSG_KS68EC020);
		      uae_restart (-1, NULL);
	  	  } else if ((rd->cpu & 3) == 2 && (changed_prefs.cpu_model < 68020 || changed_prefs.address_space_24)) {
		      notify_user (NUMSG_KS68020);
		      uae_restart (-1, NULL);
	  	  }
	  	  if (rd->cloanto)
	  	    cloanto_rom = 1;
 	    	kickstart_rom = 0;
	    	if ((rd->type & ROMTYPE_SPECIALKICK | ROMTYPE_KICK) == ROMTYPE_KICK)
	  	    kickstart_rom = 1;
  	  } else {
				write_log (_T("Unknown ROM '%s' loaded\n"), currprefs.romfile);
      }
    }
	  patch_kick ();
		write_log (_T("ROM loader end\n"));
  }

  map_banks (&custom_bank, 0xC0, 0xE0 - 0xC0, 0);
  map_banks (&cia_bank, 0xA0, 32, 0);

  /* D80000 - DDFFFF not mapped (A1000 = custom chips) */
  map_banks (&dummy_bank, 0xD8, 6, 0);

  /* map "nothing" to 0x200000 - 0x9FFFFF (0xBEFFFF if Gayle or Fat Gary) */
  bnk = allocated_chipmem >> 16;
  if (bnk < 0x20 + (currprefs.fastmem_size >> 16))
     bnk = 0x20 + (currprefs.fastmem_size >> 16);
  bnk_end = (currprefs.chipset_mask & CSMASK_AGA) ? 0xBF : 0xA0;
  map_banks (&dummy_bank, bnk, bnk_end - bnk, 0);
  if (currprefs.chipset_mask & CSMASK_AGA) {
     map_banks (&dummy_bank, 0xc0, 0xd8 - 0xc0, 0);
  }

  if (bogomemory != 0) {
	  int t = currprefs.bogomem_size >> 16;
	  if (t > 0x1C)
		  t = 0x1C;
	  if (t > 0x18 && ((currprefs.chipset_mask & CSMASK_AGA) || (currprefs.cpu_model >= 68020 && !currprefs.address_space_24)))
		  t = 0x18;
    map_banks (&bogomem_bank, 0xC0, t, 0);
  }
  map_banks (&clock_bank, 0xDC, 1, 0);

  map_banks (&kickmem_bank, 0xF8, 8, 0);
  /* map beta Kickstarts at 0x200000/0xC00000/0xF00000 */
  if (kickmemory[0] == 0x11 && kickmemory[2] == 0x4e && kickmemory[3] == 0xf9 && kickmemory[4] == 0x00) {
     uae_u32 addr = kickmemory[5];
    if (addr == 0x20 && allocated_chipmem <= 0x200000 && allocated_fastmem == 0)
      map_banks (&kickmem_bank, addr, 8, 0);
	  if (addr == 0xC0 && allocated_bogomem == 0)
      map_banks (&kickmem_bank, addr, 8, 0);
	  if (addr == 0xF0)
      map_banks (&kickmem_bank, addr, 8, 0);
  }

#ifdef AUTOCONFIG
  map_banks (&expamem_bank, 0xE8, 1, 0);
#endif

  /* Map the chipmem into all of the lower 8MB */
  map_overlay (1);

  switch (extendedkickmem_type) {
    case EXTENDED_ROM_KS:
      map_banks (&extendedkickmem_bank, 0xE0, 8, 0);
      break;
  }
#ifdef AUTOCONFIG
  if (need_uae_boot_rom ())
  	map_banks (&rtarea_bank, rtarea_base >> 16, 1, 0);
#endif

  if ((cloanto_rom) && !extendedkickmem_type)
    map_banks (&kickmem_bank, 0xE0, 8, 0);
  write_log (_T("memory init end\n"));
}

void memory_init (void)
{
  allocated_chipmem = 0;
  allocated_bogomem = 0;
  kickmemory = 0;
  extendedkickmemory = 0;
  extendedkickmem_size = 0;
  extendedkickmemory2 = 0;
  extendedkickmem2_size = 0;
  extendedkickmem_type = 0;
  chipmemory = 0;
  bogomemory = 0;

  init_mem_banks ();

	kickmemory = mapped_malloc (0x80000, _T("kick"));
  memset (kickmemory, 0, 0x80000);
  kickmem_bank.baseaddr = kickmemory;
	_tcscpy (currprefs.romfile, _T("<none>"));
  currprefs.romextfile[0] = 0;
}

void memory_cleanup (void)
{
  if (bogomemory)
  	mapped_free (bogomemory);
  if (kickmemory)
  	mapped_free (kickmemory);
  if (chipmemory)
  	mapped_free (chipmemory);
  if(extendedkickmemory)
    mapped_free (extendedkickmemory);
  if(extendedkickmemory2)
    mapped_free (extendedkickmemory2);
  
  bogomemory = 0;
  kickmemory = 0;
  chipmemory = 0;
  extendedkickmemory = 0;
  extendedkickmemory2 = 0;
  
  init_mem_banks ();
}

void memory_hardreset(void)
{
  if (savestate_state == STATE_RESTORE)
  	return;
  if (chipmemory)
  	memset (chipmemory, 0, allocated_chipmem);
  if (bogomemory)
  	memset (bogomemory, 0, allocated_bogomem);
  expansion_clear();
}

void map_banks (addrbank *bank, int start, int size, int realsize)
{
  int bnr;
  unsigned long int hioffs = 0, endhioffs = 0x100;
  addrbank *orgbank = bank;
  uae_u32 realstart = start;

  flush_icache (0, 3);		/* Sure don't want to keep any old mappings around! */

  if (!realsize)
    realsize = size << 16;

  if ((size << 16) < realsize) {
		write_log (_T("Broken mapping, size=%x, realsize=%x\nStart is %x\n"),
	    size, realsize, start);
  }

#ifndef ADDRESS_SPACE_24BIT
  if (start >= 0x100) {
    for (bnr = start; bnr < start + size; bnr++) {
      mem_banks[bnr] = bank;
    }
    return;
  }
#endif
  if (last_address_space_24)
	  endhioffs = 0x10000;
#ifdef ADDRESS_SPACE_24BIT
  endhioffs = 0x100;
#endif
  for (hioffs = 0; hioffs < endhioffs; hioffs += 0x100) {
    for (bnr = start; bnr < start + size; bnr++) {
      mem_banks[bnr + hioffs] = bank;
    }
  }
}

#ifdef SAVESTATE

/* memory save/restore code */

uae_u8 *save_bootrom(int *len)
{
  if (!uae_boot_rom)
  	return 0;
  *len = uae_boot_rom_size;
  return rtarea;
}

uae_u8 *save_cram (int *len)
{
  *len = allocated_chipmem;
  return chipmemory;
}

uae_u8 *save_bram (int *len)
{
  *len = allocated_bogomem;
  return bogomemory;
}

void restore_bootrom (int len, size_t filepos)
{
  bootrom_filepos = filepos;
}

void restore_cram (int len, size_t filepos)
{
  chip_filepos = filepos;
  changed_prefs.chipmem_size = len;
}

void restore_bram (int len, size_t filepos)
{
  bogo_filepos = filepos;
  changed_prefs.bogomem_size = len;
}

uae_u8 *restore_rom (uae_u8 *src)
{
  uae_u32 crc32, mem_start, mem_size, mem_type, version;
  TCHAR *s, *romn;
  int i, crcdet;
	struct romlist *rl = romlist_getit ();

  mem_start = restore_u32 ();
  mem_size = restore_u32 ();
  mem_type = restore_u32 ();
  version = restore_u32 ();
  crc32 = restore_u32 ();
  romn = restore_string ();
  crcdet = 0;
  for (i = 0; i < romlist_count (); i++) {
    if (rl[i].rd->crc32 == crc32 && crc32) {
      if (zfile_exists (rl[i].path)) {
	      switch (mem_type)
        {
        case 0:
        	_tcsncpy (changed_prefs.romfile, rl[i].path, 255);
          break;
	      case 1:
        	_tcsncpy (changed_prefs.romextfile, rl[i].path, 255);
	        break;
        }
			  write_log (_T("ROM '%s' = '%s'\n"), romn, rl[i].path);
        crcdet = 1;
      } else {
			  write_log (_T("ROM '%s' = '%s' invalid rom scanner path!"), romn, rl[i].path);
      }
      break;
    }
  }
  s = restore_string ();
  if (!crcdet) {
    if(zfile_exists (s)) {
      switch (mem_type)
      {
      case 0:
        _tcsncpy (changed_prefs.romfile, s, 255);
        break;
      case 1:
        _tcsncpy (changed_prefs.romextfile, s, 255);
        break;
      }
			write_log (_T("ROM detected (path) as '%s'\n"), s);
      crcdet = 1;
      }
  }
  xfree (s);
  if (!crcdet)
		write_log (_T("WARNING: ROM '%s' not found!\n"), romn);
  xfree (romn);

  return src;
}

uae_u8 *save_rom (int first, int *len, uae_u8 *dstptr)
{
  static int count;
  uae_u8 *dst, *dstbak;
  uae_u8 *mem_real_start;
  uae_u32 version;
  TCHAR *path;
  int mem_start, mem_size, mem_type, saverom;
  int i;
  TCHAR tmpname[1000];

  version = 0;
  saverom = 0;
  if (first)
  	count = 0;
  for (;;) {
  	mem_type = count;
  	mem_size = 0;
  	switch (count) {
  	case 0:		/* Kickstart ROM */
	    mem_start = 0xf80000;
	    mem_real_start = kickmemory;
	    mem_size = kickmem_size;
	    path = currprefs.romfile;
	    /* 256KB or 512KB ROM? */
	    for (i = 0; i < mem_size / 2 - 4; i++) {
    		if (longget (i + mem_start) != longget (i + mem_start + mem_size / 2))
  		    break;
	    }
	    if (i == mem_size / 2 - 4) {
    		mem_size /= 2;
    		mem_start += 262144;
	    }
	    version = longget (mem_start + 12); /* version+revision */
			_stprintf (tmpname, _T("Kickstart %d.%d"), wordget (mem_start + 12), wordget (mem_start + 14));
	    break;
	  case 1: /* Extended ROM */
	    if (!extendedkickmem_type)
		    break;
	    mem_start = extendedkickmem_start;
	    mem_real_start = extendedkickmemory;
	    mem_size = extendedkickmem_size;
	    path = currprefs.romextfile;
			_stprintf (tmpname, _T("Extended"));
	    break;
	  default:
	    return 0;
	  }
	  count++;
	  if (mem_size)
	    break;
  }
  if (dstptr)
  	dstbak = dst = dstptr;
  else
    dstbak = dst = xmalloc (uae_u8, 4 + 4 + 4 + 4 + 4 + 256 + 256 + mem_size);
  save_u32 (mem_start);
  save_u32 (mem_size);
  save_u32 (mem_type);
  save_u32 (version);
  save_u32 (get_crc32 (mem_real_start, mem_size));
  save_string (tmpname);
  save_string (path);
  if (saverom) {
    for (i = 0; i < mem_size; i++)
      *dst++ = byteget (mem_start + i);
  }
  *len = dst - dstbak;
  return dstbak;
}

#endif /* SAVESTATE */

/* memory helpers */

void memcpyha_safe (uaecptr dst, const uae_u8 *src, int size)
{
	if (!addr_valid (_T("memcpyha"), dst, size))
  	return;
  while (size--)
  	put_byte (dst++, *src++);
}
void memcpyha (uaecptr dst, const uae_u8 *src, int size)
{
    while (size--)
	put_byte (dst++, *src++);
}
void memcpyah_safe (uae_u8 *dst, uaecptr src, int size)
{
	if (!addr_valid (_T("memcpyah"), src, size))
  	return;
  while (size--)
  	*dst++ = get_byte(src++);
}
void memcpyah (uae_u8 *dst, uaecptr src, int size)
{
  while (size--)
  	*dst++ = get_byte(src++);
}
uae_char *strcpyah_safe (uae_char *dst, uaecptr src, int maxsize)
{
	uae_char *res = dst;
  uae_u8 b;
  do {
		if (!addr_valid (_T("_tcscpyah"), src, 1))
	    return res;
  	b = get_byte(src++);
  	*dst++ = b;
  	maxsize--;
  	if (maxsize <= 1) {
	    *dst++= 0;
	    break;
  	}
  } while (b);
  return res;
}
uaecptr strcpyha_safe (uaecptr dst, const uae_char *src)
{
  uaecptr res = dst;
  uae_u8 b;
  do {
		if (!addr_valid (_T("_tcscpyha"), dst, 1))
	    return res;
  	b = *src++;
  	put_byte (dst++, b);
  } while (b);
  return res;
}
