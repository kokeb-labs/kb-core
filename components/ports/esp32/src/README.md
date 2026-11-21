# components/ports/esp32/src/

The boundary `.cpp` files of the ESP32 port, listed in `../deviations/boundary-files.yml`: thin typed wrappers
that convert kb types, call ESP-IDF and map the result to `kb::Result` with the `esp_err_t` in `detail`.
