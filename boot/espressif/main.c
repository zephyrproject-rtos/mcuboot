/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <bootutil/bootutil.h>
#include <bootutil/image.h>

#include <mcuboot_config/mcuboot_logging.h>

#include <bootloader_init.h>
#include <esp_loader.h>

void do_boot(struct boot_rsp *rsp)
{
    MCUBOOT_LOG_INF("br_image_off = 0x%x", rsp->br_image_off);
    MCUBOOT_LOG_INF("ih_hdr_size = 0x%x", rsp->br_hdr->ih_hdr_size);
    int slot = (rsp->br_image_off == CONFIG_ESP_APPLICATION_PRIMARY_START_ADDRESS) ? 0 : 1;
    esp_app_image_load(slot, rsp->br_hdr->ih_hdr_size);
}

int main()
{
    bootloader_init();
    struct boot_rsp rsp;
#ifdef MCUBOOT_VER
    MCUBOOT_LOG_INF("*** Booting MCUBoot build %s  ***", MCUBOOT_VER);
#endif

    fih_int fih_rc = FIH_FAILURE;
    FIH_CALL(boot_go, fih_rc, &rsp);
    if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
        MCUBOOT_LOG_ERR("Unable to find bootable image");
        FIH_PANIC;
    }
    do_boot(&rsp);
    while(1);
}
