#include <pmb887x.h>
#include <lcd/ILI9320.h>
#include <lcd/R61505U.h>

#include "lcd-controller.h"
#include "lcd-transport.h"

#define R61505U_WIDTH 240U
#define R61505U_HEIGHT 320U

static enum lcd_pixel_format current_format = LCD_PIXEL_FORMAT_RGB565;
static struct lcd_address_mode current_mode;

static const struct r61505_register {
	uint8_t index;
	uint16_t value;
} IL220_PANEL_REGISTERS[] = {
	{ R61505U_CALIBRATION_CONTROL, R61505U_CALIBRATION_CONTROL_CALB },
	{ ILI9320_GATE_SCAN, 0x2700 },
	{ ILI9320_DISPLAY_CONTROL_2, 0x020E },
	{ ILI9320_GAMMA_30, 0x0700 },
	{ ILI9320_GAMMA_31, 0x0200 },
	{ ILI9320_GAMMA_32, 0x0202 },
	{ R61505U_GAMMA_33, 0x0003 },
	{ R61505U_GAMMA_34, 0x0303 },
	{ ILI9320_GAMMA_35, 0x0707 },
	{ ILI9320_GAMMA_36, 0x1F1F },
	{ ILI9320_GAMMA_37, 0x0506 },
	{ ILI9320_GAMMA_38, 0x0202 },
	{ ILI9320_GAMMA_39, 0x0202 },
	{ R61505U_GAMMA_3A, 0x0103 },
	{ R61505U_GAMMA_3B, 0x0303 },
	{ ILI9320_GAMMA_3C, 0x0703 },
	{ ILI9320_GAMMA_3D, 0x1F1F },
};

static const struct r61505_register IL220_POWER_AND_INTERFACE_REGISTERS[] = {
	{ ILI9320_DISPLAY_CONTROL_1, 0x0001 },
	{ R61505U_POWER_CONTROL_5, R61505U_POWER_CONTROL_5_PSE },
	{ ILI9320_POWER_CONTROL_1, 0x10B0 },
	{ ILI9320_POWER_CONTROL_2, 0x0117 },
	{ ILI9320_POWER_CONTROL_3, 0x011A },
	{ ILI9320_POWER_CONTROL_4, 0x0A00 },
	{ ILI9320_POWER_CONTROL_7, 0x000B },
	{ R61505U_POWER_CONTROL_3, 0x011A | R61505U_POWER_CONTROL_3_PSON },
	{ ILI9320_DRIVER_OUTPUT_CONTROL, 0x0500 },
	{ ILI9320_LCD_DRIVING_CONTROL, 0x0700 },
	{ ILI9320_ENTRY_MODE, 0x5030 },
	{ ILI9320_RESIZE_CONTROL, 0x0000 },
	{ ILI9320_DISPLAY_CONTROL_3, 0x0001 },
	{ ILI9320_DISPLAY_CONTROL_4, 0x0008 },
	{ ILI9320_FRAME_MARKER, 0x000E },
	{ ILI9320_WINDOW_X_START, 0x0000 },
	{ ILI9320_WINDOW_X_END, R61505U_WIDTH - 1 },
	{ ILI9320_WINDOW_Y_START, 0x0000 },
	{ ILI9320_WINDOW_Y_END, R61505U_HEIGHT - 1 },
	{ ILI9320_BASE_IMAGE, 0x0000 },
	{ ILI9320_PANEL_INTERFACE_1, 0x001C },
	{ ILI9320_PANEL_INTERFACE_2, 0x0100 },
	{ ILI9320_PANEL_INTERFACE_3, 0x0002 },
	{ ILI9320_GRAM_X, 0x0000 },
	{ ILI9320_GRAM_Y, 0x0000 },
	{ ILI9320_DISPLAY_CONTROL_1, 0x0021 },
};

static bool r61505_write_command(uint8_t command) {
	return lcd_transport_write_command(0) && lcd_transport_write_command(command);
}

static bool r61505_write_register(uint8_t index, uint16_t value) {
	uint8_t data[] = { value >> 8, value };

	return r61505_write_command(index) && lcd_transport_write_data(data, sizeof(data));
}

static bool r61505_write_registers(const struct r61505_register *registers, uint32_t count) {
	for (uint32_t i = 0; i < count; i++) {
		if (!r61505_write_register(registers[i].index, registers[i].value))
			return false;
	}

	return true;
}

static uint16_t r61505_entry_mode(void) {
	uint16_t value = ILI9320_ENTRY_MODE_DFM |
		(current_mode.reverse_x ? 0 : 1U << ILI9320_ENTRY_MODE_ID_SHIFT) |
		(current_mode.reverse_y ? 0 : 2U << ILI9320_ENTRY_MODE_ID_SHIFT) |
		(current_mode.swap_axes ? ILI9320_ENTRY_MODE_AM : 0);

	/* The IL220 module uses BGR=1 as its normal RGB wiring. */
	if (!current_mode.bgr)
		value |= ILI9320_ENTRY_MODE_BGR;
	if (current_format != LCD_PIXEL_FORMAT_RGB565)
		value |= ILI9320_ENTRY_MODE_TRI;

	return value;
}

static bool r61505_apply_entry_mode(void) {
	return r61505_write_register(ILI9320_ENTRY_MODE, r61505_entry_mode());
}

static bool r61505_set_pixel_format(enum lcd_pixel_format format) {
	if (format != LCD_PIXEL_FORMAT_RGB565 && format != LCD_PIXEL_FORMAT_RGB666_8_8_2 &&
		format != LCD_PIXEL_FORMAT_RGB666_2_8_8 && format != LCD_PIXEL_FORMAT_RGB666)
		return false;
	current_format = format;
	return r61505_apply_entry_mode();
}

static bool r61505_set_address_mode(const struct lcd_address_mode *mode) {
	current_mode = *mode;
	return r61505_apply_entry_mode();
}

static bool r61505_set_cursor(uint16_t x, uint16_t y) {
	if (x >= R61505U_WIDTH || y >= R61505U_HEIGHT)
		return false;

	return r61505_write_register(ILI9320_GRAM_X, x) &&
		r61505_write_register(ILI9320_GRAM_Y, y);
}

static bool r61505_set_window(uint16_t x_start, uint16_t x_end, uint16_t y_start, uint16_t y_end) {
	if (x_start > x_end || y_start > y_end || x_end >= R61505U_WIDTH || y_end >= R61505U_HEIGHT)
		return false;

	uint16_t current_x = current_mode.reverse_x ? x_end : x_start;
	uint16_t current_y = current_mode.reverse_y ? y_end : y_start;

	return r61505_write_register(ILI9320_WINDOW_X_START, x_start) &&
		r61505_write_register(ILI9320_WINDOW_X_END, x_end) &&
		r61505_write_register(ILI9320_WINDOW_Y_START, y_start) &&
		r61505_write_register(ILI9320_WINDOW_Y_END, y_end) &&
		r61505_set_cursor(current_x, current_y);
}

static bool r61505_write_pixels(const struct lcd_color *colors, uint32_t count) {
	if (!r61505_write_command(ILI9320_GRAM_DATA))
		return false;

	for (uint32_t i = 0; i < count; i++) {
		if (current_format == LCD_PIXEL_FORMAT_RGB565) {
			uint16_t pixel = ((uint16_t)(colors[i].red >> 1) << 11) |
				((uint16_t)colors[i].green << 5) | (colors[i].blue >> 1);
			uint8_t data[] = { pixel >> 8, pixel };

			if (!lcd_transport_write_data(data, sizeof(data)))
				return false;
		} else if (current_format == LCD_PIXEL_FORMAT_RGB666_8_8_2) {
			uint8_t data[] = {
				colors[i].red << 2 | colors[i].green >> 4,
				colors[i].green << 4 | colors[i].blue >> 2,
				colors[i].blue << 6,
			};

			if (!lcd_transport_write_data(data, sizeof(data)))
				return false;
		} else if (current_format == LCD_PIXEL_FORMAT_RGB666_2_8_8) {
			uint8_t data[] = {
				colors[i].red >> 4,
				colors[i].red << 4 | colors[i].green >> 2,
				colors[i].green << 6 | colors[i].blue,
			};

			if (!lcd_transport_write_data(data, sizeof(data)))
				return false;
		} else {
			uint8_t data[] = { colors[i].red << 2, colors[i].green << 2, colors[i].blue << 2 };

			if (!lcd_transport_write_data(data, sizeof(data)))
				return false;
		}
	}
	return true;
}

static uint8_t r61505_expand_5bit(uint8_t value) {
	return (value << 1) | (value >> 4);
}

static void r61505_quantize_color(
	enum lcd_pixel_format format,
	const struct lcd_color *input,
	struct lcd_color *output
) {
	(void) format;
	*output = *input;
	output->red = r61505_expand_5bit(input->red >> 1);
	output->blue = r61505_expand_5bit(input->blue >> 1);
}

static bool r61505_read_pixels(struct lcd_color *colors, uint32_t count) {
	uint8_t data[2];

	if (!r61505_write_command(ILI9320_GRAM_DATA) ||
		!lcd_transport_read_data(data, sizeof(data)))
		return false;
	for (uint32_t i = 0; i < count; i++) {
		if (!lcd_transport_read_data(data, sizeof(data)))
			return false;

		uint16_t pixel = ((uint16_t)data[0] << 8) | data[1];
		colors[i] = (struct lcd_color) {
			.red = r61505_expand_5bit(pixel >> 11),
			.green = (pixel >> 5) & 0x3F,
			.blue = r61505_expand_5bit(pixel & 0x1F),
		};
	}

	return true;
}

static bool r61505_matches_id(uint32_t id) {
#if defined(BOARD_LG_KE970)
	if (id == LCD_CONTROLLER_ID_UNAVAILABLE)
		return true;
#endif

	return id == 0x9320 || ((id & 0xfff) == 0x505);
}

static bool r61505_probe(uint32_t *id) {
#if defined(BOARD_LG_KE970)
	printf("# IL220 device-code read unavailable: KE970 DIF_RD is used for HOOK_DETECT\n");
	*id = LCD_CONTROLLER_ID_UNAVAILABLE;
	return true;
#else
	uint8_t raw[8] = { 0 };

	for (uint32_t i = 0; i < 4; i++) {
		if (!r61505_write_command(ILI9320_OSCILLATION))
			return false;
	}
	if (!lcd_transport_read_data(raw, sizeof(raw)))
		return false;

	// TODO: untested on anything but KE970, see above why :)
	printf("# ILI9320/R61505U R00h device code raw:");
	for (uint32_t i = 0; i < ARRAY_SIZE(raw); i++)
		printf(" %02X", raw[i]);
	printf("\n");

	for (uint32_t i = 0; i + 1 < ARRAY_SIZE(raw); i++) {
		uint16_t candidate = ((uint16_t)raw[i] << 8) | raw[i + 1];
		if (r61505_matches_id(candidate)) {
			*id = candidate;
			return true;
		}
	}

	*id = ((uint32_t)raw[0] << 8) | raw[1];
	return false;
#endif
}

static bool r61505_initialize(void) {
	static const struct lcd_address_mode DEFAULT_MODE = { 0 };

	current_format = LCD_PIXEL_FORMAT_RGB565;
	current_mode = DEFAULT_MODE;
	if (!r61505_write_register(ILI9320_DISPLAY_CONTROL_1, 0x0000) ||
		!r61505_write_register(ILI9320_POWER_CONTROL_3, 0x0000))
		return false;
	stopwatch_usleep_wd(9000);

	for (uint32_t i = 0; i < 4; i++) {
		if (!r61505_write_command(ILI9320_OSCILLATION))
			return false;
	}
	if (!r61505_write_register(ILI9320_OSCILLATION, ILI9320_OSCILLATION_OSC))
		return false;
	stopwatch_usleep_wd(10000);

	if (!r61505_write_registers(IL220_PANEL_REGISTERS, ARRAY_SIZE(IL220_PANEL_REGISTERS)))
		return false;
	stopwatch_usleep_wd(9000);

	if (!r61505_write_registers(
			IL220_POWER_AND_INTERFACE_REGISTERS,
			ARRAY_SIZE(IL220_POWER_AND_INTERFACE_REGISTERS)
		))
		return false;
	stopwatch_usleep_wd(10000);

	if (!r61505_write_register(
			R61505U_DISPLAY_CONTROL_1,
			0x0021 | R61505U_DISPLAY_CONTROL_1_VON
		))
		return false;
	stopwatch_usleep_wd(6000);

	return r61505_write_register(
		R61505U_DISPLAY_CONTROL_1,
		0x0133 | R61505U_DISPLAY_CONTROL_1_VON
	);
}

const struct lcd_controller lcd_controller_r61505 = {
	.name = "R61505U/ILI9320",
	.type = LCD_CONTROLLER_R61505,
	.id = 0x1505,
	.width = R61505U_WIDTH,
	.height = R61505U_HEIGHT,
	.pixel_formats = BIT(LCD_PIXEL_FORMAT_RGB565) | BIT(LCD_PIXEL_FORMAT_RGB666),
	.reset_settle_ms = 0,
	.gram_write_command = { 0x00, ILI9320_GRAM_DATA },
	.gram_write_command_size = 2,
	.gram_read_command = { 0x00, ILI9320_GRAM_DATA },
	.gram_read_command_size = 2,
	.rgb565_read_dummy_bytes = 2,
	.swap_axes_changes_gram_order = false,
	.reverse_x_mirrors_coordinates = false,
	.reverse_y_mirrors_coordinates = false,
	.matches_id = r61505_matches_id,
	.probe = r61505_probe,
	.initialize = r61505_initialize,
	.quantize_color = r61505_quantize_color,
	.set_pixel_format = r61505_set_pixel_format,
	.set_address_mode = r61505_set_address_mode,
	.set_window = r61505_set_window,
	.set_cursor = r61505_set_cursor,
	.write_pixels = r61505_write_pixels,
	.read_pixels = r61505_read_pixels,
};
