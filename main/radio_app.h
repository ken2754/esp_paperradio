#ifndef RADIO_APP_H
#define RADIO_APP_H

#include <stdbool.h>
#include "esp_err.h"
#include "ui_display.h"

esp_err_t radio_app_init(void);

void radio_app_play(void);
void radio_app_stop(void);
void radio_app_select_station(int index, bool start_playing);
void radio_app_set_volume(int volume);
void radio_app_adjust_volume(int delta);
void radio_app_request_ui_refresh(bool full);

/** Fills `out` with a snapshot of the current radio & system status. */
void radio_app_get_status(ui_state_t *out);

bool radio_app_is_playing_intent(void);

#endif // RADIO_APP_H
