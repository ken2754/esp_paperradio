#ifndef PMIC_AXP2101_H
#define PMIC_AXP2101_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PMIC_PEK_NONE = 0,
    PMIC_PEK_SHORT,     // short press
    PMIC_PEK_LONG,      // long press (>= ~4s, used as power-off request)
} pmic_pek_event_t;

esp_err_t pmic_init(void);

/** Battery voltage in mV, or -1 on error. */
int pmic_get_battery_mv(void);

/** Battery level 0..100 %, or -1 if unavailable. */
int pmic_get_battery_percent(void);

/** True while external power (VBUS) is present. */
bool pmic_is_vbus_present(void);

/** True while the battery is charging. */
bool pmic_is_charging(void);

/**
 * Polls AXP2101 IRQ status for power-key events and clears all IRQ flags.
 * Call periodically (e.g. every 20-50 ms) from the input task.
 */
pmic_pek_event_t pmic_poll_pek(void);

/** Hardware power-off via AXP2101 reg 0x10 = 0x01. */
esp_err_t pmic_power_off(void);

#endif // PMIC_AXP2101_H
