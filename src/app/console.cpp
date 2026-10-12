#include "app/console.h"
// Before the VFS headers: their <sys/termios.h> defines ECHO, a Valence frame type.
#include "link/link.h"

#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace garnet::console {
namespace {

struct Console {
    TetherLink* tether = nullptr;
    QueueHandle_t commands = nullptr;
    char line[kLineMax]{};
};
Console c;

void dispatch(size_t n) {
    while (n > 0 && c.line[n - 1] == '\r') c.line[--n] = '\0';
    if (n == 0) return;
    if (n > 3 && c.line[0] == '@' && c.line[1] == 'V' && c.line[2] == ' ') {
        c.tether->pushLine(c.line + 3);
    } else {
        xQueueSend(c.commands, c.line, 0);  // full: the command is dropped
    }
}

// Bulk reads from the driver: stdio's byte-at-a-time fgets cannot keep up
// with a catalog burst on the tether, and the driver drops what overflows.
void reader(void*) {
    uint8_t buf[256];
    size_t len = 0;
    bool overlong = false;
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; ++i) {
            if (buf[i] != '\n') {
                if (len < kLineMax - 1) {
                    c.line[len++] = char(buf[i]);
                } else {
                    overlong = true;
                }
                continue;
            }
            c.line[len] = '\0';
            if (!overlong) dispatch(len);
            len = 0;
            overlong = false;
        }
    }
}

}  // namespace

void start(TetherLink& tether) {
    c.tether = &tether;
    c.commands = xQueueCreateWithCaps(4, kLineMax, MALLOC_CAP_SPIRAM);
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 16384;
    cfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCoreWithCaps(reader, "console", 4096, nullptr, 4, nullptr, 0, MALLOC_CAP_INTERNAL);
}

bool next(char* out) { return c.commands && xQueueReceive(c.commands, out, 0) == pdPASS; }

}  // namespace garnet::console
