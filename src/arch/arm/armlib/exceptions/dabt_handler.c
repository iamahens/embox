/**
 * @file
 * @brief
 *
 * @author Aleksey Zhmulin
 * @date 06.11.22
 */

#include <inttypes.h>

#include <kernel/printk.h>
#include <hal/test/traps_core.h>
#include <arm/exception.h>

extern uint32_t _get_mmu_data_fault_status(void);

fault_handler_t arm_data_fault_table[0x10];

void arm_dabt_handler(excpt_context_t *ctx) {
	uint32_t fault_status;

	fault_status = _get_mmu_data_fault_status() & 0xf;
	if (arm_data_fault_table[fault_status]) {
		arm_data_fault_table[fault_status](fault_status, ctx);
	}
	else {
		printk("\nUnresolvable data abort exception!\n");
		printk("Fault status = %" PRIu32 "\n", fault_status);
		PRINT_PTREGS(&ctx->ptregs);
		while (1)
			;
	}
}
