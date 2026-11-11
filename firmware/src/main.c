
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

// LED connected to P0.20
#define LED_PIN 20

// Buzzer connected to P0.09
#define BUZZER_PIN 9

// GPIO port 0
#define GPIO_PORT_NODE DT_NODELABEL(gpio0)

int main(void)
{
    const struct device *gpio_dev =
        DEVICE_DT_GET(GPIO_PORT_NODE);

    // Check GPIO device
    if (!device_is_ready(gpio_dev))
    {
        printk("GPIO device is not ready!\n");
        return 0;
    }

    // Configure LED as output
    gpio_pin_configure(gpio_dev,
                       LED_PIN,
                       GPIO_OUTPUT_INACTIVE);

    // Configure buzzer as output
    gpio_pin_configure(gpio_dev,
                       BUZZER_PIN,
                       GPIO_OUTPUT_INACTIVE);

    printk("Task 2: LED and Buzzer Test Started\n");

    while (1)
    {
        // Turn ON LED
        gpio_pin_set(gpio_dev, LED_PIN, 1);

        // Turn ON buzzer
        gpio_pin_set(gpio_dev, BUZZER_PIN, 1);

        printk("LED ON - BUZZER ON\n");

        k_msleep(500);

        // Turn OFF LED
        gpio_pin_set(gpio_dev, LED_PIN, 0);

        // Turn OFF buzzer
        gpio_pin_set(gpio_dev, BUZZER_PIN, 0);

        printk("LED OFF - BUZZER OFF\n");

        k_msleep(500);
    }

    return 0;
}
