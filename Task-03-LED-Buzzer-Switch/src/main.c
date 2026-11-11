
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#define LED_PIN       20
#define BUZZER_PIN     9
#define SWITCH_PIN    25

int main(void)
{
    const struct device *gpio0 =
        DEVICE_DT_GET(DT_NODELABEL(gpio0));

    int ret;
    int switch_state;

    if (!device_is_ready(gpio0)) {
        printk("GPIO device not ready\n");
        return 0;
    }

    ret = gpio_pin_configure(gpio0, LED_PIN,
                             GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("LED configuration error: %d\n", ret);
        return 0;
    }

    ret = gpio_pin_configure(gpio0, BUZZER_PIN,
                             GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("Buzzer configuration error: %d\n", ret);
        return 0;
    }

    ret = gpio_pin_configure(gpio0, SWITCH_PIN,
                             GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        printk("Switch configuration error: %d\n", ret);
        return 0;
    }

    printk("Task 03 Started\n");

    while (1) {

        switch_state = gpio_pin_get_raw(gpio0, SWITCH_PIN);

        if (switch_state < 0) {
            printk("Switch read error: %d\n", switch_state);
            k_msleep(10);
            continue;
        }

        if (switch_state == 0) {
            /* Switch pressed */
            gpio_pin_set(gpio0, LED_PIN, 1);
            gpio_pin_set(gpio0, BUZZER_PIN, 1);
        }
        else {
            /* Switch released */
            gpio_pin_set(gpio0, LED_PIN, 0);
            gpio_pin_set(gpio0, BUZZER_PIN, 0);
        }

        k_msleep(10);
    }

    return 0;
}
