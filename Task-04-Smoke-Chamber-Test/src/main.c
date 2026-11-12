
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>

/* GPIO pin definitions */
#define IR_LED_PIN          12
#define SENSOR_PIN          10
#define STATUS_LED_PIN      20
#define BUZZER_PIN           9

/* Output active levels */
#define LED_ACTIVE_LEVEL     1
#define BUZZER_ACTIVE_LEVEL  1

/* Smoke detection settings */
#define SMOKE_BLINK_MS      300

#define HIT_MAX              20
#define HIT_ON               15
#define HIT_OFF               5

/* GPIO port */
static const struct device *gpio0 =
    DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Smoke detection variables */
static int idle_level = 1;
static int hits = 0;
static bool smoke_detected = false;

/* Read sensor while IR LED is ON */
static int read_sensor_once(void)
{
    int sensor_value;

    /* Turn ON IR LED */
    gpio_pin_set_raw(gpio0, IR_LED_PIN, 1);

    /* Wait for sensor response */
    k_busy_wait(1000);

    /* Read digital sensor output */
    sensor_value = gpio_pin_get_raw(gpio0, SENSOR_PIN);

    /* Turn OFF IR LED */
    gpio_pin_set_raw(gpio0, IR_LED_PIN, 0);

    return sensor_value;
}

/* Calibrate sensor in clean air */
static int learn_idle_level(void)
{
    int ones = 0;
    int total = 0;

    printk("Calibrating smoke chamber...\n");
    printk("Keep the chamber in clean air.\n");

    for (int i = 0; i < 100; i++)
    {
        int value = read_sensor_once();

        if (value >= 0)
        {
            total++;

            if (value == 1)
            {
                ones++;
            }
        }

        k_msleep(10);
    }

    if (total == 0)
    {
        printk("Sensor read error!\n");
        return -1;
    }

    /* Determine normal sensor level */
    return (ones * 2 > total) ? 1 : 0;
}

/* Configure GPIO pins */
static int configure_gpio(void)
{
    if (!device_is_ready(gpio0))
    {
        printk("GPIO device not ready!\n");
        return -1;
    }

    /* IR LED output */
    if (gpio_pin_configure(gpio0, IR_LED_PIN,
                           GPIO_OUTPUT_INACTIVE) < 0)
    {
        printk("IR LED configuration failed!\n");
        return -1;
    }

    /* Photodiode digital sensor input */
    if (gpio_pin_configure(gpio0, SENSOR_PIN,
                           GPIO_INPUT | GPIO_PULL_UP) < 0)
    {
        printk("Sensor configuration failed!\n");
        return -1;
    }

    /* Status LED output */
    if (gpio_pin_configure(gpio0, STATUS_LED_PIN,
                           GPIO_OUTPUT_INACTIVE) < 0)
    {
        printk("Status LED configuration failed!\n");
        return -1;
    }

    /* Buzzer output */
    if (gpio_pin_configure(gpio0, BUZZER_PIN,
                           GPIO_OUTPUT_INACTIVE) < 0)
    {
        printk("Buzzer configuration failed!\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    int sensor_value;
    int led_state = 0;

    uint32_t last_blink_time = 0;

    printk("\n");
    printk("================================\n");
    printk("      SMOKE CHAMBER TEST\n");
    printk("================================\n");

    /* Initialize GPIO */
    if (configure_gpio() != 0)
    {
        printk("GPIO initialization failed!\n");
        return 0;
    }

    printk("GPIO initialized successfully.\n");

    /* Calibrate sensor in clean air */
    idle_level = learn_idle_level();

    if (idle_level < 0)
    {
        printk("Calibration failed. Check sensor wiring!\n");
        return 0;
    }

    printk("Normal sensor level: %d\n", idle_level);
    printk("Smoke monitoring started.\n");

    while (1)
    {
        /* Read smoke chamber sensor */
        sensor_value = read_sensor_once();

        if (sensor_value >= 0)
        {
            /* Sensor differs from clean-air level */
            if (sensor_value != idle_level)
            {
                if (hits < HIT_MAX)
                {
                    hits++;
                }
            }
            else
            {
                if (hits > 0)
                {
                    hits--;
                }
            }

            /* Smoke detection ON */
            if (hits >= HIT_ON)
            {
                smoke_detected = true;
            }
            /* Smoke detection OFF */
            else if (hits <= HIT_OFF)
            {
                smoke_detected = false;
            }
        }
        else
        {
            printk("Sensor read error!\n");
        }

        if (smoke_detected)
        {
            uint32_t now = k_uptime_get_32();

            /* Blink LED every 300 ms */
            if ((now - last_blink_time) >= SMOKE_BLINK_MS)
            {
                led_state = !led_state;
                last_blink_time = now;
            }

            /* Buzzer continuously ON */
            gpio_pin_set_raw(
                gpio0,
                BUZZER_PIN,
                BUZZER_ACTIVE_LEVEL
            );
        }
        else
        {
            /* No smoke: LED and buzzer OFF */
            led_state = 0;

            gpio_pin_set_raw(
                gpio0,
                BUZZER_PIN,
                !BUZZER_ACTIVE_LEVEL
            );
        }

        /* Update status LED */
        gpio_pin_set_raw(
            gpio0,
            STATUS_LED_PIN,
            led_state ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL
        );

        /* Sensor sampling interval */
        k_msleep(10);
    }

    return 0;
}
