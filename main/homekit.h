// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License

#pragma once

#include <gdo.h>

void homekit_task_entry(void* ctx);

void notify_homekit_current_door_state_change(gdo_door_state_t door);
void notify_homekit_target_door_state_change(gdo_door_state_t door);
void notify_homekit_obstruction(gdo_obstruction_state_t obstructed);
void notify_homekit_current_lock(gdo_lock_state_t lock);
void notify_homekit_target_lock(gdo_lock_state_t lock);
void notify_homekit_light(gdo_light_state_t light);
void notify_homekit_motion(gdo_motion_state_t motion);
void notify_homekit_battery(gdo_battery_state_t battery);

int homekit_paired_controller_count(void);
char *homekit_setup_payload(void);
