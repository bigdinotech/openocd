/***************************************************************************
 *   Copyright (C) 2013-2014 Synopsys, Inc.                                *
 *   Frank Dols <frank.dols@synopsys.com>                                  *
 *   Anton Kolesov <anton.kolesov@synopsys.com>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifndef ARC32_H
#define ARC32_H

#include <helper/time_support.h>
#include <jtag/jtag.h>

#include "algorithm.h"
#include "breakpoints.h"
#include "jtag/interface.h"
#include "register.h"
#include "target.h"
#include "target_request.h"
#include "target_type.h"

#include "arc_dbg.h"
#include "arc_jtag.h"
#include "arc_mem.h"
#include "arc_mntr.h"
#include "arc_ocd.h"
#include "arc_regs.h"

#if defined _WIN32 || defined __CYGWIN__
#include <windows.h>
#define sleep(x) Sleep(x)
#endif


#define ARC32_COMMON_MAGIC	0xB32EB324  /* just a unique number */

/* supported ARC processor types */
enum arc_processor_type {
	ARCEM_NUM	= 1,
	ARC600_NUM,
	ARC700_NUM
};

#define ARCEM_STR	"arc-em"
#define ARC600_STR	"arc600"
#define ARC700_STR	"arc700"

typedef uint32_t arc32_address_t;

/* ARC core ARCompatISA register set */
enum arc32_isa_mode {
	ARC32_ISA_ARC32 = 0,
	ARC32_ISA_ARC16 = 1,
};

/* offsets into arc32 core register cache */
struct arc32_comparator {
	int used;
	uint32_t bp_value;
	uint32_t reg_address;
};

struct arc32_common {
	uint32_t common_magic;
	void *arch_info;

	struct arc_jtag jtag_info;

	struct reg_cache *core_cache;

	enum arc_processor_type processor_type;
	enum arc32_isa_mode     isa_mode;

	/* working area for fastdata access */
	struct working_area *fast_data_area;

	int bp_scanned;
	int num_inst_bpoints;
	int num_data_bpoints;
	int num_inst_bpoints_avail;
	int num_data_bpoints_avail;

	struct arc32_comparator *inst_break_list;
	struct arc32_comparator *data_break_list;

	/* Cache control */
	bool has_dcache;
	/* If true, then D$ has been already flushed since core has been
	 * halted. */
	bool dcache_flushed;
	/* If true, then caches have been already flushed since core has been
	 * halted. */
	bool cache_invalidated;

	/* Whether to support old ARC GDB that doesn't understand XML target
	 * descriptions. */
	bool gdb_compatibility_mode;
	/* Store values of BCR permanently. */
	struct bcr_set_t bcr_set;

	/* CCM memory regions (optional). */
	arc32_address_t iccm0_start;
	arc32_address_t iccm0_end;
	arc32_address_t iccm1_start;
	arc32_address_t iccm1_end;
	arc32_address_t dccm_start;
	arc32_address_t dccm_end;
};

//#define ARC32_FASTDATA_HANDLER_SIZE	0x8000 /* haps51 */
#define ARC32_FASTDATA_HANDLER_SIZE	0x10000  /* 64Kb */

/* ARC 32bits Compact v2 opcodes */
#define ARC32_SDBBP 0x256F003F  /* BRK */

/* ARC 16bits Compact v2 opcodes */
#define ARC16_SDBBP 0x7FFF      /* BRK_S */

/* Borrowed from nds32.h */
#define CHECK_RETVAL(action)			\
	do {					\
		int __retval = (action);	\
		if (__retval != ERROR_OK) {	\
			LOG_DEBUG("error while calling \"%s\"",	\
				# action);     \
			return __retval;	\
		}				\
	} while (0)

#define JIM_CHECK_RETVAL(action)		\
	do {					\
		int __retval = (action);	\
		if (__retval != JIM_OK) {	\
			LOG_DEBUG("error while calling \"%s\"",	\
				# action);     \
			return __retval;	\
		}				\
	} while (0)


#define ARC_COMMON_MAGIC 0x1A471AC5  /* just a unique number */

struct arc_common {
	int common_magic;
	bool is_4wire;
	struct arc32_common arc32;
};

/* ----- Inlined functions ------------------------------------------------- */

/**
 * Convert data in host endianness to the middle endian. This is required to
 * write 4-byte instructions.
 */
static inline void arc32_h_u32_to_me(uint8_t* buf, int val)
{
    buf[1] = (uint8_t) (val >> 24);
    buf[0] = (uint8_t) (val >> 16);
    buf[3] = (uint8_t) (val >> 8);
    buf[2] = (uint8_t) (val >> 0);
}

/**
 * Convert data in middle endian to host endian. This is required to read 32-bit
 * instruction from little endian ARCs.
 */
static inline uint32_t arc32_me_to_h_u32(const uint8_t* buf)
{
    return (uint32_t)(buf[2] | buf[3] << 8 | buf[0] << 16 | buf[1] << 24);
}

static inline struct arc32_common * target_to_arc32(struct target *target)
{
	return target->arch_info;
}

/* ----- Exported functions ------------------------------------------------ */

int arc32_init_arch_info(struct target *target, struct arc32_common *arc32,
	struct jtag_tap *tap);

struct reg_cache *arc32_build_reg_cache(struct target *target);

int arc32_save_context(struct target *target);
int arc32_restore_context(struct target *target);

int arc32_enable_interrupts(struct target *target, int enable);

int arc32_start_core(struct target *target);

int arc32_config_step(struct target *target, int enable_step);

int arc32_cache_invalidate(struct target *target);

int arc32_wait_until_core_is_halted(struct target *target);

int arc32_print_core_state(struct target *target);
int arc32_arch_state(struct target *target);
int arc32_get_current_pc(struct target *target);
int arc32_dcache_flush(struct target *target);

int arc32_reset_caches_states(struct target *target);

int arc32_write_instruction_u32(struct target *target, uint32_t address,
		uint32_t instr);
int arc32_read_instruction_u32(struct target *target, uint32_t address,
		uint32_t *value);

int arc32_configure(struct target *target);

#endif /* ARC32_H */
