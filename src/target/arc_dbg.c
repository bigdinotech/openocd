/***************************************************************************
 *   Copyright (C) 2013-2014 Synopsys, Inc.                                *
 *   Frank Dols <frank.dols@synopsys.com>                                  *
 *   Mischa Jonker <mischa.jonker@synopsys.com>                            *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arc32.h"

/* ----- Supporting functions ---------------------------------------------- */

static int arc_dbg_set_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->inst_break_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		int bp_num = 0;

		while (comparator_list[bp_num].used && (bp_num < arc32->num_inst_bpoints))
			bp_num++;

		if (bp_num >= arc32->num_inst_bpoints) {
			LOG_ERROR("Can not find free FP Comparator(bpid: %" PRIu32 ")",
					breakpoint->unique_id);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		breakpoint->set = bp_num + 1;
		comparator_list[bp_num].used = 1;
		comparator_list[bp_num].bp_value = breakpoint->address;
		target_write_u32(target, comparator_list[bp_num].reg_address,
				comparator_list[bp_num].bp_value);
		target_write_u32(target, comparator_list[bp_num].reg_address + 0x08, 0x00000000);
		target_write_u32(target, comparator_list[bp_num].reg_address + 0x18, 1);

		LOG_DEBUG("bpid: %" PRIu32 ", bp_num %i bp_value 0x%" PRIx32,
				  breakpoint->unique_id,
				  bp_num, comparator_list[bp_num].bp_value);
	} else if (breakpoint->type == BKPT_SOFT) {
		LOG_DEBUG("bpid: %" PRIu32, breakpoint->unique_id);

		if (breakpoint->length == 4) { /* WAS: == 4) { but we have only 32 bits access !!*/
			uint32_t verify = 0xffffffff;

			CHECK_RETVAL(target_read_buffer(target, breakpoint->address, breakpoint->length,
					breakpoint->orig_instr));

			CHECK_RETVAL(arc32_write_instruction_u32(target, breakpoint->address,
					ARC32_SDBBP));

			CHECK_RETVAL(arc32_read_instruction_u32(target, breakpoint->address, &verify));

			if (verify != ARC32_SDBBP) {
				LOG_ERROR("Unable to set 32bit breakpoint at address %08" PRIx32
						" - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		} else {
			uint16_t verify = 0xffff;

			CHECK_RETVAL(target_read_buffer(target, breakpoint->address, breakpoint->length,
					breakpoint->orig_instr));
			CHECK_RETVAL(target_write_u16(target, breakpoint->address, ARC16_SDBBP));

			CHECK_RETVAL(target_read_u16(target, breakpoint->address, &verify));
			if (verify != ARC16_SDBBP) {
				LOG_ERROR("Unable to set 16bit breakpoint at address %08" PRIx32
						" - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		}

		/* core instruction cache is now invalid */
		CHECK_RETVAL(arc32_cache_invalidate(target));

		breakpoint->set = 64; /* Any nice value but 0 */
	}

	return ERROR_OK;
}

static int arc_dbg_unset_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->inst_break_list;

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		int bp_num = breakpoint->set - 1;
		if ((bp_num < 0) || (bp_num >= arc32->num_inst_bpoints)) {
			LOG_DEBUG("Invalid FP Comparator number in breakpoint (bpid: %" PRIu32 ")",
					  breakpoint->unique_id);
			return ERROR_OK;
		}
		LOG_DEBUG("bpid: %" PRIu32 " - releasing hw: %i",
				breakpoint->unique_id,
				bp_num);
		comparator_list[bp_num].used = 0;
		comparator_list[bp_num].bp_value = 0;
		target_write_u32(target, comparator_list[bp_num].reg_address + 0x18, 0);

	} else {
		/* restore original instruction (kept in target endianness) */
		LOG_DEBUG("bpid: %" PRIu32, breakpoint->unique_id);
		if (breakpoint->length == 4) {
			uint32_t current_instr;

			/* check that user program has not modified breakpoint instruction */
			CHECK_RETVAL(arc32_read_instruction_u32(target, breakpoint->address, &current_instr));

			if (current_instr == ARC32_SDBBP) {
				CHECK_RETVAL(target_write_buffer(target, breakpoint->address,
					breakpoint->length, breakpoint->orig_instr));
			} else {
				LOG_WARNING("Software breakpoint @%" PRIx32
					" has been overwritten outside of debugger.", breakpoint->address);
			}
		} else {
			uint16_t current_instr;

			/* check that user program has not modified breakpoint instruction */
			CHECK_RETVAL(target_read_memory(target, breakpoint->address, 2, 1,
					(uint8_t *)&current_instr));
			current_instr = target_buffer_get_u16(target, (uint8_t *)&current_instr);
			if (current_instr == ARC16_SDBBP) {
				CHECK_RETVAL(target_write_buffer(target, breakpoint->address,
					breakpoint->length, breakpoint->orig_instr));
			} else {
				LOG_WARNING("Software breakpoint @%" PRIx32
					" has been overwritten outside of debugger.", breakpoint->address);
			}
		}
	}

	/* core instruction cache is now invalid */
	CHECK_RETVAL(arc32_cache_invalidate(target));

	breakpoint->set = 0;

	return ERROR_OK;
}

static void arc_dbg_enable_breakpoints(struct target *target)
{
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint) {
		if (breakpoint->set == 0)
			arc_dbg_set_breakpoint(target, breakpoint);
		breakpoint = breakpoint->next;
	}
}

static int arc_dbg_set_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->data_break_list;

	int wp_num = 0;
	/*
	 * watchpoint enabled, ignore all byte lanes in value register
	 * and exclude both load and store accesses from  watchpoint
	 * condition evaluation
	*/
	int enable = 1;
	//int enable = EJTAG_DBCn_NOSB | EJTAG_DBCn_NOLB | EJTAG_DBCn_BE |
		//	(0xff << EJTAG_DBCn_BLM_SHIFT);

	if (watchpoint->set) {
		LOG_WARNING("watchpoint already set");
		return ERROR_OK;
	}

	while (comparator_list[wp_num].used && (wp_num < arc32->num_data_bpoints))
		wp_num++;
	if (wp_num >= arc32->num_data_bpoints) {
		LOG_ERROR("Can not find free FP Comparator");
		return ERROR_FAIL;
	}

	if (watchpoint->length != 4) {
		LOG_ERROR("Only watchpoints of length 4 are supported");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	if (watchpoint->address % 4) {
		LOG_ERROR("Watchpoints address should be word aligned");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

#ifdef NEEDS_PORTING
	switch (watchpoint->rw) {
		case WPT_READ:
			enable &= ~EJTAG_DBCn_NOLB;
			break;
		case WPT_WRITE:
			enable &= ~EJTAG_DBCn_NOSB;
			break;
		case WPT_ACCESS:
			enable &= ~(EJTAG_DBCn_NOLB | EJTAG_DBCn_NOSB);
			break;
		default:
			LOG_ERROR("BUG: watchpoint->rw neither read, write nor access");
	}
#endif

	watchpoint->set = wp_num + 1;
	comparator_list[wp_num].used = 1;
	comparator_list[wp_num].bp_value = watchpoint->address;
	target_write_u32(target, comparator_list[wp_num].reg_address, comparator_list[wp_num].bp_value);
	target_write_u32(target, comparator_list[wp_num].reg_address + 0x08, 0x00000000);
	target_write_u32(target, comparator_list[wp_num].reg_address + 0x10, 0x00000000);
	target_write_u32(target, comparator_list[wp_num].reg_address + 0x18, enable);
	target_write_u32(target, comparator_list[wp_num].reg_address + 0x20, 0);

	LOG_DEBUG("wp_num %i bp_value 0x%" PRIx32, wp_num, comparator_list[wp_num].bp_value);

	return ERROR_OK;
}

static int arc_dbg_unset_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->data_break_list;

	if (!watchpoint->set) {
		LOG_WARNING("watchpoint not set");
		return ERROR_OK;
	}

	int wp_num = watchpoint->set - 1;
	if ((wp_num < 0) || (wp_num >= arc32->num_data_bpoints)) {
		LOG_DEBUG("Invalid FP Comparator number in watchpoint");
		return ERROR_OK;
	}

	comparator_list[wp_num].used = 0;
	comparator_list[wp_num].bp_value = 0;
	target_write_u32(target, comparator_list[wp_num].reg_address + 0x18, 0);
	watchpoint->set = 0;

	return ERROR_OK;
}

static void arc_dbg_enable_watchpoints(struct target *target)
{
	struct watchpoint *watchpoint = target->watchpoints;

	/* set any pending watchpoints */
	while (watchpoint) {
		if (watchpoint->set == 0)
			arc_dbg_set_watchpoint(target, watchpoint);
		watchpoint = watchpoint->next;
	}
}

static int arc_dbg_single_step_core(struct target *target)
{
	arc_dbg_debug_entry(target);

	/* disable interrupts while stepping */
	arc32_enable_interrupts(target, 0);

	/* configure single step mode */
	arc32_config_step(target, 1);

	/* exit debug mode */
	arc_dbg_enter_debug(target);

	return ERROR_OK;
}

/* ----- Exported supporting functions ------s------------------------------- */

int arc_dbg_enter_debug(struct target *target)
{
	uint32_t value;
	struct arc32_common *arc32 = target_to_arc32(target);

	/* Do read-modify-write sequence, or DEBUG.UB will be reset unintentionally. */
	/* TODO: I think this should be moved to halt(). */
	CHECK_RETVAL(arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, &value));
	value |= SET_CORE_FORCE_HALT; /* set the HALT bit */
	CHECK_RETVAL(arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, value));
	alive_sleep(1);

#ifdef DEBUG
	LOG_DEBUG("core stopped (halted) DEGUB-REG: 0x%08" PRIx32, value);
	CHECK_RETVAL(arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_STATUS32_REG, &value));
	LOG_DEBUG("core STATUS32: 0x%08" PRIx32, value);
#endif

	return ERROR_OK;
}

int arc_dbg_examine_debug_reason(struct target *target)
{
	/* Only check for reason if don't know it already. */
	/* BTW After singlestep at this point core is not marked as halted, so
	 * reading from memory to get current instruction won't work anyways. */
	if (DBG_REASON_DBGRQ == target->debug_reason ||
	    DBG_REASON_SINGLESTEP == target->debug_reason) {
		return ERROR_OK;
	}

	/* Ensure that DEBUG register value is in cache */
	struct reg *debug_reg = &(target->reg_cache->reg_list[ARC_REG_DEBUG]);
	if (!debug_reg->valid) {
		CHECK_RETVAL(debug_reg->type->get(debug_reg));
	}

	/* DEBUG.BH is set if core halted due to BRK instruction. */
	uint32_t debug_reg_value = buf_get_u32(debug_reg->value, 0, debug_reg->size);
	if (debug_reg_value & SET_CORE_BREAKPOINT_HALT) {
		target->debug_reason = DBG_REASON_BREAKPOINT;
	}

	return ERROR_OK;
}

int arc_dbg_debug_entry(struct target *target)
{
	uint32_t dpc;
	struct arc32_common *arc32 = target_to_arc32(target);

	/* save current PC */
	CHECK_RETVAL(arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_PC_REG, &dpc));
	arc32->jtag_info.dpc = dpc;
	arc32_save_context(target);

	/* We must reset internal indicators of caches states, otherwise D$/I$
	 * will not be flushed/invalidated when required. */
	CHECK_RETVAL(arc32_reset_caches_states(target));
	CHECK_RETVAL(arc_dbg_examine_debug_reason(target));

	return ERROR_OK;
}

int arc_dbg_exit_debug(struct target *target)
{
	uint32_t value;
	struct arc32_common *arc32 = target_to_arc32(target);

	target->state = TARGET_RUNNING;

	/* raise the Reset Applied bit flag */
	CHECK_RETVAL(arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, &value));
	value |= SET_CORE_RESET_APPLIED; /* set the RA bit */
	CHECK_RETVAL(arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, value));

#ifdef DEBUG
	CHECK_RETVAL(arc32_print_core_state(target));
#endif
	return ERROR_OK;
}

/* ----- Exported functions ------------------------------------------------ */

int arc_dbg_halt(struct target *target)
{
	LOG_DEBUG("target->state: %s", target_state_name(target));

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_WARNING("target was in unknown state when halt was requested");

	if (target->state == TARGET_RESET) {
		if ((jtag_get_reset_config() & RESET_SRST_PULLS_TRST) && jtag_get_srst()) {
			LOG_ERROR("can't request a halt while in reset if nSRST pulls nTRST");
			return ERROR_TARGET_FAILURE;
		} else {
			target->debug_reason = DBG_REASON_DBGRQ;
		}
	}

	/* break (stop) processor */
	CHECK_RETVAL(arc_dbg_enter_debug(target));

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

int arc_dbg_resume(struct target *target, int current, uint32_t address,
	int handle_breakpoints, int debug_execution)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	struct breakpoint *breakpoint = NULL;
	uint32_t resume_pc = 0;

	LOG_DEBUG("current:%i, address:0x%08" PRIx32 ", handle_breakpoints:%i, debug_execution:%i",
		current, address, handle_breakpoints, debug_execution);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution) {
		/* (gdb) continue = execute until we hit break/watch-point */
		LOG_DEBUG("we are in debug execution mode");
		target_free_all_working_areas(target);
		arc_dbg_enable_breakpoints(target);
		arc_dbg_enable_watchpoints(target);
	}

	/* current = 1: continue on current PC, otherwise continue at <address> */
	if (!current) {
		buf_set_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32, address);
		arc32->core_cache->reg_list[ARC_REG_PC].dirty = 1;
		arc32->core_cache->reg_list[ARC_REG_PC].valid = 1;
		LOG_DEBUG("Changing the value of current PC to 0x%08" PRIx32, address);
	}

	if (!current)
		resume_pc = address;
	else
		resume_pc = buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value,
			0, 32);

	arc32_restore_context(target);

	LOG_DEBUG("Target resumes from PC=0x%" PRIx32 ", pc.dirty=%i, pc.valid=%i",
		resume_pc,
		arc32->core_cache->reg_list[ARC_REG_PC].dirty,
		arc32->core_cache->reg_list[ARC_REG_PC].valid);

	/* check if GDB tells to set our PC where to continue from */
	if ((arc32->core_cache->reg_list[ARC_REG_PC].valid == 1) &&
		(resume_pc == buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value,
			0, 32))) {

		uint32_t value;
		value = buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32);
		LOG_DEBUG("resume Core (when start-core) with PC @:0x%08" PRIx32, value);
		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_PC_REG, value);
	}

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%08" PRIx32,
				breakpoint->address);
			arc_dbg_unset_breakpoint(target, breakpoint);
			arc_dbg_single_step_core(target);
			arc_dbg_set_breakpoint(target, breakpoint);
		}
	}

	/* enable interrupts if we are running */
	arc32_enable_interrupts(target, !debug_execution);

	/* exit debug mode */
	arc_dbg_enter_debug(target);
	target->debug_reason = DBG_REASON_NOTHALTED;

	/* ready to get us going again */
	arc32_start_core(target);

	/* registers are now invalid */
	register_cache_invalidate(arc32->core_cache);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%08" PRIx32, resume_pc);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%08" PRIx32, resume_pc);
	}

	return ERROR_OK;
}

int arc_dbg_step(struct target *target, int current, uint32_t address,
	int handle_breakpoints)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct breakpoint *breakpoint = NULL;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current) {
		buf_set_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32, address);
		arc32->core_cache->reg_list[ARC_REG_PC].dirty = 1;
		arc32->core_cache->reg_list[ARC_REG_PC].valid = 1;
	}

	LOG_DEBUG("Target steps one instruction from PC=0x%" PRIx32,
		buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32));

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target,
			buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32));
		if (breakpoint)
			arc_dbg_unset_breakpoint(target, breakpoint);
	}

	/* restore context */
	arc32_restore_context(target);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	/* disable interrupts while stepping */
	arc32_enable_interrupts(target, 0);

	/* exit debug mode */
	arc_dbg_enter_debug(target);

	/* do a single step */
	arc32_config_step(target, 1);

	/* make sure we done our step */
	alive_sleep(1);

	/* registers are now invalid */
	register_cache_invalidate(arc32->core_cache);

	if (breakpoint)
		arc_dbg_set_breakpoint(target, breakpoint);

	LOG_DEBUG("target stepped ");

	/* target_call_event_callbacks() will send a response to GDB that
	 * execution has stopped (packet T05). If target state is not set to
	 * HALTED beforehand, then this creates a race condition: target state
	 * will not be changed to HALTED until next invocation of
	 * arc_ocd_poll(), however GDB can issue next command _before_
	 * arc_ocd_poll() will be invoked. If GDB request requires target to be
	 * halted this request execution will fail. Also it seems that
	 * gdb_server cannot handle this failure properly, causing some
	 * unexpected results instead of error message. Strangely no other
	 * target does this except for ARM11, which sets target state to HALTED
	 * in debug_entry. Thus either every other target is suspect to the
	 * error, or they do something else differently, but I couldn't
	 * understand this. */
	/* TODO (akolesov): Perhaps just move this to arc_dbg_debug_entry?
	 * See nds32_v3_debug_entry. */
	target->state = TARGET_HALTED;
	arc_dbg_debug_entry(target);
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	return ERROR_OK;
}

/* ......................................................................... */

int arc_dbg_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct arc32_common *arc32 = target_to_arc32(target);

	if (breakpoint->type == BKPT_HARD) {
		if (arc32->num_inst_bpoints_avail < 1) {
			LOG_ERROR(" > Hardware breakpoints are not supported in this release.");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		arc32->num_inst_bpoints_avail--;
	}

	if (target->state == TARGET_HALTED) {
		return arc_dbg_set_breakpoint(target, breakpoint);
	} else {
		arc_dbg_enter_debug(target);
		LOG_WARNING(" > core was not halted, please try again.");
		return ERROR_OK;
	}
}

int arc_dbg_add_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	LOG_ERROR("Context breakpoints are NOT SUPPORTED IN THIS RELEASE.");
	return ERROR_OK;
}

int arc_dbg_add_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	LOG_ERROR("Hybryd breakpoints are NOT SUPPORTED IN THIS RELEASE.");
	return ERROR_OK;
}

int arc_dbg_remove_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (breakpoint->set)
		arc_dbg_unset_breakpoint(target, breakpoint);

	if (breakpoint->type == BKPT_HARD)
		arc32->num_inst_bpoints_avail++;

	return ERROR_OK;
}

int arc_dbg_add_watchpoint(struct target *target,
	struct watchpoint *watchpoint)
{
	struct arc32_common *arc32 = target_to_arc32(target);

	if (arc32->num_data_bpoints_avail < 1) {
		LOG_INFO("Hardware watchpoints are not supported in this release.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	arc32->num_data_bpoints_avail--;

	arc_dbg_set_watchpoint(target, watchpoint);

	return ERROR_OK;
}

int arc_dbg_remove_watchpoint(struct target *target,
	struct watchpoint *watchpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (watchpoint->set)
		arc_dbg_unset_watchpoint(target, watchpoint);

	arc32->num_data_bpoints_avail++;

	return ERROR_OK;
}
