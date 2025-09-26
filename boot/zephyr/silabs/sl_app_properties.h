/*
 * Silicon Labs Application Properties for MCUboot
 * File: bootloader/mcuboot/boot/zephyr/silabs/sl_app_properties.h
 */

#ifndef SL_APP_PROPERTIES_H
#define SL_APP_PROPERTIES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


// Silicon Labs Application Properties Magic Numbers and Types
// Exact definitions from application_properties.h
#define APPLICATION_PROPERTIES_MAGIC     { \
    0x13, 0xb7, 0x79, 0xfa,                \
    0xc9, 0x25, 0xdd, 0xb7,                \
    0xad, 0xf3, 0xcf, 0xe0,                \
    0xf1, 0xb6, 0x14, 0xb8                 \
}

#define APPLICATION_PROPERTIES_VERSION_MAJOR (1UL)
#define APPLICATION_PROPERTIES_VERSION_MINOR (2UL)
#define APPLICATION_CERTIFICATE_VERSION      (1UL)
#define APPLICATION_SIGNATURE_NONE           (0UL)
#define APPLICATION_SIGNATURE_ECDSA_P256     (1UL << 0UL)
#define APPLICATION_SIGNATURE_CRC32          (1UL << 1UL)

// Application Types
#define APPLICATION_TYPE_ZIGBEE              (1UL << 0UL)
#define APPLICATION_TYPE_THREAD              (1UL << 1UL)
#define APPLICATION_TYPE_FLEX                (1UL << 2UL)
#define APPLICATION_TYPE_BLUETOOTH           (1UL << 3UL)
#define APPLICATION_TYPE_MCU                 (1UL << 4UL)
#define APPLICATION_TYPE_BLUETOOTH_APP       (1UL << 5UL)
#define APPLICATION_TYPE_BOOTLOADER          (1UL << 6UL)
#define APPLICATION_TYPE_ZWAVE               (1UL << 7UL)

// Version definitions
#define BOOTLOADER_VERSION_MAIN              0x00010000
#define APPLICATION_VERSION_MAIN             0x00010000

// Calculate structure version
#define APPLICATION_PROPERTIES_VERSION ((APPLICATION_PROPERTIES_VERSION_MAJOR << 0U) | \
                                        (APPLICATION_PROPERTIES_VERSION_MINOR << 8U))

/// Application Data
typedef struct ApplicationData {
  uint32_t type;
  uint32_t version;
  uint32_t capabilities;
  uint8_t productId[16];
} ApplicationData_t;

/// Application Certificate
typedef struct ApplicationCertificate {
  uint8_t structVersion;
  uint8_t flags[3];
  uint8_t key[64];
  uint32_t version;
  uint8_t signature[64];
} ApplicationCertificate_t;

/**
 * @brief Silicon Labs Application Properties Structure - Exact format from application_properties.h
 * 
 * This structure is used by Silicon Labs tools (Simplicity Commander, 
 * Simplicity Studio) to identify and validate firmware images.
 */
typedef struct {
  uint8_t magic[16];
  uint32_t structVersion;
  uint32_t signatureType;
  uint32_t signatureLocation;
  ApplicationData_t app;
  ApplicationCertificate_t *cert;
  uint8_t *longTokenSectionAddress;
  const uint8_t decryptKey[16];
} ApplicationProperties_t;

/**
 * @brief Get Application Properties
 * 
 * @return Pointer to the application properties structure
 */
const ApplicationProperties_t* sl_get_app_properties(void);

#ifdef __cplusplus
}
#endif

#endif /* SL_APP_PROPERTIES_H */
