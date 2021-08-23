/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 * Copyright (c) 2020 Arm Limited
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <zephyr.h>
#include <kernel.h>
#include <version.h>

#include <drivers/gpio.h>
#include <sys/__assert.h>
#include <drivers/flash.h>
#include <drivers/timer/system_timer.h>
#include <linker/linker-defs.h>
#include <drivers/uart.h>

#include <soc.h>

#include "target.h"

#include "bootutil/bootutil_log.h"
#include "bootutil/image.h"
#include "bootutil/bootutil.h"
#include "bootutil/fault_injection_hardening.h"
#include "flash_map_backend/flash_map_backend.h"

/* CONFIG_LOG_MINIMAL is the legacy Kconfig property,
 * replaced by CONFIG_LOG_MODE_MINIMAL.
 */
#if (defined(CONFIG_LOG_MODE_MINIMAL) || defined(CONFIG_LOG_MINIMAL))
#define ZEPHYR_LOG_MODE_MINIMAL 1
#endif

#if defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) && \
    !defined(ZEPHYR_LOG_MODE_MINIMAL)
#ifdef CONFIG_LOG_PROCESS_THREAD
#warning "The log internal thread for log processing can't transfer the log"\
         "well for MCUBoot."
#else
#include <logging/log_ctrl.h>

#define BOOT_LOG_PROCESSING_INTERVAL K_MSEC(30) /* [ms] */

/* log are processing in custom routine */
K_THREAD_STACK_DEFINE(boot_log_stack, CONFIG_MCUBOOT_LOG_THREAD_STACK_SIZE);
struct k_thread boot_log_thread;
volatile bool boot_log_stop = false;
K_SEM_DEFINE(boot_log_sem, 1, 1);

/* log processing need to be initalized by the application */
#define ZEPHYR_BOOT_LOG_START() zephyr_boot_log_start()
#define ZEPHYR_BOOT_LOG_STOP() zephyr_boot_log_stop()
#endif /* CONFIG_LOG_PROCESS_THREAD */
#else
/* synchronous log mode doesn't need to be initalized by the application */
#define ZEPHYR_BOOT_LOG_START() do { } while (false)
#define ZEPHYR_BOOT_LOG_STOP() do { } while (false)
#endif /* defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) */

#ifdef CONFIG_SOC_FAMILY_NRF
#include <hal/nrf_power.h>

static inline bool boot_skip_serial_recovery()
{
#if NRF_POWER_HAS_RESETREAS
    uint32_t rr = nrf_power_resetreas_get(NRF_POWER);

    return !(rr == 0 || (rr & NRF_POWER_RESETREAS_RESETPIN_MASK));
#else
    return false;
#endif
}
#else
static inline bool boot_skip_serial_recovery()
{
    return false;
}
#endif

MCUBOOT_LOG_MODULE_REGISTER(mcuboot);

void os_heap_init(void);

/* Default: Assume entry point is at the very beginning of the image. Simply
 * lock interrupts and jump there. This is the right thing to do for X86 and
 * possibly other platforms.
 */
/* Generic and RISCV */
static void do_boot(struct boot_rsp *rsp)
{
    void *start = (void *)(uint64_t)(rsp->br_hdr->ih_load_addr + rsp->br_hdr->ih_hdr_size);
    BOOT_LOG_INF("Jumping to boot address: %p", start);

    /* Lock interrupts and dive into the entry point */
    irq_lock();
    ((void (*)(void))start)();
}

#if defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) &&\
    !defined(CONFIG_LOG_PROCESS_THREAD) && !defined(ZEPHYR_LOG_MODE_MINIMAL)
/* The log internal thread for log processing can't transfer log well as has too
 * low priority.
 * Dedicated thread for log processing below uses highest application
 * priority. This allows to transmit all logs without adding k_sleep/k_yield
 * anywhere else int the code.
 */

/* most simple log processing theread */
void boot_log_thread_func(void *dummy1, void *dummy2, void *dummy3)
{
    (void)dummy1;
    (void)dummy2;
    (void)dummy3;

     log_init();

     while (1) {
             if (log_process(false) == false) {
                    if (boot_log_stop) {
                        break;
                    }
                    k_sleep(BOOT_LOG_PROCESSING_INTERVAL);
             }
     }

     k_sem_give(&boot_log_sem);
}

void zephyr_boot_log_start(void)
{
        /* start logging thread */
        k_thread_create(&boot_log_thread, boot_log_stack,
                K_THREAD_STACK_SIZEOF(boot_log_stack),
                boot_log_thread_func, NULL, NULL, NULL,
                K_HIGHEST_APPLICATION_THREAD_PRIO, 0,
                BOOT_LOG_PROCESSING_INTERVAL);

        k_thread_name_set(&boot_log_thread, "logging");
}

void zephyr_boot_log_stop(void)
{
    boot_log_stop = true;

    /* wait until log procesing thread expired
     * This can be reworked using a thread_join() API once a such will be
     * available in zephyr.
     * see https://github.com/zephyrproject-rtos/zephyr/issues/21500
     */
    (void)k_sem_take(&boot_log_sem, K_FOREVER);
}
#endif/* defined(CONFIG_LOG) && !defined(CONFIG_LOG_IMMEDIATE) &&\
        !defined(CONFIG_LOG_PROCESS_THREAD) */

////////////////////////////////////////////////////////////////////////////////

#ifndef GIT_BRANCH
#define GIT_BRANCH "?"
#endif

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "?"
#endif

void poll_for_custom_firmware_load(int timeout_seconds)
{
    const struct device *uart_dev = device_get_binding("UART_0");
    
    char user_input = 0;
    while (timeout_seconds > 0)
    {
        BOOT_LOG_INF(" Hit the `G` key in %d seconds to prevent firmware load ...", timeout_seconds--);
        
        if (uart_poll_in(uart_dev, &user_input) != -1)
        {
            if (user_input == 'g' || user_input == 'G')
            {
                BOOT_LOG_INF(" Firmware load stopped ...");
                ZEPHYR_BOOT_LOG_STOP();

                __asm__ volatile ("ebreak");
                while (1)
                    ;
            }
        }
        k_msleep(1000);
    }
}

void main(void)
{
    struct boot_rsp rsp;
    int rc;
    fih_int fih_rc = FIH_FAILURE;

    MCUBOOT_WATCHDOG_FEED();
    BOOT_LOG_INF("     _____                       _          __                    __          ");
    BOOT_LOG_INF("    / ___/_________  _________  (_)___     / /   ____  ____ _____/ /__  _____ ");
    BOOT_LOG_INF("    \\__ \\/ ___/ __ \\/ ___/ __ \\/ / __ \\   / /   / __ \\/ __ `/ __  / _ \\/ ___/ ");
    BOOT_LOG_INF("   ___/ / /__/ /_/ / /  / /_/ / / /_/ /  / /___/ /_/ / /_/ / /_/ /  __/ /     ");
    BOOT_LOG_INF("  /____/\\___/\\____/_/  / .___/_/\\____/  /_____/\\____/\\__,_/\\__,_/\\___/_/      ");
    BOOT_LOG_INF("                      /_/                                                     ");
    BOOT_LOG_INF("");
    BOOT_LOG_INF("      Zephyr kernel   : %s", KERNEL_VERSION_STRING);
    BOOT_LOG_INF("      Git branch      : %s", GIT_BRANCH);
    BOOT_LOG_INF("      Git commit hash : %s", GIT_COMMIT_HASH);
    BOOT_LOG_INF("");

    os_heap_init();

    ZEPHYR_BOOT_LOG_START();

    (void)rc;

    FIH_CALL(boot_go, fih_rc, &rsp);
    if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
        BOOT_LOG_ERR("Unable to find bootable image, issue halt for user debug ...");
        
        // Issue a debug break to allow loading f/w
        __asm__ volatile ("ebreak");
        
        while (1)
            ;
    }

    poll_for_custom_firmware_load(5); // 5 seconds

    ZEPHYR_BOOT_LOG_STOP();

    do_boot(&rsp);

    while (1)
        ;
}
