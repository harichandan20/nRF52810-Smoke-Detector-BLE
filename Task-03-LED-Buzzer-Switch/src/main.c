#include <zephyr/kernel.h>

int main(void)
{
    /* TODO: Implement LED, buzzer and switch control using GPIO */

    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
