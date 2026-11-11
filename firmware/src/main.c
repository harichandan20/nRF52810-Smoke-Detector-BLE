
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

/* LED connected to P0.20 */
#define LED_PIN 20

void main(void)
{
    const struct device *gpio0;

    /* Get GPIO port 0 */
    gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

    /* Check GPIO device */
    if (!device_is_ready(gpio0))
    {
        return;
    }

    /* Configure P0.20 as output */
    gpio_pin_configure(gpio0, LED_PIN, GPIO_OUTPUT_INACTIVE);

    while (1)
    {
        /* Turn LED ON */
        gpio_pin_set(gpio0, LED_PIN, 1);

        k_msleep(500);

        /* Turn LED OFF */
        gpio_pin_set(gpio0, LED_PIN, 0);

        k_msleep(500);
    }
}
