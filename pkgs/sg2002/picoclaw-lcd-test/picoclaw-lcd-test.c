#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum {
    LCD_WIDTH = 240,
    LCD_HEIGHT = 240,
    LCD_Y_OFFSET = 80,
    GPIO_DC_INDEX = 0,
    GPIO_RESET_INDEX = 1,
    GPIO_BACKLIGHT_INDEX = 2,
};

static volatile sig_atomic_t stopping;

static void on_signal(int signo)
{
    (void)signo;
    stopping = 1;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stopping)
        ;
}

static void die(const char *operation)
{
    fprintf(stderr, "picoclaw-lcd-test: %s: %s\n", operation, strerror(errno));
    exit(EXIT_FAILURE);
}

static void write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            die("SPI write");
        }
        if (written == 0) {
            errno = EIO;
            die("short SPI write");
        }
        cursor += written;
        length -= (size_t)written;
    }
}

static void gpio_set(int line_fd, unsigned int index, int high)
{
    struct gpio_v2_line_values values = {
        .bits = high ? (UINT64_C(1) << index) : 0,
        .mask = UINT64_C(1) << index,
    };

    if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0)
        die("set GPIO line");
}

static int request_control_lines(const char *gpiochip)
{
    int chip_fd = open(gpiochip, O_RDONLY | O_CLOEXEC);
    struct gpio_v2_line_request request = {0};

    if (chip_fd < 0)
        die("open GPIO chip");

    request.offsets[GPIO_DC_INDEX] = 28;
    request.offsets[GPIO_RESET_INDEX] = 27;
    request.offsets[GPIO_BACKLIGHT_INDEX] = 19;
    request.num_lines = 3;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    /* D/C low, reset high, active-low backlight high (off). */
    request.config.attrs[0].attr.values =
        (UINT64_C(1) << GPIO_RESET_INDEX) |
        (UINT64_C(1) << GPIO_BACKLIGHT_INDEX);
    request.config.attrs[0].mask = UINT64_C(0x7);
    snprintf(request.consumer, sizeof(request.consumer), "picoclaw-lcd");

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
        die("request GPIOA19/A27/A28");
    close(chip_fd);
    return request.fd;
}

static void lcd_command(int spi_fd, int gpio_fd, uint8_t command,
                        const uint8_t *data, size_t data_length)
{
    gpio_set(gpio_fd, GPIO_DC_INDEX, 0);
    write_all(spi_fd, &command, 1);
    if (data_length > 0) {
        gpio_set(gpio_fd, GPIO_DC_INDEX, 1);
        write_all(spi_fd, data, data_length);
    }
}

static void lcd_reset(int gpio_fd)
{
    gpio_set(gpio_fd, GPIO_BACKLIGHT_INDEX, 1);
    gpio_set(gpio_fd, GPIO_RESET_INDEX, 0);
    sleep_ms(20);
    gpio_set(gpio_fd, GPIO_RESET_INDEX, 1);
    sleep_ms(120);
}

static void lcd_init(int spi_fd, int gpio_fd)
{
    const uint8_t pixel_format = 0x55; /* RGB565 */
    const uint8_t madctl = 0xc0;       /* Sipeed rvclaw rotation=180 */

    lcd_reset(gpio_fd);
    lcd_command(spi_fd, gpio_fd, 0x01, NULL, 0); /* SWRESET */
    sleep_ms(150);
    lcd_command(spi_fd, gpio_fd, 0x11, NULL, 0); /* SLPOUT */
    sleep_ms(120);
    lcd_command(spi_fd, gpio_fd, 0x3a, &pixel_format, 1); /* COLMOD */
    lcd_command(spi_fd, gpio_fd, 0x36, &madctl, 1);       /* MADCTL */
    lcd_command(spi_fd, gpio_fd, 0x21, NULL, 0);         /* INVON */
    lcd_command(spi_fd, gpio_fd, 0x29, NULL, 0);         /* DISPON */
    sleep_ms(100);
}

struct glyph {
    char character;
    uint8_t rows[7];
};

static const struct glyph glyphs[] = {
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'C', { 0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f } },
    { 'I', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'N', { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
};

static const uint8_t *find_glyph(char character)
{
    size_t index;

    for (index = 0; index < sizeof(glyphs) / sizeof(glyphs[0]); ++index)
        if (glyphs[index].character == character)
            return glyphs[index].rows;
    return NULL;
}

static int text_pixel(const char *text, int origin_x, int origin_y, int scale,
                      int x, int y)
{
    int relative_x = x - origin_x;
    int relative_y = y - origin_y;
    int cell_width = 6 * scale;
    int character_index;
    int glyph_x;
    int glyph_y;
    const uint8_t *rows;

    if (relative_x < 0 || relative_y < 0 || relative_y >= 7 * scale)
        return 0;
    character_index = relative_x / cell_width;
    if (character_index < 0 || (size_t)character_index >= strlen(text))
        return 0;
    glyph_x = (relative_x % cell_width) / scale;
    glyph_y = relative_y / scale;
    if (glyph_x >= 5)
        return 0;
    rows = find_glyph(text[character_index]);
    return rows != NULL && (rows[glyph_y] & (UINT8_C(1) << (4 - glyph_x)));
}

static uint16_t test_pixel(int x, int y)
{
    uint16_t background;

    if (x < 80)
        background = 0xf800; /* red */
    else if (x < 160)
        background = 0x07e0; /* green */
    else
        background = 0x001f; /* blue */

    if (y >= 44 && y < 190 && x >= 16 && x < 224) {
        background = 0x0000;
        if (x == 16 || x == 223 || y == 44 || y == 189)
            return 0xffff;
    }
    if (text_pixel("NIXOS", 62, 69, 4, x, y))
        return 0x07ff; /* cyan */
    if (text_pixel("PICOCLAW", 49, 126, 3, x, y))
        return 0xffff;
    if (y >= 204)
        return (((x / 12) + (y / 12)) & 1) ? 0xffff : 0x0000;
    return background;
}

static void lcd_draw_test(int spi_fd, int gpio_fd)
{
    uint8_t column[] = { 0x00, 0x00, 0x00, LCD_WIDTH - 1 };
    uint16_t y_end = LCD_Y_OFFSET + LCD_HEIGHT - 1;
    uint8_t row_address[] = {
        0x00, LCD_Y_OFFSET, (uint8_t)(y_end >> 8), (uint8_t)y_end,
    };
    uint8_t row[LCD_WIDTH * 2];
    int x;
    int y;

    lcd_command(spi_fd, gpio_fd, 0x2a, column, sizeof(column)); /* CASET */
    lcd_command(spi_fd, gpio_fd, 0x2b, row_address,
                sizeof(row_address)); /* RASET */
    lcd_command(spi_fd, gpio_fd, 0x2c, NULL, 0); /* RAMWR */
    gpio_set(gpio_fd, GPIO_DC_INDEX, 1);

    for (y = 0; y < LCD_HEIGHT; ++y) {
        for (x = 0; x < LCD_WIDTH; ++x) {
            uint16_t pixel = test_pixel(x, y);

            row[x * 2] = (uint8_t)(pixel >> 8);
            row[x * 2 + 1] = (uint8_t)pixel;
        }
        write_all(spi_fd, row, sizeof(row));
    }
}

static int open_spi(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 10000000;

    if (fd < 0)
        die("open SPI device");
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
        die("configure SPI device");
    return fd;
}

int main(int argc, char **argv)
{
    const char *spi_path = argc > 1 ? argv[1] : "/dev/spidev1.0";
    const char *gpiochip = argc > 2 ? argv[2] : "/dev/gpiochip0";
    int gpio_fd;
    int spi_fd;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    gpio_fd = request_control_lines(gpiochip);
    spi_fd = open_spi(spi_path);
    lcd_init(spi_fd, gpio_fd);
    lcd_draw_test(spi_fd, gpio_fd);
    gpio_set(gpio_fd, GPIO_BACKLIGHT_INDEX, 0);

    printf("PicoClaw ST7789 test pattern is on %s\n", spi_path);
    fflush(stdout);
    while (!stopping)
        pause();

    gpio_set(gpio_fd, GPIO_BACKLIGHT_INDEX, 1);
    close(spi_fd);
    close(gpio_fd);
    return EXIT_SUCCESS;
}
