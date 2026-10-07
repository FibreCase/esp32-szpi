#include <assert.h>
#include <stddef.h>
#include "../components/szpi_input/src/szpi_input_transform.h"

int main(void)
{
    uint16_t x = 0;
    uint16_t y = 0;

    assert(szpi_input_map_coordinates(0, 319, &x, &y) && x == 0 && y == 0);
    assert(szpi_input_map_coordinates(0, 0, &x, &y) && x == 319 && y == 0);
    assert(szpi_input_map_coordinates(239, 319, &x, &y) && x == 0 && y == 239);
    assert(szpi_input_map_coordinates(239, 0, &x, &y) && x == 319 && y == 239);
    assert(szpi_input_map_coordinates(120, 160, &x, &y) && x == 159 && y == 120);
    assert(!szpi_input_map_coordinates(240, 0, &x, &y));
    assert(!szpi_input_map_coordinates(0, 320, &x, &y));
    assert(!szpi_input_map_coordinates(0, 0, NULL, &y));
    assert(!szpi_input_map_coordinates(0, 0, &x, NULL));

    szpi_input_raw_touch_t points[] = {
        {.id = 3, .event = 2},
        {.id = 7, .event = 2},
    };
    assert(szpi_input_select_primary(points, 2, true, 7) == 1);
    points[1].event = SZPI_INPUT_EVENT_LIFT_UP;
    assert(szpi_input_select_primary(points, 2, true, 7) == 0);
    points[0].event = SZPI_INPUT_EVENT_NO_EVENT;
    assert(szpi_input_select_primary(points, 2, true, 7) == -1);
    assert(szpi_input_select_primary(NULL, 2, false, 0) == -1);
    return 0;
}
