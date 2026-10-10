# syntax=docker/dockerfile:1
FROM espressif/idf:v5.4 AS build

WORKDIR /project
COPY . .
RUN . "$IDF_PATH/export.sh" && idf.py build

FROM scratch AS firmware
COPY --from=build /project/build/bootloader/bootloader.bin /bootloader/bootloader.bin
COPY --from=build /project/build/partition_table/partition-table.bin /partition_table/partition-table.bin
COPY --from=build /project/build/flushhiki.bin /
COPY --from=build /project/build/flash_args /
