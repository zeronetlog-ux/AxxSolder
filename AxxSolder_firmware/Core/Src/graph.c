#include "main.h"
#include "app_colors.h"
#include "lcd.h"
#include "display_app.h"   /* convert_temperature, display_app_graph_invalidate */
#include "settings.h"
#include "handle.h"
#include "util.h"
#include "graph.h"
#include "string.h"
#include <stdio.h>
#include <math.h>
#include "gui.h"

/* ------------------------------------------------------------------ */
/* Graph view: JBC-style screen, portrait and landscape               */
/* ------------------------------------------------------------------ */

#define TEMP_MIN 0
#define TEMP_MAX_C 450
#define TEMP_MAX_F 850
#define GRAPH_POINTS 100

uint16_t GRAPH_X0;
uint16_t GRAPH_Y0;
uint16_t GRAPH_WIDTH;
uint16_t GRAPH_HEIGHT;

uint8_t initialized = 0;

/* 240x320: graph on top, big tip temperature bottom-left, info pane
 * bottom-right. */
static const graph_layout_t layout_portrait = {
    .x0 = 40, .y0 = 12, .w = 156, .h = 180,
    .rule_y = 230, .div_x = 130,
    .big_x = 8, .big_y = 238,
    .ring_x = 110, .ring_y = 244,
    .c_x = 116, .c_y = 238,
    .sel_y = 286,
    .info_x = 132, .info_value_x = 174, .info_y0 = 236, .info_pitch = 24,
};

/* 320x240: same elements squeezed into the shorter axis. */
static const graph_layout_t layout_landscape = {
    .x0 = 40, .y0 = 8, .w = 240, .h = 124,
    .rule_y = 154, .div_x = 140,
    .big_x = 10, .big_y = 160,
    .ring_x = 112, .ring_y = 166,
    .c_x = 118, .c_y = 160,
    .sel_y = 208,
    .info_x = 150, .info_value_x = 205, .info_y0 = 160, .info_pitch = 24,
};

graph_layout_t graph_layout;

typedef struct {
    int16_t  temp_min;
    int16_t  temp_max;
    uint16_t power_max;   /* watts for the labels, 0 = percent axis */
} graph_scale_t;

static graph_scale_t graph_scale;

static uint16_t temp_array[GRAPH_POINTS];
static uint16_t power_array[GRAPH_POINTS];
static uint8_t  index_graph = 0;

typedef struct {
    int x1, y1, x2, y2;
    uint16_t color;
} Line;

static Line temp_lines_prev[GRAPH_POINTS - 1];
static Line power_lines_prev[GRAPH_POINTS - 1];

/* ------------------------------------------------------------------ */
/* Geometry / scaling                                                 */
/* ------------------------------------------------------------------ */

static bool graph_is_portrait(void)
{
    return (flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2);
}

static uint8_t graph_tb_index(void)
{
    int idx = (int)flash_values.graph_timebase;
    return (uint8_t)((idx < 0 || idx > 4) ? 1 : idx);
}

uint8_t graph_timebase_points(void)
{
    static const uint8_t points[5] = {50, 100, 100, 100, 100};
    return points[graph_tb_index()];
}

uint8_t graph_timebase_decim(void)
{
    static const uint8_t decim[5] = {1, 1, 2, 5, 10};
    return decim[graph_tb_index()];
}

static const char *graph_tb_label(void)
{
    static const char *label[5] = {"0.5s/div", "1s/div", "2s/div", "5s/div", "10s/div"};
    return label[graph_tb_index()];
}

static uint16_t graph_power_axis_max(void)
{
    int watts;

    if (flash_values.power_unit != 0) {
        return 100;
    }
    watts = (int)sensor_values.max_power_watt;
    if (watts < 5) {
        watts = 5;
    }
    return (uint16_t)(((watts + 4) / 5) * 5);
}

static void graph_update_scale(void)
{
    const int step = (flash_values.deg_celsius == 1) ? 5 : 10;
    const int top  = (flash_values.deg_celsius == 1) ? TEMP_MAX_C : TEMP_MAX_F;
    int centre = (int)convert_temperature(sensor_values.set_temperature);
    int lo, hi;

    centre = ((centre + step / 2) / step) * step;

    if ((graph_scale.temp_max <= graph_scale.temp_min) ||
        (centre < (int)graph_scale.temp_min + step) ||
        (centre > (int)graph_scale.temp_max - step)) {
        lo = centre - (2 * step);
        hi = centre + (2 * step);
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > top) { lo -= (hi - top); hi = top; }
        if (lo < 0) { lo = 0; }

        graph_scale.temp_min = (int16_t)lo;
        graph_scale.temp_max = (int16_t)hi;
    }
    graph_scale.power_max = graph_power_axis_max();
}

static int graph_temp_to_y(uint16_t temp)
{
    int span = (int)graph_scale.temp_max - (int)graph_scale.temp_min;
    int y;

    /* Outside the zoom window the curve is not shown at all. */
    if (((int)temp < (int)graph_scale.temp_min) || ((int)temp > (int)graph_scale.temp_max)) {
        return -1;
    }
    if (span <= 0) {
        span = 1;
    }
    y = GRAPH_Y0 - (((int)temp - (int)graph_scale.temp_min) * GRAPH_HEIGHT) / span;
    return y;
}

static int graph_power_to_y(uint16_t power_tenths)
{
    int span = (int)graph_scale.power_max * 10;
    int y;

    if (span < 10) {
        span = 10;
    }
    /* More power than the axis shows: hide the curve. */
    if ((int)power_tenths > span) {
        return -1;
    }
    y = GRAPH_Y0 - ((int)power_tenths * GRAPH_HEIGHT) / span;
    return y;
}

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                    */
/* ------------------------------------------------------------------ */

void draw_line_with_thickness(int x1, int y1, int x2, int y2, uint16_t color, uint8_t thickness)
{
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        UG_FillFrame(x1, y1, x1 + (thickness - 1), y1 + (thickness - 1), color);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void draw_dashed_line_fillframe(int x1, int y1, int x2, int y2, uint16_t color,
                                uint8_t dash_len, uint8_t gap_len, uint8_t thickness)
{
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    uint8_t phase = 0;
    uint8_t draw = 1;
    uint8_t segment_len = dash_len;

    while (1) {
        if (draw) {
            UG_FillFrame(x1, y1, x1 + (thickness - 1), y1 + (thickness - 1), color);
        }
        phase++;
        if (phase >= segment_len) {
            phase = 0;
            draw = !draw;
            segment_len = draw ? dash_len : gap_len;
        }
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

/* Degree symbol + unit letter (the 17x18/12x15 faces have no '°'). */
static void deg_c(int x, int y, int r, int dx, char letter, UG_FONT *f, uint16_t color)
{
    char b[2] = { letter, 0 };

    UG_DrawCircle(x + r, y + r + 1, r, color);
    LCD_PutStr((uint16_t)(x + dx), (uint16_t)y, b, f, color, C_Background);
}

/* Axis labels: power left (white, right-aligned), temperature right (red).
 * Called from the init and whenever the zoom window / power budget moves. */
static void draw_axis_labels(void)
{
    const int step = (flash_values.deg_celsius == 1) ? 5 : 10;
    const int x0 = graph_layout.x0;
    const int y_top = graph_layout.y0;
    const int y_bot = graph_layout.y0 + graph_layout.h - 18;
    const int right_x = x0 + graph_layout.w + 4;
    char b[8];

    for (int i = 0; i < 5; i++) {
        int y = y_bot - (i * (y_bot - y_top)) / 4;
        int power;
        int clear_right = right_x + 32;
        int sw = graph_is_portrait() ? 240 : 320;

        if (clear_right > sw - 3) {
            clear_right = sw - 3;
        }

        UG_FillFrame(x0 - 36, y, x0 - 4, y + 17, C_Background);
        UG_FillFrame(right_x, y, clear_right, y + 17, C_Background);

        snprintf(b, sizeof(b), "%d", graph_scale.temp_min + (i * step));
        LCD_PutStr((uint16_t)right_x, (uint16_t)y, b, FONT_arial_17X18, C_JbcRed, C_Background);

        power = (flash_values.power_unit == 0)
              ? (i * 25 * (int)graph_scale.power_max + 50) / 100
              : i * 25;
        snprintf(b, sizeof(b), "%d", power);
        {
            int digits = (power >= 100) ? 3 : ((power >= 10) ? 2 : 1);
            LCD_PutStr((uint16_t)(x0 - 4 - digits * 10), (uint16_t)y, b,
                       FONT_arial_17X18, C_Text, C_Background);
        }
    }
}

/* Redraw the 4 inner horizontal + 4 vertical grid lines (solid 1 px). */
static void draw_grid_lines(void)
{
    for (int i = 1; i <= 4; i++) {
        int gy = GRAPH_Y0 - (i * GRAPH_HEIGHT) / 5;
        UG_DrawLine(GRAPH_X0, gy, GRAPH_X0 + GRAPH_WIDTH, gy, C_JbcGrid);
    }
    for (int i = 1; i <= 4; i++) {
        int gx = GRAPH_X0 + (i * GRAPH_WIDTH) / 5;
        UG_DrawLine(gx, GRAPH_Y0 - GRAPH_HEIGHT, gx, GRAPH_Y0, C_JbcGrid);
    }
}

static void graph_draw_static(void)
{
    const uint16_t sw = graph_is_portrait() ? 240 : 320;
    const uint16_t sh = graph_is_portrait() ? 320 : 240;
    const uint16_t x0 = graph_layout.x0;
    const uint16_t y0 = graph_layout.y0;
    const uint16_t w  = graph_layout.w;
    const uint16_t h  = graph_layout.h;
    const char unit = (flash_values.deg_celsius == 1) ? 'C' : 'F';
    static const char *names[3] = {"Tip", "Vin", "Pwr"};

    /* no screen border, no plot frame: only the inner grid lines */
    draw_grid_lines();

    draw_axis_labels();

    /* units + time base in the strip below the data area */
    LCD_PutStr(x0 - 4 - 12, y0 + h + 4, "W", FONT_arial_12X15, C_Text, C_Background);
    deg_c(x0 + w + 4, y0 + h + 4, 2, 6, unit, FONT_arial_12X15, C_JbcRed);
    LCD_PutStr((uint16_t)(x0 + w - 58), (uint16_t)(y0 + h + 4),
               (char *)graph_tb_label(), FONT_arial_12X15, C_Text, C_Background);

    /* readout pane */
    UG_DrawLine(0, graph_layout.rule_y, sw - 1, graph_layout.rule_y, C_Text);
    UG_DrawLine(graph_layout.div_x, graph_layout.rule_y + 2,
                graph_layout.div_x, sh - 1, C_JbcGrid);

    for (uint8_t i = 0; i < 3; i++) {
        uint16_t y = (uint16_t)(graph_layout.info_y0 + i * graph_layout.info_pitch);
        LCD_PutStr(graph_layout.info_x, y, (char *)names[i],
                   FONT_arial_20X23, C_Text, C_Background);
        if (i) {
            draw_dashed_line_fillframe(graph_layout.info_x, (int16_t)(y - 5),
                                       (int16_t)(sw - 6), (int16_t)(y - 5),
                                       C_JbcGrid, 2, 2, 1);
        }
    }
}

void draw_graph_init(void)
{
    UG_FillScreen(C_Background);
    UG_FontSetTransparency(0);

    memset(temp_lines_prev, 0, sizeof(temp_lines_prev));
    memset(power_lines_prev, 0, sizeof(power_lines_prev));

    graph_layout = graph_is_portrait() ? layout_portrait : layout_landscape;
    GRAPH_X0 = graph_layout.x0;
    GRAPH_Y0 = graph_layout.y0 + graph_layout.h;
    GRAPH_WIDTH = graph_layout.w;
    GRAPH_HEIGHT = graph_layout.h;

    graph_update_scale();
    graph_draw_static();

    initialized = 1;
    display_app_graph_invalidate();
}

/* Restore the solid grid at a given point: repaint a whole 5 px segment of
 * the grid line (the same idea that made the dashed grid stable - a single
 * pixel would leave gaps behind the 2 px erase brush). */
static void restore_grid_point(int x, int y)
{
    for (int i = 1; i <= 4; i++) {
        int grid_y = GRAPH_Y0 - (i * GRAPH_HEIGHT) / 5;
        if (abs(y - grid_y) <= 1) {
            int xs = x - 2;
            int xe = x + 2;

            if (xs < GRAPH_X0) { xs = GRAPH_X0; }
            if (xe > GRAPH_X0 + GRAPH_WIDTH) { xe = GRAPH_X0 + GRAPH_WIDTH; }
            UG_FillFrame(xs, grid_y, xe, grid_y, C_JbcGrid);
            return;
        }
    }
    for (int i = 1; i <= 4; i++) {
        int grid_x = GRAPH_X0 + (i * GRAPH_WIDTH) / 5;
        if (abs(x - grid_x) <= 1) {
            int ys = y - 2;
            int ye = y + 2;

            if (ys < GRAPH_Y0 - GRAPH_HEIGHT) { ys = GRAPH_Y0 - GRAPH_HEIGHT; }
            if (ye > GRAPH_Y0) { ye = GRAPH_Y0; }
            UG_FillFrame(grid_x, ys, grid_x, ye, C_JbcGrid);
            return;
        }
    }
}

static void erase_line_and_restore_grid(int x1, int y1, int x2, int y2)
{
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    draw_line_with_thickness(x1, y1, x2, y2, C_Background, 2);

    while (1) {
        restore_grid_point(x1, y1);
        if (x1 == x2 && y1 == y2) break;

        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx)  { err += dx; y1 += sy; }
    }
}

static bool lines_overlap(Line *l1, Line *l2, int thickness)
{
    int l1_xmin = l1->x1 < l1->x2 ? l1->x1 : l1->x2;
    int l1_xmax = l1->x1 > l1->x2 ? l1->x1 : l1->x2;
    int l1_ymin = l1->y1 < l1->y2 ? l1->y1 : l1->y2;
    int l1_ymax = l1->y1 > l1->y2 ? l1->y1 : l1->y2;
    int l2_xmin = l2->x1 < l2->x2 ? l2->x1 : l2->x2;
    int l2_xmax = l2->x1 > l2->x2 ? l2->x1 : l2->x2;
    int l2_ymin = l2->y1 < l2->y2 ? l2->y1 : l2->y2;
    int l2_ymax = l2->y1 > l2->y2 ? l2->y1 : l2->y2;

    l1_xmin -= thickness; l1_xmax += thickness;
    l1_ymin -= thickness; l1_ymax += thickness;
    l2_xmin -= thickness; l2_xmax += thickness;
    l2_ymin -= thickness; l2_ymax += thickness;

    return (l1_xmin <= l2_xmax) && (l1_xmax >= l2_xmin) &&
           (l1_ymin <= l2_ymax) && (l1_ymax >= l2_ymin);
}

static bool line_valid(const Line *l)
{
    return (l->x1 || l->y1 || l->x2 || l->y2);
}

static void graph_make_lines(uint8_t i, uint8_t points, Line *temp, Line *power)
{
    uint8_t idx_prev = (index_graph + GRAPH_POINTS - i) % GRAPH_POINTS;
    uint8_t idx_curr = (index_graph + GRAPH_POINTS - i - 1) % GRAPH_POINTS;
    int x_prev = GRAPH_X0 + ((points - i) * GRAPH_WIDTH) / (points - 1);
    int x_curr = GRAPH_X0 + ((points - i - 1) * GRAPH_WIDTH) / (points - 1);
    int ty_prev = graph_temp_to_y(temp_array[idx_prev]);
    int ty_curr = graph_temp_to_y(temp_array[idx_curr]);
    int py_prev = graph_power_to_y(power_array[idx_prev]);
    int py_curr = graph_power_to_y(power_array[idx_curr]);

    temp->color = C_JbcRed;
    power->color = C_Text;

    if ((ty_prev < 0) || (ty_curr < 0)) {
        temp->x1 = temp->y1 = temp->x2 = temp->y2 = 0;
    } else {
        temp->x1 = x_prev;
        temp->y1 = ty_prev;
        temp->x2 = x_curr;
        temp->y2 = ty_curr;
    }

    if ((py_prev < 0) || (py_curr < 0)) {
        power->x1 = power->y1 = power->x2 = power->y2 = 0;
    } else {
        power->x1 = x_prev;
        power->y1 = py_prev;
        power->x2 = x_curr;
        power->y2 = py_curr;
    }
}

void draw_graph_update(void)
{
    if (!initialized) {
        draw_graph_init();
    }

    {
        int16_t  old_min  = graph_scale.temp_min;
        int16_t  old_max  = graph_scale.temp_max;
        uint16_t old_pmax = graph_scale.power_max;

        graph_update_scale();
        if ((old_min != graph_scale.temp_min) || (old_max != graph_scale.temp_max) ||
            (old_pmax != graph_scale.power_max)) {
            draw_axis_labels();
        }
    }

    const uint8_t points = graph_timebase_points();

    for (uint8_t i = 1; i < points; i++) {
        Line new_temp, new_power;
        Line *old_temp = &temp_lines_prev[i - 1];
        Line *old_power = &power_lines_prev[i - 1];
        bool temp_changed, power_changed, old_temp_valid, old_power_valid;

        graph_make_lines(i, points, &new_temp, &new_power);

        temp_changed = (old_temp->x1 != new_temp.x1) || (old_temp->y1 != new_temp.y1) ||
                       (old_temp->x2 != new_temp.x2) || (old_temp->y2 != new_temp.y2) ||
                       (old_temp->color != new_temp.color);
        power_changed = (old_power->x1 != new_power.x1) || (old_power->y1 != new_power.y1) ||
                        (old_power->x2 != new_power.x2) || (old_power->y2 != new_power.y2) ||
                        (old_power->color != new_power.color);

        old_temp_valid  = line_valid(old_temp);
        old_power_valid = line_valid(old_power);

        if (temp_changed || power_changed) {
            if (old_temp_valid && old_power_valid && lines_overlap(old_temp, old_power, 2)) {
                erase_line_and_restore_grid(old_temp->x1, old_temp->y1, old_temp->x2, old_temp->y2);
                erase_line_and_restore_grid(old_power->x1, old_power->y1, old_power->x2, old_power->y2);
            } else {
                if (temp_changed && old_temp_valid) {
                    erase_line_and_restore_grid(old_temp->x1, old_temp->y1, old_temp->x2, old_temp->y2);
                }
                if (power_changed && old_power_valid) {
                    erase_line_and_restore_grid(old_power->x1, old_power->y1, old_power->x2, old_power->y2);
                }
            }
        }

        if (line_valid(&new_power)) {
            draw_line_with_thickness(new_power.x1, new_power.y1, new_power.x2, new_power.y2, new_power.color, 2);
        }
        if (line_valid(&new_temp)) {
            draw_line_with_thickness(new_temp.x1, new_temp.y1, new_temp.x2, new_temp.y2, new_temp.color, 2);
        }

        *old_temp = new_temp;
        *old_power = new_power;
    }
}

void add_data_point(uint16_t temp, uint16_t power, uint16_t set_temp)
{
    (void)set_temp;   /* the reference/setpoint trace is no longer drawn */

    if (flash_values.deg_celsius == 1) {
        temp = clamp(temp, 0, TEMP_MAX_C);
    } else {
        temp = clamp(temp, 0, TEMP_MAX_F);
    }
    if (power > 1500) {
        power = 1500;
    }

    temp_array[index_graph] = temp;
    power_array[index_graph] = power;
    index_graph++;
    if (index_graph >= GRAPH_POINTS) index_graph = 0;
}
