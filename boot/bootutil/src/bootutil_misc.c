/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2017-2019 Linaro LTD
 * Copyright (c) 2016-2019 JUUL Labs
 * Copyright (c) 2019-2020 Arm Limited
 *
 * Original license:
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include "sysflash/sysflash.h"
#include "flash_map_backend/flash_map_backend.h"

#include "bootutil/image.h"
#include "bootutil/bootutil.h"
#include "bootutil_priv.h"
#include "bootutil_misc.h"
#include "bootutil/bootutil_log.h"
#include "bootutil/fault_injection_hardening.h"
#ifdef MCUBOOT_ENC_IMAGES
#include "bootutil/enc_key.h"
#endif

BOOT_LOG_MODULE_DECLARE(mcuboot);

/* Currently only used by imgmgr */
int boot_current_slot;

/**
 * @brief Determine if the data at two memory addresses is equal
 *
 * @param s1    The first  memory region to compare.
 * @param s2    The second memory region to compare.
 * @param n     The amount of bytes to compare.
 *
 * @note        This function does not comply with the specification of memcmp,
 *              so should not be considered a drop-in replacement. It has no
 *              constant time execution. The point is to make sure that all the
 *              bytes are compared and detect if loop was abused and some cycles
 *              was skipped due to fault injection.
 *
 * @return      FIH_SUCCESS if memory regions are equal, otherwise FIH_FAILURE
 */
#ifdef MCUBOOT_FIH_PROFILE_OFF
inline
fih_ret boot_fih_memequal(const void *s1, const void *s2, size_t n)
{
    return memcmp(s1, s2, n);
}
#else
fih_ret boot_fih_memequal(const void *s1, const void *s2, size_t n)
{
    size_t i;
    uint8_t *s1_p = (uint8_t*) s1;
    uint8_t *s2_p = (uint8_t*) s2;
    FIH_DECLARE(ret, FIH_FAILURE);

    for (i = 0; i < n; i++) {
        if (s1_p[i] != s2_p[i]) {
            goto out;
        }
    }
    if (i == n) {
        ret = FIH_SUCCESS;
    }

out:
    FIH_RET(ret);
}
#endif

/*
 * Amount of space used to save information required when doing a swap,
 * or while a swap is under progress, but not the status of sector swap
 * progress itself.
 */
static inline uint32_t
boot_trailer_info_sz(void)
{
    return (
#ifdef MCUBOOT_ENC_IMAGES
           /* encryption keys */
#  if MCUBOOT_SWAP_SAVE_ENCTLV
           BOOT_ENC_TLV_ALIGN_SIZE * 2            +
#  else
           BOOT_ENC_KEY_ALIGN_SIZE * 2            +
#  endif
#endif
           /* swap_type + copy_done + image_ok + swap_size */
           BOOT_MAX_ALIGN * 4                     +
           BOOT_MAGIC_ALIGN_SIZE
           );
}

/*
 * Amount of space used to maintain progress information for a single swap
 * operation.
 */
static inline uint32_t
boot_status_entry_sz(uint32_t min_write_sz)
{
    return BOOT_STATUS_STATE_COUNT * min_write_sz;
}

uint32_t
boot_status_sz(uint32_t min_write_sz)
{
    return BOOT_STATUS_MAX_ENTRIES * boot_status_entry_sz(min_write_sz);
}

uint32_t
boot_trailer_sz(uint32_t min_write_sz)
{
    return boot_status_sz(min_write_sz) + boot_trailer_info_sz();
}

#if MCUBOOT_SWAP_USING_SCRATCH
/*
 * Similar to `boot_trailer_sz` but this function returns the space used to
 * store status in the scratch partition. The scratch partition only stores
 * status during the swap of the last sector from primary/secondary (which
 * is the first swap operation) and thus only requires space for one swap.
 */
static uint32_t
boot_scratch_trailer_sz(uint32_t min_write_sz)
{
    return boot_status_entry_sz(min_write_sz) + boot_trailer_info_sz();
}
#endif

int
boot_status_entries(int image_index, const struct flash_area *fap)
{
#if MCUBOOT_SWAP_USING_SCRATCH
    if (flash_area_get_id(fap) == FLASH_AREA_IMAGE_SCRATCH) {
        return BOOT_STATUS_STATE_COUNT;
    } else
#endif
    if (flash_area_get_id(fap) == FLASH_AREA_IMAGE_PRIMARY(image_index) ||
        flash_area_get_id(fap) == FLASH_AREA_IMAGE_SECONDARY(image_index)) {
        return BOOT_STATUS_STATE_COUNT * BOOT_STATUS_MAX_ENTRIES;
    }
    return -1;
}

uint32_t
boot_status_off(const struct flash_area *fap)
{
    uint32_t off_from_end;
    uint32_t elem_sz;

    elem_sz = flash_area_align(fap);

#if MCUBOOT_SWAP_USING_SCRATCH
    if (flash_area_get_id(fap) == FLASH_AREA_IMAGE_SCRATCH) {
        off_from_end = boot_scratch_trailer_sz(elem_sz);
    } else {
#endif
        off_from_end = boot_trailer_sz(elem_sz);
#if MCUBOOT_SWAP_USING_SCRATCH
    }
#endif

    assert(off_from_end <= flash_area_get_size(fap));
    return flash_area_get_size(fap) - off_from_end;
}

#ifdef MCUBOOT_ENC_IMAGES
static inline uint32_t
boot_enc_key_off(const struct flash_area *fap, uint8_t slot)
{
#if MCUBOOT_SWAP_SAVE_ENCTLV
    return boot_swap_size_off(fap) - ((slot + 1) * BOOT_ENC_TLV_ALIGN_SIZE);
#else
    return boot_swap_size_off(fap) - ((slot + 1) * BOOT_ENC_KEY_ALIGN_SIZE);
#endif
}
#endif

/**
 * This functions tries to locate the status area after an aborted swap,
 * by looking for the magic in the possible locations.
 *
 * If the magic is successfully found, a flash_area * is returned and it
 * is the responsibility of the called to close it.
 *
 * @returns 0 on success, -1 on errors
 */
int
boot_find_status(int image_index, const struct flash_area **fap)
{
    uint8_t areas[] = {
#if MCUBOOT_SWAP_USING_SCRATCH
        FLASH_AREA_IMAGE_SCRATCH,
#endif
        FLASH_AREA_IMAGE_PRIMARY(image_index),
    };
    unsigned int i;

    /*
     * In the middle a swap, tries to locate the area that is currently
     * storing a valid magic, first on the primary slot, then on scratch.
     * Both "slots" can end up being temporary storage for a swap and it
     * is assumed that if magic is valid then other metadata is too,
     * because magic is always written in the last step.
     */

    for (i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
        uint8_t magic[BOOT_MAGIC_SZ];

        if (flash_area_open(areas[i], fap)) {
            break;
        }

        if (flash_area_read(*fap, boot_magic_off(*fap), magic, BOOT_MAGIC_SZ)) {
            flash_area_close(*fap);
            break;
        }

        if (BOOT_MAGIC_GOOD == boot_magic_decode(magic)) {
            return 0;
        }

        flash_area_close(*fap);
    }

    /* If we got here, no magic was found */
    fap = NULL;
    return -1;
}

int
boot_read_swap_size(const struct flash_area *fap, uint32_t *swap_size)
{
    uint32_t off;
    int rc;

    off = boot_swap_size_off(fap);
    rc = flash_area_read(fap, off, swap_size, sizeof *swap_size);

    return rc;
}

#ifdef MCUBOOT_ENC_IMAGES
int
boot_read_enc_key(const struct flash_area *fap, uint8_t slot, struct boot_status *bs)
{
    uint32_t off;
#if MCUBOOT_SWAP_SAVE_ENCTLV
    uint32_t i;
#endif
    int rc;

    off = boot_enc_key_off(fap, slot);
#if MCUBOOT_SWAP_SAVE_ENCTLV
    rc = flash_area_read(fap, off, bs->enctlv[slot], BOOT_ENC_TLV_ALIGN_SIZE);
    if (rc == 0) {
        for (i = 0; i < BOOT_ENC_TLV_ALIGN_SIZE; i++) {
            if (bs->enctlv[slot][i] != 0xff) {
                break;
            }
        }
        /* Only try to decrypt non-erased TLV metadata */
        if (i != BOOT_ENC_TLV_ALIGN_SIZE) {
            rc = boot_decrypt_key(bs->enctlv[slot], bs->enckey[slot]);
        }
    }
#else
    rc = flash_area_read(fap, off, bs->enckey[slot], BOOT_ENC_KEY_ALIGN_SIZE);
#endif

    return rc;
}
#endif

int
boot_write_swap_size(const struct flash_area *fap, uint32_t swap_size)
{
    uint32_t off;

    off = boot_swap_size_off(fap);
    BOOT_LOG_DBG("writing swap_size; fa_id=%d off=0x%lx (0x%lx)",
                 flash_area_get_id(fap), (unsigned long)off,
                 (unsigned long)flash_area_get_off(fap) + off);
    return boot_write_trailer(fap, off, (const uint8_t *) &swap_size, 4);
}

#ifdef MCUBOOT_ENC_IMAGES
int
boot_write_enc_key(const struct flash_area *fap, uint8_t slot,
        const struct boot_status *bs)
{
    uint32_t off;
    int rc;

    off = boot_enc_key_off(fap, slot);
    BOOT_LOG_DBG("writing enc_key; fa_id=%d off=0x%lx (0x%lx)",
                 flash_area_get_id(fap), (unsigned long)off,
                 (unsigned long)flash_area_get_off(fap) + off);
#if MCUBOOT_SWAP_SAVE_ENCTLV
    rc = flash_area_write(fap, off, bs->enctlv[slot], BOOT_ENC_TLV_ALIGN_SIZE);
#else
    rc = flash_area_write(fap, off, bs->enckey[slot], BOOT_ENC_KEY_ALIGN_SIZE);
#endif
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    return 0;
}
#endif

#ifdef MCUBOOT_SWAP_USING_SCRATCH
size_t
boot_get_first_trailer_sector(struct boot_loader_state *state, size_t slot, size_t trailer_sz)
{
    size_t first_trailer_sector = boot_img_num_sectors(state, slot) - 1;
    size_t sector_sz = boot_img_sector_size(state, slot, first_trailer_sector);
    size_t trailer_sector_sz = sector_sz;

    while (trailer_sector_sz < trailer_sz) {
        /* Consider that the image trailer may span across sectors of different sizes */
        --first_trailer_sector;
        sector_sz = boot_img_sector_size(state, slot, first_trailer_sector);

        trailer_sector_sz += sector_sz;
    }

    return first_trailer_sector;
}

/**
 * Returns the offset to the end of the first sector of a given slot that holds image trailer data.
 *
 * @param state      Current bootloader's state.
 * @param slot       The index of the slot to consider.
 * @param trailer_sz The size of the trailer, in bytes.
 *
 * @return The offset to the end of the first sector of the slot that holds image trailer data.
 */
static uint32_t
get_first_trailer_sector_end_off(struct boot_loader_state *state, size_t slot, size_t trailer_sz)
{
    size_t first_trailer_sector = boot_get_first_trailer_sector(state, slot, trailer_sz);

    return boot_img_sector_off(state, slot, first_trailer_sector) +
           boot_img_sector_size(state, slot, first_trailer_sector);
}
#endif /* MCUBOOT_SWAP_USING_SCRATCH */

uint32_t bootutil_max_image_size(struct boot_loader_state *state, const struct flash_area *fap)
{
#if defined(MCUBOOT_SINGLE_APPLICATION_SLOT) ||      \
    defined(MCUBOOT_FIRMWARE_LOADER)
    (void) state;
    return boot_status_off(fap);
#elif defined(MCUBOOT_SWAP_USING_SCRATCH)
    size_t slot_trailer_sz = boot_trailer_sz(BOOT_WRITE_SZ(state));
    size_t slot_trailer_off = flash_area_get_size(fap) - slot_trailer_sz;

    /* If the trailer doesn't fit in the last sector of the primary or secondary slot, some padding
     * might have to be inserted between the end of the firmware image and the beginning of the
     * trailer to ensure there is enough space for the trailer in the scratch area when the last
     * sector of the secondary will be copied to the scratch area.
     *
     * The value of the padding depends on the amount of trailer data that is contained in the first
     * trailer containing part of the trailer in the primary and secondary slot.
     */
    size_t trailer_sector_primary_end_off =
        get_first_trailer_sector_end_off(state, BOOT_PRIMARY_SLOT, slot_trailer_sz);
    size_t trailer_sector_secondary_end_off =
        get_first_trailer_sector_end_off(state, BOOT_SECONDARY_SLOT, slot_trailer_sz);

    size_t trailer_sz_in_first_sector;

    if (trailer_sector_primary_end_off > trailer_sector_secondary_end_off) {
        trailer_sz_in_first_sector = trailer_sector_primary_end_off - slot_trailer_off;
    } else {
        trailer_sz_in_first_sector = trailer_sector_secondary_end_off - slot_trailer_off;
    }

    size_t trailer_padding = 0;
    size_t scratch_trailer_sz = boot_scratch_trailer_sz(BOOT_WRITE_SZ(state));

    if (scratch_trailer_sz > trailer_sz_in_first_sector) {
        trailer_padding = scratch_trailer_sz - trailer_sz_in_first_sector;
    }

    return slot_trailer_off - trailer_padding;
#elif defined(MCUBOOT_SWAP_USING_MOVE)
    (void) state;

    struct flash_sector sector;
    /* get the last sector offset */
    int rc = flash_area_get_sector(fap, boot_status_off(fap), &sector);
    if (rc) {
        BOOT_LOG_ERR("Unable to determine flash sector of the image trailer");
        return 0; /* Returning of zero here should cause any check which uses
                   * this value to fail.
                   */
    }
    return flash_sector_get_off(&sector);
#elif defined(MCUBOOT_OVERWRITE_ONLY)
    (void) state;
    return boot_swap_info_off(fap);
#elif defined(MCUBOOT_DIRECT_XIP)
    (void) state;
    return boot_swap_info_off(fap);
#elif defined(MCUBOOT_RAM_LOAD)
    (void) state;
    return boot_swap_info_off(fap);
#endif
}
