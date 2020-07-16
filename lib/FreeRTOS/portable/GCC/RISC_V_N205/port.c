/*
 * FreeRTOS Kernel V10.0.1
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "portmacro.h"



#include <stdio.h>

#include "n200_func.h"
#include "soc.h"
#include "riscv_encoding.h"
#include "n200_timer.h"
#include "n200_eclic.h"

/* Standard Includes */
#include <stdlib.h>
#include <unistd.h>
//#include "printf.h"

/* Each task maintains its own interrupt status in the critical nesting
variable. */
UBaseType_t uxCriticalNesting = 0xaaaaaaaa;

#if USER_MODE_TASKS
	unsigned long MSTATUS_INIT = (MSTATUS_MPIE);
#else
	unsigned long MSTATUS_INIT = (MSTATUS_MPP | MSTATUS_MPIE);
#endif


/*
 * Used to catch tasks that attempt to return from their implementing function.
 */
static void prvTaskExitError( void );
unsigned long ulSynchTrap(unsigned long mcause, unsigned long sp, unsigned long arg1);
void vPortSysTickHandler(void);
void vPortSetupTimer(void);
void vPortSetup(void);

/* System Call Trap */
//ECALL macro stores argument in a2
unsigned long ulSynchTrap(unsigned long mcause, unsigned long sp, unsigned long arg1)	{

	switch (mcause&0X00000fff)	{
		//on User and Machine ECALL, handler the request
		case 8:
		case 11:
			if (arg1 == IRQ_DISABLE)	{
				//zero out mstatus.mpie
				clear_csr(mstatus,MSTATUS_MPIE);

			} else if(arg1 == IRQ_ENABLE)	{
				//set mstatus.mpie
				set_csr(mstatus,MSTATUS_MPIE);

			} else if(arg1 == PORT_YIELD)		{
				//always yield from machine mode
				//fix up mepc on sync trap
				unsigned long epc = read_csr(mepc);
				vPortYield(sp,epc+4); //never returns

			} else if(arg1 == PORT_YIELD_TO_RA)	{

				vPortYield(sp,(*(unsigned long*)(sp+1*sizeof(sp)))); //never returns
			}

			break;
		default:
                printf("In trap handler, the mcause is 0x%lx\n",(mcause&0X00000fff) );
                printf("In trap handler, the mepc is 0x%lx\n", read_csr(mepc));
                printf("In trap handler, the mtval is 0x%lx\n", read_csr(mbadaddr));
			//_exit(mcause);
			do {}while(1);
	}

	//fix mepc and return
	unsigned long epc = read_csr(mepc);

	write_csr(mepc,epc+4);
	return sp;
}


void vPortEnterCritical( void )
{
	//printf("vPortEnterCritical\n");
	#if USER_MODE_TASKS
		ECALL(IRQ_DISABLE);
	#else
		portDISABLE_INTERRUPTS();
		//eclic_set_mth ((configMAX_SYSCALL_INTERRUPT_PRIORITY)<<4);
	#endif

	uxCriticalNesting++;
}
/*-----------------------------------------------------------*/

void vPortExitCritical( void )
{
	configASSERT( uxCriticalNesting );
	uxCriticalNesting--;
	if ( uxCriticalNesting == 0 )
	{
		#if USER_MODE_TASKS
			ECALL(IRQ_ENABLE);
		#else
			//eclic_set_mth (0);
			portENABLE_INTERRUPTS()	;
		#endif
	}
	return;
}
/*-----------------------------------------------------------*/


/*-----------------------------------------------------------*/

/* Clear current interrupt mask and set given mask */
void vPortClearInterruptMask(int int_mask)
{
	eclic_set_mth (int_mask);


}
/*-----------------------------------------------------------*/

/* Set interrupt mask and return current interrupt enable register */
int xPortSetInterruptMask()
{
	int int_mask=0;
	int_mask=eclic_get_mth();

	eclic_set_mth ((configMAX_SYSCALL_INTERRUPT_PRIORITY)<<4);
	return int_mask;
}

/*-----------------------------------------------------------*/
/*
 * See header file for description.
 */
StackType_t *pxPortInitialiseStack( StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters )
{
	/* Simulate the stack frame as it would be created by a context switch
	interrupt. */

	//register int *tp asm("x3");
	pxTopOfStack--;
	*pxTopOfStack = (portSTACK_TYPE)pxCode;			/* Start address */

	//set the initial mstatus value
	pxTopOfStack--;
	*pxTopOfStack = MSTATUS_INIT;

	pxTopOfStack -= 22;
	*pxTopOfStack = (portSTACK_TYPE)pvParameters;	/* Register a0 */
	//pxTopOfStack -= 7;
	//*pxTopOfStack = (portSTACK_TYPE)tp; /* Register thread pointer */
	//pxTopOfStack -= 2;
	pxTopOfStack -=9;
	*pxTopOfStack = (portSTACK_TYPE)prvTaskExitError; /* Register ra */
	pxTopOfStack--;

	return pxTopOfStack;
}
/*-----------------------------------------------------------*/


void prvTaskExitError( void )
{
	/* A function that implements a task must not exit or attempt to return to
	its caller as there is nothing to return to.  If a task wants to exit it
	should instead call vTaskDelete( NULL ).
	Artificially force an assert() to be triggered if configASSERT() is
	defined, then stop here so application writers can catch the error. */
	configASSERT( uxCriticalNesting == ~0UL );
	portDISABLE_INTERRUPTS();
	printf("prvTaskExitError\n");
	for ( ;; );
}
/*-----------------------------------------------------------*/



/*Entry Point for Machine Timer Interrupt Handler*/
//Bob: add the function argument int_num

void vPortSysTickHandler(void){
	static uint64_t then = 0;
    volatile uint64_t * mtime       = (uint64_t*) (TIMER_CTRL_ADDR + TIMER_MTIME);
    volatile uint64_t * mtimecmp    = (uint64_t*) (TIMER_CTRL_ADDR + TIMER_MTIMECMP);

	if (then != 0)  {
		//next timer irq is 1 second from previous
		then += (configRTC_CLOCK_HZ / configTICK_RATE_HZ);
	} else{ //first time setting the timer
		uint64_t now = *mtime;
		then = now + (configRTC_CLOCK_HZ / configTICK_RATE_HZ);
	}
	*mtimecmp = then;

	/* Increment the RTOS tick. */
	if ( xTaskIncrementTick() != pdFALSE )
	{
		portYIELD();
		//vTaskSwitchContext();
	}
}
/*-----------------------------------------------------------*/


void vPortSetupTimer(void)	{
    uint8_t mtime_intattr;
    // Set the machine timer
    //Bob: update it to TMR
    volatile uint64_t * mtime       = (uint64_t*) (TIMER_CTRL_ADDR + TIMER_MTIME);
    volatile uint64_t * mtimecmp    = (uint64_t*) (TIMER_CTRL_ADDR + TIMER_MTIMECMP);
    uint64_t now = *mtime;
    uint64_t then = now + (configRTC_CLOCK_HZ / configTICK_RATE_HZ);
    *mtimecmp = then;
	//print_eclic();
    mtime_intattr=eclic_get_intattr (ECLIC_INT_MTIP);
    mtime_intattr|=ECLIC_INT_ATTR_SHV | ECLIC_INT_ATTR_MACH_MODE;
    mtime_intattr|= ECLIC_INT_ATTR_TRIG_EDGE;
    eclic_set_intattr(ECLIC_INT_MTIP,mtime_intattr);
    eclic_enable_interrupt (ECLIC_INT_MTIP);
    //eclic_set_nlbits(4);
    //eclic_set_irq_lvl_abs(ECLIC_INT_MTIP,1);
	eclic_set_intctrl(ECLIC_INT_MTIP, 10 << 4);

    set_csr(mstatus, MSTATUS_MIE);
}
/*-----------------------------------------------------------*/


void vPortSetup(void)	{
	vPortSetupTimer();
	uxCriticalNesting = 0;
}
/*-----------------------------------------------------------*/