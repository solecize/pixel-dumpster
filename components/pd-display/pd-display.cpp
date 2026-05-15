#include "pd-display.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub75.h"
#include "hub75_types.h"

static const char *TAG = "pd-display";
static Hub75Driver *pd_display_driver = nullptr;
static Hub75Config pd_display_hub75_config = {};
/* Currently active layout (mirrors what was used to instantiate the driver). */
static int pd_display_current_panel_width = 0;
static int pd_display_current_panel_height = 0;
static int pd_display_current_panel_rows = 1;
static int pd_display_current_panel_cols = 1;
static int pd_display_current_chain_pattern = 0;
static int pd_display_current_rotation = 0;
static int pd_display_current_scan = 0;
static int pd_display_current_color_order = 0;
/* Virtual canvas dimensions after rotation/tile (what the framebuffer logically is). */
static int pd_display_current_width = 0;
static int pd_display_current_height = 0;
static uint8_t pd_display_current_brightness = 96;

/* Map logical RGB to wire-order RGB based on color_order:
 * 0=RGB -> (r,g,b)
 * 1=BGR -> (b,g,r)
 * 2=GRB -> (g,r,b)
 * 3=BRG -> (b,r,g)  (common on some panels) */
static void pd_display_map_color(uint8_t lr, uint8_t lg, uint8_t lb,
                                  uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    switch (pd_display_current_color_order) {
        case 1:  /* BGR */
            *out_r = lb; *out_g = lg; *out_b = lr;
            break;
        case 2:  /* GRB */
            *out_r = lg; *out_g = lr; *out_b = lb;
            break;
        case 3:  /* BRG */
            *out_r = lb; *out_g = lr; *out_b = lg;
            break;
        default: /* RGB */
            *out_r = lr; *out_g = lg; *out_b = lb;
            break;
    }
}

#define PD_FONT_W 5
#define PD_FONT_H 7
#define PD_CELL_W 6
#define PD_CELL_H 9

static const uint8_t pd_font_5x7[96][PD_FONT_H] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // 33 '!'
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, // 35 '#'
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // 36 '$'
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // 37 '%'
    {0x08,0x14,0x14,0x08,0x15,0x12,0x0D}, // 38 '&'
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, // 39 '''
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // 40 '('
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // 41 ')'
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // 42 '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, // 44 ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, // 46 '.'
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, // 47 '/'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 48 '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 49 '1'
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 50 '2'
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 51 '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 52 '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 53 '5'
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 54 '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 55 '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 56 '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 57 '9'
    {0x00,0x00,0x04,0x00,0x00,0x04,0x00}, // 58 ':'
    {0x00,0x00,0x04,0x00,0x00,0x04,0x08}, // 59 ';'
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // 60 '<'
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // 61 '='
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // 62 '>'
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // 63 '?'
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, // 64 '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 65 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 66 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 67 'C'
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // 68 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 69 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 70 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // 71 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 72 'H'
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // 73 'I'
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // 74 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 75 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 76 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 77 'M'
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // 78 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 79 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 80 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 81 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 82 'R'
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // 83 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 84 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 85 'U'
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}, // 86 'V'
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // 87 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 88 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 89 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 90 'Z'
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // 91 '['
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, // 92 '\'
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // 93 ']'
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // 94 '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // 95 '_'
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // 97 'a'
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, // 98 'b'
    {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E}, // 99 'c'
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, // 100 'd'
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // 101 'e'
    {0x06,0x08,0x1C,0x08,0x08,0x08,0x08}, // 102 'f'
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, // 103 'g'
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, // 104 'h'
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // 105 'i'
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, // 106 'j'
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // 107 'k'
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, // 108 'l'
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, // 109 'm'
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, // 110 'n'
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // 111 'o'
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, // 112 'p'
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}, // 113 'q'
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, // 114 'r'
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, // 115 's'
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, // 116 't'
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // 117 'u'
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, // 118 'v'
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // 119 'w'
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, // 120 'x'
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, // 121 'y'
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, // 122 'z'
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, // 123 '{'
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, // 124 '|'
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, // 125 '}'
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, // 126 '~'
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, // 127 DEL (solid block)
};

#define PD_TINY_W 3
#define PD_TINY_H 5
#define PD_TINY_CELL_W 4
#define PD_TINY_CELL_H 6

static const uint8_t pd_font_tiny5[96][PD_TINY_H] = {
    {0x0,0x0,0x0,0x0,0x0}, // 32 ' '
    {0x2,0x2,0x2,0x0,0x2}, // 33 '!'
    {0x5,0x5,0x0,0x0,0x0}, // 34 '"'
    {0x5,0x7,0x5,0x7,0x5}, // 35 '#'
    {0x3,0x6,0x3,0x6,0x3}, // 36 '$'
    {0x5,0x1,0x2,0x4,0x5}, // 37 '%'
    {0x2,0x5,0x2,0x5,0x3}, // 38 '&'
    {0x2,0x2,0x0,0x0,0x0}, // 39 '\''
    {0x1,0x2,0x2,0x2,0x1}, // 40 '('
    {0x4,0x2,0x2,0x2,0x4}, // 41 ')'
    {0x5,0x2,0x5,0x0,0x0}, // 42 '*'
    {0x0,0x2,0x7,0x2,0x0}, // 43 '+'
    {0x0,0x0,0x0,0x2,0x4}, // 44 ','
    {0x0,0x0,0x7,0x0,0x0}, // 45 '-'
    {0x0,0x0,0x0,0x0,0x2}, // 46 '.'
    {0x1,0x1,0x2,0x4,0x4}, // 47 '/'
    {0x7,0x5,0x5,0x5,0x7}, // 48 '0'
    {0x2,0x6,0x2,0x2,0x7}, // 49 '1'
    {0x7,0x1,0x7,0x4,0x7}, // 50 '2'
    {0x7,0x1,0x3,0x1,0x7}, // 51 '3'
    {0x5,0x5,0x7,0x1,0x1}, // 52 '4'
    {0x7,0x4,0x7,0x1,0x7}, // 53 '5'
    {0x7,0x4,0x7,0x5,0x7}, // 54 '6'
    {0x7,0x1,0x1,0x2,0x2}, // 55 '7'
    {0x7,0x5,0x7,0x5,0x7}, // 56 '8'
    {0x7,0x5,0x7,0x1,0x7}, // 57 '9'
    {0x0,0x2,0x0,0x2,0x0}, // 58 ':'
    {0x0,0x2,0x0,0x2,0x4}, // 59 ';'
    {0x1,0x2,0x4,0x2,0x1}, // 60 '<'
    {0x0,0x7,0x0,0x7,0x0}, // 61 '='
    {0x4,0x2,0x1,0x2,0x4}, // 62 '>'
    {0x7,0x1,0x3,0x0,0x2}, // 63 '?'
    {0x7,0x5,0x5,0x4,0x7}, // 64 '@'
    {0x2,0x5,0x7,0x5,0x5}, // 65 'A'
    {0x6,0x5,0x6,0x5,0x6}, // 66 'B'
    {0x3,0x4,0x4,0x4,0x3}, // 67 'C'
    {0x6,0x5,0x5,0x5,0x6}, // 68 'D'
    {0x7,0x4,0x6,0x4,0x7}, // 69 'E'
    {0x7,0x4,0x6,0x4,0x4}, // 70 'F'
    {0x3,0x4,0x5,0x5,0x3}, // 71 'G'
    {0x5,0x5,0x7,0x5,0x5}, // 72 'H'
    {0x7,0x2,0x2,0x2,0x7}, // 73 'I'
    {0x1,0x1,0x1,0x5,0x2}, // 74 'J'
    {0x5,0x5,0x6,0x5,0x5}, // 75 'K'
    {0x4,0x4,0x4,0x4,0x7}, // 76 'L'
    {0x5,0x7,0x5,0x5,0x5}, // 77 'M'
    {0x5,0x7,0x7,0x5,0x5}, // 78 'N'
    {0x2,0x5,0x5,0x5,0x2}, // 79 'O'
    {0x6,0x5,0x6,0x4,0x4}, // 80 'P'
    {0x2,0x5,0x5,0x7,0x3}, // 81 'Q'
    {0x6,0x5,0x6,0x5,0x5}, // 82 'R'
    {0x3,0x4,0x2,0x1,0x6}, // 83 'S'
    {0x7,0x2,0x2,0x2,0x2}, // 84 'T'
    {0x5,0x5,0x5,0x5,0x7}, // 85 'U'
    {0x5,0x5,0x5,0x5,0x2}, // 86 'V'
    {0x5,0x5,0x5,0x7,0x5}, // 87 'W'
    {0x5,0x5,0x2,0x5,0x5}, // 88 'X'
    {0x5,0x5,0x2,0x2,0x2}, // 89 'Y'
    {0x7,0x1,0x2,0x4,0x7}, // 90 'Z'
    {0x3,0x2,0x2,0x2,0x3}, // 91 '['
    {0x4,0x4,0x2,0x1,0x1}, // 92 '\\'
    {0x6,0x2,0x2,0x2,0x6}, // 93 ']'
    {0x2,0x5,0x0,0x0,0x0}, // 94 '^'
    {0x0,0x0,0x0,0x0,0x7}, // 95 '_'
    {0x4,0x2,0x0,0x0,0x0}, // 96 '`'
    {0x0,0x3,0x5,0x5,0x3}, // 97 'a'
    {0x4,0x6,0x5,0x5,0x6}, // 98 'b'
    {0x0,0x3,0x4,0x4,0x3}, // 99 'c'
    {0x1,0x3,0x5,0x5,0x3}, // 100 'd'
    {0x0,0x7,0x5,0x6,0x3}, // 101 'e'
    {0x1,0x2,0x7,0x2,0x2}, // 102 'f'
    {0x0,0x3,0x5,0x3,0x6}, // 103 'g'
    {0x4,0x6,0x5,0x5,0x5}, // 104 'h'
    {0x2,0x0,0x2,0x2,0x2}, // 105 'i'
    {0x1,0x0,0x1,0x5,0x2}, // 106 'j'
    {0x4,0x5,0x6,0x5,0x5}, // 107 'k'
    {0x2,0x2,0x2,0x2,0x3}, // 108 'l'
    {0x0,0x5,0x7,0x5,0x5}, // 109 'm'
    {0x0,0x6,0x5,0x5,0x5}, // 110 'n'
    {0x0,0x2,0x5,0x5,0x2}, // 111 'o'
    {0x0,0x6,0x5,0x6,0x4}, // 112 'p'
    {0x0,0x3,0x5,0x3,0x1}, // 113 'q'
    {0x0,0x3,0x4,0x4,0x4}, // 114 'r'
    {0x0,0x3,0x2,0x1,0x6}, // 115 's'
    {0x2,0x7,0x2,0x2,0x1}, // 116 't'
    {0x0,0x5,0x5,0x5,0x3}, // 117 'u'
    {0x0,0x5,0x5,0x5,0x2}, // 118 'v'
    {0x0,0x5,0x5,0x7,0x5}, // 119 'w'
    {0x0,0x5,0x2,0x5,0x5}, // 120 'x' 
    {0x0,0x5,0x3,0x1,0x6}, // 121 'y'
    {0x0,0x7,0x1,0x4,0x7}, // 122 'z'
    {0x1,0x2,0x6,0x2,0x1}, // 123 '{'
    {0x2,0x2,0x2,0x2,0x2}, // 124 '|'
    {0x4,0x2,0x3,0x2,0x4}, // 125 '}'
    {0x0,0x5,0x2,0x0,0x0}, // 126 '~'
    {0x7,0x7,0x7,0x7,0x7}, // 127 DEL (solid block)
};

static Hub75Rotation pd_display_rotation_from_degrees(int degrees)
{
    switch (degrees) {
        case 90:
            return Hub75Rotation::ROTATE_90;
        case 180:
            return Hub75Rotation::ROTATE_180;
        case 270:
            return Hub75Rotation::ROTATE_270;
        case 0:
        default:
            return Hub75Rotation::ROTATE_0;
    }
}

static void pd_display_apply_matrixportal_s3_pins(Hub75Pins *pins)
{
    if (!pins) {
        return;
    }

    pins->r1 = 42;
    pins->g1 = 41;
    pins->b1 = 40;
    pins->r2 = 38;
    pins->g2 = 39;
    pins->b2 = 37;
    pins->a = 45;
    pins->b = 36;
    pins->c = 48;
    pins->d = 35;
    pins->e = 21;
    pins->lat = 47;
    pins->oe = 14;
    pins->clk = 2;
}

static Hub75ScanWiring pd_display_scan_wiring_from_int(int val)
{
    switch (val) {
        case 1: return Hub75ScanWiring::SCAN_1_4_16PX_HIGH;
        case 2: return Hub75ScanWiring::SCAN_1_8_32PX_HIGH;
        case 3: return Hub75ScanWiring::SCAN_1_8_40PX_HIGH;
        case 4: return Hub75ScanWiring::SCAN_1_8_64PX_HIGH;
        default: return Hub75ScanWiring::STANDARD_TWO_SCAN;
    }
}

static Hub75PanelLayout pd_display_layout_from_int(int val)
{
    switch (val) {
        case 1: return Hub75PanelLayout::TOP_LEFT_DOWN;
        case 2: return Hub75PanelLayout::TOP_RIGHT_DOWN;
        case 3: return Hub75PanelLayout::BOTTOM_LEFT_UP;
        case 4: return Hub75PanelLayout::BOTTOM_RIGHT_UP;
        case 5: return Hub75PanelLayout::TOP_LEFT_DOWN_ZIGZAG;
        case 6: return Hub75PanelLayout::TOP_RIGHT_DOWN_ZIGZAG;
        case 7: return Hub75PanelLayout::BOTTOM_LEFT_UP_ZIGZAG;
        case 8: return Hub75PanelLayout::BOTTOM_RIGHT_UP_ZIGZAG;
        case 0:
        default: return Hub75PanelLayout::HORIZONTAL;
    }
}

static esp_err_t pd_display_start_driver(int panel_w, int panel_h,
                                         int panel_rows, int panel_cols,
                                         int chain_pattern,
                                         int rotation_deg, int scan_wiring,
                                         int color_order)
{
    if (pd_display_driver) {
        pd_display_driver->end();
        delete pd_display_driver;
        pd_display_driver = nullptr;
    }

    if (panel_w <= 0 || panel_h <= 0 || panel_rows <= 0 || panel_cols <= 0) {
        ESP_LOGE(TAG, "invalid layout: panel=%dx%d rows=%d cols=%d",
                 panel_w, panel_h, panel_rows, panel_cols);
        return ESP_ERR_INVALID_ARG;
    }

    pd_display_hub75_config = {};
    pd_display_hub75_config.panel_width = (uint16_t)panel_w;
    pd_display_hub75_config.panel_height = (uint16_t)panel_h;
    pd_display_hub75_config.layout_rows = (uint16_t)panel_rows;
    pd_display_hub75_config.layout_cols = (uint16_t)panel_cols;
    pd_display_hub75_config.layout = pd_display_layout_from_int(chain_pattern);
    pd_display_hub75_config.shift_driver = Hub75ShiftDriver::FM6126A;
    pd_display_hub75_config.scan_wiring = pd_display_scan_wiring_from_int(scan_wiring);
    pd_display_hub75_config.rotation = pd_display_rotation_from_degrees(rotation_deg);
    pd_display_apply_matrixportal_s3_pins(&pd_display_hub75_config.pins);

    pd_display_driver = new Hub75Driver(pd_display_hub75_config);
    if (!pd_display_driver->begin()) {
        ESP_LOGE(TAG, "display driver begin() failed for panel %dx%d %dx%d chain pat=%d",
                 panel_w, panel_h, panel_rows, panel_cols, chain_pattern);
        delete pd_display_driver;
        pd_display_driver = nullptr;
        return ESP_FAIL;
    }

    pd_display_driver->set_brightness(pd_display_current_brightness);
    pd_display_driver->clear();
    pd_display_driver->flip_buffer();
    pd_display_driver->clear();
    vTaskDelay(pdMS_TO_TICKS(100));

    pd_display_current_panel_width = panel_w;
    pd_display_current_panel_height = panel_h;
    pd_display_current_panel_rows = panel_rows;
    pd_display_current_panel_cols = panel_cols;
    pd_display_current_chain_pattern = chain_pattern;
    pd_display_current_rotation = rotation_deg;
    pd_display_current_scan = scan_wiring;
    pd_display_current_color_order = color_order;
    pd_display_current_width = pd_display_driver->get_width();
    pd_display_current_height = pd_display_driver->get_height();

    ESP_LOGI(TAG,
             "display started: panel=%dx%d chain=%dx%d pat=%d rot=%d scan=%d color=%d virtual=%dx%d",
             panel_w, panel_h, panel_rows, panel_cols, chain_pattern,
             rotation_deg, scan_wiring, color_order,
             pd_display_current_width, pd_display_current_height);
    return ESP_OK;
}

extern "C" esp_err_t pd_display_init(const pd_display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    int rows = config->panel_rows > 0 ? config->panel_rows : 1;
    int cols = config->panel_cols > 0 ? config->panel_cols : 1;
    return pd_display_start_driver(config->panel_width, config->panel_height,
                                   rows, cols, config->chain_pattern,
                                   config->rotation_deg, config->scan_wiring,
                                   config->color_order);
}

extern "C" esp_err_t pd_display_reinit(const pd_display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    int rows = config->panel_rows > 0 ? config->panel_rows : 1;
    int cols = config->panel_cols > 0 ? config->panel_cols : 1;
    bool layout_same = (pd_display_driver &&
        config->panel_width == pd_display_current_panel_width &&
        config->panel_height == pd_display_current_panel_height &&
        rows == pd_display_current_panel_rows &&
        cols == pd_display_current_panel_cols &&
        config->chain_pattern == pd_display_current_chain_pattern &&
        config->rotation_deg == pd_display_current_rotation &&
        config->scan_wiring == pd_display_current_scan);
    if (layout_same && config->color_order == pd_display_current_color_order) {
        return ESP_OK;
    }
    if (layout_same) {
        /* only color_order changed — no driver restart needed */
        pd_display_current_color_order = config->color_order;
        ESP_LOGI(TAG, "color_order updated to %d (no driver restart)", config->color_order);
        return ESP_OK;
    }
    /* layout changed — driver restart required; not safe on this hardware at runtime */
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" void pd_display_set_brightness(uint8_t brightness)
{
    pd_display_current_brightness = brightness;
    if (pd_display_driver) {
        pd_display_driver->set_brightness(brightness);
    }
}

extern "C" uint8_t pd_display_get_brightness(void)
{
    return pd_display_current_brightness;
}

extern "C" void pd_display_clear(void)
{
    if (pd_display_driver) {
        pd_display_driver->clear();
    }
}

extern "C" void pd_display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, pd_display_color_t color)
{
    if (pd_display_driver) {
        uint8_t mr, mg, mb;
        pd_display_map_color(color.r, color.g, color.b, &mr, &mg, &mb);
        pd_display_driver->fill(x, y, w, h, mr, mg, mb);
    }
}

extern "C" void pd_display_set_pixel(uint16_t x, uint16_t y, pd_display_color_t color)
{
    if (pd_display_driver) {
        uint8_t mr, mg, mb;
        pd_display_map_color(color.r, color.g, color.b, &mr, &mg, &mb);
        pd_display_driver->set_pixel(x, y, mr, mg, mb);
    }
}

extern "C" void pd_display_render_rgb(const uint8_t *rgb, int img_w, int img_h)
{
    if (!pd_display_driver || !rgb) return;
    int dw = pd_display_current_width;
    int dh = pd_display_current_height;
    /* center the image if smaller than display */
    int ox = (img_w < dw) ? (dw - img_w) / 2 : 0;
    int oy = (img_h < dh) ? (dh - img_h) / 2 : 0;
    /* blit image pixels (no clear — overwrite in place to avoid flicker) */
    int blit_w = (img_w < dw) ? img_w : dw;
    int blit_h = (img_h < dh) ? img_h : dh;
    for (int y = 0; y < blit_h; y++) {
        for (int x = 0; x < blit_w; x++) {
            int src_idx = (y * img_w + x) * 3;
            uint8_t lr = rgb[src_idx];
            uint8_t lg = rgb[src_idx + 1];
            uint8_t lb = rgb[src_idx + 2];
            uint8_t mr, mg, mb;
            pd_display_map_color(lr, lg, lb, &mr, &mg, &mb);
            pd_display_driver->set_pixel(ox + x, oy + y, mr, mg, mb);
        }
    }
}

extern "C" void pd_display_render_framebuf(const uint8_t *rgb)
{
    if (!pd_display_driver || !rgb) return;
    int dw = pd_display_current_width;
    int dh = pd_display_current_height;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int idx = (y * dw + x) * 3;
            uint8_t lr = rgb[idx];
            uint8_t lg = rgb[idx + 1];
            uint8_t lb = rgb[idx + 2];
            uint8_t mr, mg, mb;
            pd_display_map_color(lr, lg, lb, &mr, &mg, &mb);
            pd_display_driver->set_pixel(x, y, mr, mg, mb);
        }
    }
}

extern "C" void pd_display_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, pd_display_color_t color)
{
    if (!pd_display_driver || w == 0 || h == 0) return;
    uint8_t mr, mg, mb;
    pd_display_map_color(color.r, color.g, color.b, &mr, &mg, &mb);
    for (uint16_t i = 0; i < w; i++) {
        pd_display_driver->set_pixel(x + i, y, mr, mg, mb);
        pd_display_driver->set_pixel(x + i, y + h - 1, mr, mg, mb);
    }
    for (uint16_t i = 0; i < h; i++) {
        pd_display_driver->set_pixel(x, y + i, mr, mg, mb);
        pd_display_driver->set_pixel(x + w - 1, y + i, mr, mg, mb);
    }
}

extern "C" void pd_display_draw_char(uint16_t x, uint16_t y, char ch, pd_display_color_t color)
{
    if (!pd_display_driver) {
        return;
    }

    int idx = (int)ch - 32;
    if (idx < 0 || idx >= 96) {
        idx = 0;
    }

    const uint8_t *glyph = pd_font_5x7[idx];
    uint16_t dw = pd_display_driver->get_width();
    uint16_t dh = pd_display_driver->get_height();

    for (int row = 0; row < PD_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < PD_FONT_W; col++) {
            if (bits & (1 << (PD_FONT_W - 1 - col))) {
                uint16_t px = x + col;
                uint16_t py = y + row;
                if (px < dw && py < dh) {
                    pd_display_set_pixel(px, py, color);
                }
            }
        }
    }
}

extern "C" void pd_display_draw_text(uint16_t x, uint16_t y, const char *text, pd_display_color_t color)
{
    if (!text || !pd_display_driver) {
        return;
    }

    uint16_t dw = pd_display_driver->get_width();
    uint16_t cx = x;
    while (*text) {
        if (cx + PD_FONT_W > dw) {
            break;
        }
        pd_display_draw_char(cx, y, *text, color);
        cx += PD_CELL_W;
        text++;
    }
}

extern "C" void pd_display_draw_char_tiny(uint16_t x, uint16_t y, char ch, pd_display_color_t color)
{
    if (!pd_display_driver) {
        return;
    }

    int idx = (int)ch - 32;
    if (idx < 0 || idx >= 96) {
        idx = 0;
    }

    const uint8_t *glyph = pd_font_tiny5[idx];
    uint16_t dw = pd_display_driver->get_width();
    uint16_t dh = pd_display_driver->get_height();

    for (int row = 0; row < PD_TINY_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < PD_TINY_W; col++) {
            if (bits & (1 << (PD_TINY_W - 1 - col))) {
                uint16_t px = x + col;
                uint16_t py = y + row;
                if (px < dw && py < dh) {
                    pd_display_set_pixel(px, py, color);
                }
            }
        }
    }
}

extern "C" void pd_display_draw_text_tiny(uint16_t x, uint16_t y, const char *text, pd_display_color_t color)
{
    if (!text || !pd_display_driver) {
        return;
    }

    uint16_t dw = pd_display_driver->get_width();
    uint16_t cx = x;
    while (*text) {
        if (cx + PD_TINY_W > dw) {
            break;
        }
        pd_display_draw_char_tiny(cx, y, *text, color);
        cx += PD_TINY_CELL_W;
        text++;
    }
}

extern "C" int pd_display_get_width(void)
{
    return pd_display_driver ? pd_display_driver->get_width() : 0;
}

extern "C" int pd_display_get_height(void)
{
    return pd_display_driver ? pd_display_driver->get_height() : 0;
}

extern "C" int pd_display_text_cols(void)
{
    return pd_display_driver ? pd_display_driver->get_width() / PD_CELL_W : 0;
}

extern "C" int pd_display_text_rows(void)
{
    return pd_display_driver ? pd_display_driver->get_height() / PD_CELL_H : 0;
}

extern "C" int pd_display_get_panel_width(void)  { return pd_display_current_panel_width; }
extern "C" int pd_display_get_panel_height(void) { return pd_display_current_panel_height; }
extern "C" int pd_display_get_panel_rows(void)   { return pd_display_current_panel_rows; }
extern "C" int pd_display_get_panel_cols(void)   { return pd_display_current_panel_cols; }
extern "C" int pd_display_get_chain_pattern(void) { return pd_display_current_chain_pattern; }
extern "C" int pd_display_get_rotation_deg(void) { return pd_display_current_rotation; }
extern "C" int pd_display_get_color_order(void) { return pd_display_current_color_order; }

extern "C" void pd_display_flip_buffer(void)
{
    if (pd_display_driver) {
        pd_display_driver->flip_buffer();
    }
}

/* Map (row, col) of the chain to the rectangle on the *virtual canvas*.
 *
 * Implementation detail: rotation is applied first by the driver, then panel
 * layout. From the framebuffer's perspective, the virtual canvas is always
 * exactly (panel_cols × panel_w_eff) × (panel_rows × panel_h_eff) where
 * (w_eff, h_eff) are the panel's apparent size after rotation. The scan_wiring
 * and chain_pattern only affect the *physical* mapping inside the driver, not
 * the framebuffer coordinates we draw to. So a simple grid carve is correct
 * for the purposes of overlaying per-panel content. */
extern "C" bool pd_display_get_panel_rect(int row, int col,
                                          int *out_x, int *out_y,
                                          int *out_w, int *out_h)
{
    if (!pd_display_driver) return false;
    int rows = pd_display_current_panel_rows;
    int cols = pd_display_current_panel_cols;
    if (row < 0 || row >= rows || col < 0 || col >= cols) return false;

    int pw = pd_display_current_panel_width;
    int ph = pd_display_current_panel_height;
    int rot = pd_display_current_rotation;

    /* With 90/270° rotation the driver transposes the virtual canvas:
     *   virtual_w = ph * rows,  virtual_h = pw * cols
     * so the physical panel grid is laid out with swapped dimensions.
     */
    if (rot == 90 || rot == 270) {
        if (out_x) *out_x = row * ph;
        if (out_y) *out_y = col * pw;
        if (out_w) *out_w = ph;
        if (out_h) *out_h = pw;
    } else {
        if (out_x) *out_x = col * pw;
        if (out_y) *out_y = row * ph;
        if (out_w) *out_w = pw;
        if (out_h) *out_h = ph;
    }
    return true;
}

extern "C" void pd_display_render_boot_message(void)
{
    if (!pd_display_driver) {
        return;
    }

    pd_display_driver->clear();

    /* Center "SETUP" on the full virtual canvas so it reads correctly
     * regardless of panel chain length / orientation. */
    int dw = pd_display_driver->get_width();
    int dh = pd_display_driver->get_height();
    const char *msg = "SETUP";
    int text_w = (int)strlen(msg) * PD_CELL_W - (PD_CELL_W - PD_FONT_W);
    int x = (dw - text_w) / 2;
    int y = (dh - PD_FONT_H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    pd_display_draw_text(x, y, msg, PD_COLOR_YELLOW);
    pd_display_driver->flip_buffer();
    ESP_LOGI(TAG, "render boot message: SETUP (centered on %dx%d)", dw, dh);
}

extern "C" void pd_display_render_idle(const char *device_name, int res_w, int res_h,
                                       int orient, int scan, const char *ip)
{
    if (!pd_display_driver) {
        return;
    }

    uint16_t dw = pd_display_driver->get_width();
    uint16_t dh = pd_display_driver->get_height();

    pd_display_driver->clear();

    /* 1px border at the display edges */
    pd_display_draw_rect(0, 0, dw, dh, PD_COLOR_DIM);

    /* Build up to 5 lines of info; render centered as a block on the canvas. */
    char lines[5][32];
    pd_display_color_t colors[5];
    int line_count = 0;
    if (device_name && device_name[0]) {
        snprintf(lines[line_count], sizeof(lines[0]), "%s", device_name);
        colors[line_count++] = PD_COLOR_CYAN;
    }
    snprintf(lines[line_count], sizeof(lines[0]), "%dx%d", res_w, res_h);
    colors[line_count++] = PD_COLOR_WHITE;
    snprintf(lines[line_count], sizeof(lines[0]), "rot %d sc %d", orient, scan);
    colors[line_count++] = PD_COLOR_WHITE;
    if (ip && ip[0]) {
        snprintf(lines[line_count], sizeof(lines[0]), "%s", ip);
        colors[line_count++] = PD_COLOR_GREEN;
    } else {
        snprintf(lines[line_count], sizeof(lines[0]), "DHCP");
        colors[line_count++] = PD_COLOR_DIM;
    }

    int block_h = line_count * PD_TINY_CELL_H;
    int y0 = (dh - block_h) / 2;
    if (y0 < 1) y0 = 1;

    for (int i = 0; i < line_count; i++) {
        int len = (int)strlen(lines[i]);
        int text_w = len * PD_TINY_CELL_W - (PD_TINY_CELL_W - PD_TINY_W);
        if (text_w < 0) text_w = 0;
        int x = (dw - text_w) / 2;
        if (x < 1) x = 1;
        pd_display_draw_text_tiny((uint16_t)x,
                                  (uint16_t)(y0 + i * PD_TINY_CELL_H),
                                  lines[i], colors[i]);
    }

    pd_display_driver->flip_buffer();
    ESP_LOGI(TAG, "render idle: %s %dx%d rot=%d scan=%d ip=%s",
             device_name ? device_name : "", res_w, res_h, orient, scan,
             ip ? ip : "dhcp");
}

extern "C" void pd_display_wizard_menu(const char *title, const char **options, int count, int selected)
{
    if (!pd_display_driver || !title || !options) {
        return;
    }

    pd_display_driver->clear();

    int rows = pd_display_driver->get_height() / PD_TINY_CELL_H;

    pd_display_draw_text_tiny(0, 0, title, PD_COLOR_YELLOW);

    int max_visible = rows - 1;
    if (max_visible < 1) {
        max_visible = 1;
    }

    int scroll_offset = 0;
    if (selected >= max_visible) {
        scroll_offset = selected - max_visible + 1;
    }

    for (int i = 0; i < max_visible && (i + scroll_offset) < count; i++) {
        int opt_idx = i + scroll_offset;
        uint16_t y = (uint16_t)((i + 1) * PD_TINY_CELL_H);
        pd_display_color_t color = (opt_idx == selected) ? PD_COLOR_GREEN : PD_COLOR_WHITE;

        if (opt_idx == selected) {
            pd_display_draw_char_tiny(0, y, '>', PD_COLOR_GREEN);
        }

        pd_display_draw_text_tiny(PD_TINY_CELL_W, y, options[opt_idx], color);
    }
}

extern "C" void pd_display_wizard_text(const char *title, const char *value, bool mask)
{
    if (!pd_display_driver || !title) {
        return;
    }

    pd_display_driver->clear();
    pd_display_draw_text_tiny(0, 0, title, PD_COLOR_YELLOW);

    if (value && value[0] != '\0') {
        if (mask) {
            size_t len = strlen(value);
            char masked[64];
            size_t mlen = (len < sizeof(masked) - 1) ? len : sizeof(masked) - 1;
            memset(masked, '*', mlen);
            masked[mlen] = '\0';
            pd_display_draw_text_tiny(0, PD_TINY_CELL_H, masked, PD_COLOR_WHITE);
        } else {
            pd_display_draw_text_tiny(0, PD_TINY_CELL_H, value, PD_COLOR_WHITE);
        }
    }

    pd_display_draw_char_tiny(0, (uint16_t)(PD_TINY_CELL_H * 2), '_', PD_COLOR_CYAN);
}

extern "C" void pd_display_wizard_status(const char *message)
{
    if (!pd_display_driver || !message) {
        return;
    }

    pd_display_driver->clear();
    pd_display_draw_text_tiny(0, 0, message, PD_COLOR_GREEN);
}

extern "C" void pd_display_render_default_marquee(void)
{
    if (!pd_display_driver) {
        return;
    }
    pd_display_driver->clear();
    int dw = pd_display_driver->get_width();
    int dh = pd_display_driver->get_height();
    /* Simple centered two-line text, large enough to read */
    const char *line1 = "DEFAULT";
    const char *line2 = "MARQUEE";
    /* each PD_CELL is small; use tiny renderer for small panels */
    int text_w = 7 * 4;  /* approx chars * cell width */
    int x = (dw - text_w) / 2; if (x < 0) x = 0;
    int y1 = dh / 2 - PD_TINY_CELL_H - 1;
    int y2 = dh / 2 + 1;
    pd_display_draw_text_tiny(x, y1, line1, PD_COLOR_YELLOW);
    pd_display_draw_text_tiny(x, y2, line2, PD_COLOR_YELLOW);
    pd_display_driver->flip_buffer();
}

extern "C" void pd_display_render_no_source(void)
{
    if (!pd_display_driver) {
        return;
    }

    pd_display_driver->clear();
    
    int row = 0;
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "NO MARQUEE SOURCE", PD_COLOR_YELLOW);
    row += 2;
    
    // Animated dots
    static int dots = 0;
    dots = (dots + 1) % 4;
    char searching[16];
    snprintf(searching, sizeof(searching), "Searching%.*s", dots, "...");
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, searching, PD_COLOR_CYAN);
    row += 2;
    
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "Device:", PD_COLOR_DIM);
    row++;
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "pixel-dumpster", PD_COLOR_WHITE);
    row++;
    
    char line[24];
    int dw = pd_display_driver->get_width();
    int dh = pd_display_driver->get_height();
    snprintf(line, sizeof(line), "%dx%d", dw, dh);
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, line, PD_COLOR_WHITE);
    
    pd_display_driver->flip_buffer();
}

extern "C" void pd_display_render_source_status(int state, const char *hostname, const char *ip,
                                                const char *es_version, bool browsing, bool launch,
                                                const char *methods)
{
    if (!pd_display_driver) {
        return;
    }

    pd_display_driver->clear();
    
    int row = 0;
    
    // Title
    pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "MARQUEE SOURCE", PD_COLOR_YELLOW);
    row += 2;
    
    // State indicator
    if (state == 0) {
        // Searching
        static int dots = 0;
        dots = (dots + 1) % 4;
        char msg[16];
        snprintf(msg, sizeof(msg), "searching%.*s", dots, "...");
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, msg, PD_COLOR_CYAN);
    } else if (state == 1) {
        // Connecting
        static int dots = 0;
        dots = (dots + 1) % 4;
        char msg[16];
        snprintf(msg, sizeof(msg), "connecting%.*s", dots, "...");
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, msg, PD_COLOR_CYAN);
    } else if (state == 2) {
        // Connected
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "connected!", PD_COLOR_GREEN);
    }
    row += 2;
    
    // Hostname
    if (hostname && hostname[0]) {
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, hostname, PD_COLOR_WHITE);
        row++;
    }
    
    // IP address
    if (ip && ip[0]) {
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, ip, PD_COLOR_CYAN);
        row++;
    }
    
    // ES version
    if (es_version && es_version[0]) {
        char line[24];
        snprintf(line, sizeof(line), "ES: %.10s", es_version);
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, line, PD_COLOR_DIM);
        row++;
    }
    
    // Event capabilities
    if (browsing && launch) {
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "Events: Full", PD_COLOR_GREEN);
    } else if (launch) {
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "Events: Launch", PD_COLOR_YELLOW);
    } else {
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, "Events: None", PD_COLOR_RED);
    }
    row++;
    
    // Methods (if provided and fits)
    if (methods && methods[0] && row < 10) {
        char line[24];
        snprintf(line, sizeof(line), "%.18s", methods);
        pd_display_draw_text_tiny(2, 2 + row * PD_TINY_CELL_H, line, PD_COLOR_DIM);
    }
    
    pd_display_driver->flip_buffer();
    ESP_LOGI(TAG, "render source status: state=%d host=%s", state, hostname ? hostname : "none");
}
