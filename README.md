# Flushhiki Zigbee flush controller

ESP-IDF firmware for a Seeed Studio XIAO ESP32-C6. It joins Zigbee2MQTT as a standard Zigbee Home Automation on/off light. Sending `ON` to endpoint 10 produces a 500 ms active-high pulse on GPIO1, then returns the Zigbee state to `OFF`. GPIO15's active-low user LED follows the pulse.

## Build with Docker

Docker is the only build prerequisite. Build the firmware and export the flashable files into `dist/`:

```sh
docker build --output type=local,dest=dist .
```

The first build downloads the `espressif/idf:v5.4` image and Espressif's official Zigbee components. The output contains:

- `dist/bootloader/bootloader.bin`
- `dist/partition_table/partition-table.bin`
- `dist/flushhiki.bin`
- `dist/flash_args`

The directory layout matches `flash_args`. Flashing does not require ESP-IDF; install Espressif's `esptool` and run it from inside `dist/`:

```sh
python3 -m pip install esptool
ls /dev/cu.usb*                         # find the port on macOS
cd dist
python3 -m esptool --chip esp32c6 --port /dev/cu.usbmodemXXXX erase-flash
python3 -m esptool --chip esp32c6 --port /dev/cu.usbmodemXXXX write-flash @flash_args
```

On Linux the port is usually `/dev/ttyACM0`. The PyPI package named `idf` is unrelated to ESP-IDF and does not provide `idf.py`.

## Build and flash with a local ESP-IDF

Alternatively, install ESP-IDF 5.2 or newer and run:

```sh
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Open permit-join in Zigbee2MQTT before first boot. The firmware retries joining once per second while it is factory-new. Zigbee2MQTT may initially label this custom model unsupported; it should still appear in the device list, and its device page can generate an external definition from the discovered standard on/off cluster.

If it does not appear, erase the stored Zigbee network, open permit-join, and reset the board while permit-join is still open. A BOOT-button factory reset is tracked in [`TODO.md`](TODO.md).

To remove stored pairing data and join a different network:

```sh
idf.py -p PORT erase-flash
idf.py -p PORT flash monitor
```

## Output wiring

GPIO1 is a **3.3 V logic signal only**. Do not connect it directly to the actuator's motor or to a button/sensor line whose voltage has not been measured. Use a correctly sized MOSFET or optocoupler. GPIO1 is configured low before the Zigbee stack starts.

The firmware is an always-on Zigbee router for prompt command delivery. This is simple and responsive, but it consumes more power than a sleepy end device.

## Zigbee identity

- Manufacturer: `Flushhiki`
- Model: `CatToiletFlushV1`
- Endpoint: `10`
- Device type: HA on/off light
- Input command: On/Off cluster (`0x0006`) `ON`
