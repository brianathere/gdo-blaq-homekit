#include <stdint.h>

#include "esp_private/panic_internal.h"
#include "esp_rom_sys.h"

extern "C" {
#include "hal/gpio_ll.h"
}

static constexpr uint32_t GDO_UART_TX_PIN = 1;

extern "C" void __real_esp_panic_handler(panic_info_t *info);

extern "C" void __wrap_esp_panic_handler(panic_info_t *info)
{
    esp_rom_printf("PANIC: DISABLING GDO UART TX PIN!\n");
    gpio_dev_t *gpio = &GPIO;
    gpio_ll_func_sel(gpio, GDO_UART_TX_PIN, PIN_FUNC_GPIO);
    gpio_ll_set_level(gpio, GDO_UART_TX_PIN, 0);
    gpio_ll_output_disable(gpio, GDO_UART_TX_PIN);
    gpio_ll_pullup_dis(gpio, GDO_UART_TX_PIN);
    gpio_ll_pulldown_en(gpio, GDO_UART_TX_PIN);
    gpio_ll_input_enable(gpio, GDO_UART_TX_PIN);
    __real_esp_panic_handler(info);
}
