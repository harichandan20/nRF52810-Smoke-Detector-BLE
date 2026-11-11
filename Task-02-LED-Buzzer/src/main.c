
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

/* GPIO pin configuration */
#define LED_PIN       20
#define BUZZER_PIN     9

/* Timing configuration */
#define ON_TIME_MS   500
#define OFF_TIME_MS  500

int main(void)
{
    const struct device *gpio0 =
        DEVICE_DT_GET(DT_NODELABEL(gpio0));

    /* Check GPIO device */
    if (!device_is_ready(gpio0)) {
        printk("ERROR: GPIO device is not ready!\n");
        return 0;
    }

    /* Configure LED and buzzer as outputs */
    int ret;

    ret = gpio_pin_configure(gpio0, LED_PIN,
                             GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("ERROR: LED configuration failed!\n");
        return 0;
    }

    ret = gpio_pin_configure(gpio0, BUZZER_PIN,
                             GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("ERROR: Buzzer configuration failed!\n");
        return 0;
    }

    printk("Task 02: LED and Buzzer Started\n");

    while (1) {

        /* LED ON and Buzzer ON */
        gpio_pin_set(gpio0, LED_PIN, 1);
        gpio_pin_set(gpio0, BUZZER_PIN, 1);

        printk("LED ON | BUZZER ON\n");

        k_msleep(ON_TIME_MS);

        /* LED OFF and Buzzer OFF */
        gpio_pin_set(gpio0, LED_PIN, 0);
        gpio_pin_set(gpio0, BUZZER_PIN, 0);

        printk("LED OFF | BUZZER OFF\n");

        k_msleep(OFF_TIME_MS);
    }

    return 0;
}
