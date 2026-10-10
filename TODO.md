# TODO

- [ ] Add Zigbee factory reset: when the BOOT button (GPIO9, active-low) is held for at least 5 seconds, indicate confirmation with the user LED, call `esp_zb_bdb_reset_via_local_action()`, and restart network steering so the device can join a different coordinator. Short presses must do nothing, and GPIO1 must remain inactive during reset.
