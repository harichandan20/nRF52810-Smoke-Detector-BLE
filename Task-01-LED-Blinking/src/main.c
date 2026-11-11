#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#define LED_PIN 20

#define GPIO_PORT_NODE DT_NODELABEL(gpio0)

int main(void)
{
    const struct device *gpio_dev =
        DEVICE_DT_GET(GPIO_PORT_NODE);

    if (!device_is_ready(gpio_dev))
    {
        printk("GPIO device is not ready!\n");
        return 0;
    }

    int ret = gpio_pin_configure(
        gpio_dev,
        LED_PIN,
        GPIO_OUTPUT_INACTIVE
    );

    if (ret < 0)
    {
        printk("LED configuration failed!\n");
        return 0;
    }

    printk("Task 1: LED Blinking Started\n");

    while (1)
    {
        gpio_pin_set(gpio_dev, LED_PIN, 1);
        printk("LED ON\n");

        k_msleep(500);

        gpio_pin_set(gpio_dev, LED_PIN, 0);
        printk("LED OFF\n");

        k_msleep(500);
    }

    return 0;
}