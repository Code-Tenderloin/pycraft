#include <Python.h>
#include <math.h>
#include <stdarg.h>

#include "render.h"

#include "colours.c"
#include "data.c"


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>


PyObject *C_RENDERER_EXCEPTION;

static PrintableChar *last_frame;
static bool resize;
static bool redraw_all;
static long width;
static long height;


#define S_POS_STR_FORMAT L"\033[%ld;%ldH"
#define POS_STR_FORMAT_MAX_LEN (sizeof(S_POS_STR_FORMAT))
static wchar_t *POS_STR_FORMAT = S_POS_STR_FORMAT;

size_t
pos_str(long x, long y, wchar_t *result)
{
    return swprintf(result, POS_STR_FORMAT_MAX_LEN, POS_STR_FORMAT, y+1, x+1);
}


#define debug_colour(c) debug(L"%f, %f, %f\n", (c).r, (c).g, (c).b)
void
debug(wchar_t *str, ...)
{
    static int debug_y = 0;
    static wchar_t debug_buff[128];
    size_t pos = pos_str(0, 50 + debug_y++, debug_buff);
    debug_buff[pos] = L'\0';

    wprintf(debug_buff);
    wprintf(L"\033[0K");

    va_list aptr;
    va_start(aptr, str);
    vwprintf(str, aptr);
    va_end(aptr);
    puts("\033[0K");

    if (debug_y > 20)
        debug_y = 0;
}


wchar_t
PyString_AsChar(PyObject *str)
{
    wchar_t result = 0;
    Py_ssize_t size;
    wchar_t *chars = PyUnicode_AsWideCharString(str, &size);
    if (chars && size > 0)
    {
        result = *chars;
    }
    return result;
}


long
get_long_from_PyDict_or(PyObject *dict, char key[], long default_result)
{
    long result = default_result;

    PyObject *item = PyDict_GetItemString(dict, key);
    if (item != NULL)
    {
         result = PyLong_AsLong(item);
    }

    return result;
}


wchar_t
get_block(long x, long y, PyObject *map)
{
    wchar_t result = 0;

    PyObject *column = PyDict_GetItem(map, PyLong_FromLong(x));
    if (column)
    {
        PyObject *block = NULL;
        if (y < PyList_Size(column))
        {
            block = PyList_GetItem(column, y);
        }

        if (block)
        {
            result = PyString_AsChar(block);
        }
    }

    return result;
}


float
lightness(Colour *rgb)
{
    return 0.2126f * rgb->r + 0.7152f * rgb->g + 0.0722f * rgb->b;
}


float
circle_dist(long test_x, long test_y, long x, long y, long r)
{
    return ( pow(test_x - x, 2.0f) / pow(r    , 2.0f) +
             pow(test_y - y, 2.0f) / pow(r*.5f, 2.0f) );
}


int
get_z_at_pos(long x, long y, PyObject *map, PyObject *slice_heights)
{
    PyObject *px = PyLong_FromLong(x);
    long slice_height = PyLong_AsLong(PyDict_GetItem(slice_heights, px));
    return (get_block_data(get_block(x, y, map))->solid || (world_gen_height - y) < slice_height) ? 0 : -1;
}


float
lit(long x, long y, long lx, long ly, long l_radius)
{
    return fmin(circle_dist(x, y, lx, ly, l_radius), 1.0f);
}


Colour
PyColour_AsColour(PyObject *py_colour)
{
    Colour rgb;
    rgb.r = -1;

    if (py_colour)
    {
        rgb.r = PyFloat_AsDouble(PyTuple_GetItem(py_colour, 0));
        rgb.g = PyFloat_AsDouble(PyTuple_GetItem(py_colour, 1));
        rgb.b = PyFloat_AsDouble(PyTuple_GetItem(py_colour, 2));
    }
    return rgb;
}


float
get_lightness(long x, long y, long world_x, PyObject *map, PyObject *slice_heights, PyObject *lights)
{
    /*
       Finds the brightest light level which reaches this position, then returns the light level from 0-1.
    */

    float min = 1.0f;
    int i = 0;
    PyObject *iter = PyObject_GetIter(lights);
    PyObject *light;

    // If the light is not hidden by the mask
    while ((light = PyIter_Next(iter)))
    {
        long lx = PyLong_AsLong(PyDict_GetItemString(light, "x"));
        long ly = PyLong_AsLong(PyDict_GetItemString(light, "y"));
        long z = PyLong_AsLong(PyDict_GetItemString(light, "z"));
        long l_radius = PyLong_AsLong(PyDict_GetItemString(light, "radius"));
        Colour rgb = PyColour_AsColour(PyDict_GetItemString(light, "colour"));

        float light_radius = lit(x, y, lx, ly, l_radius);
        bool is_lit = light_radius < 1 && z >= get_z_at_pos(world_x + lx, ly, map, slice_heights);
        float block_lightness = light_radius * lightness(&rgb);

        if (is_lit && block_lightness < min)
            min = block_lightness;

        ++i;
    }

    return 1 - min;
}


Colour
get_sky_colour(long x, long y, long world_x, PyObject *map, PyObject *slice_heights, PyObject *lights, Colour *colour_behind, Settings *settings)
{
    Colour result;
    result.r = -1;

    long slice_height = PyLong_AsLong(PyDict_GetItem(slice_heights, PyLong_FromLong(world_x + x)));
    if ((world_gen_height - y) < slice_height)
    {
        // Underground

        result.r = .1f;
        result.g = .1f;
        result.b = .1f;
        if (settings->fancy_lights > 0)
        {
            float block_lightness = get_lightness(x, y, world_x, map, slice_heights, lights);
            result.r = (result.r + block_lightness) * .5f;
            result.g = (result.g + block_lightness) * .5f;
            result.b = (result.b + block_lightness) * .5f;
        }
    }
    else
    {
        // Overground

        if (settings->fancy_lights > 0)
        {
            // Calculate light level for each light source
            int i = 0;
            PyObject *iter = PyObject_GetIter(lights);
            PyObject *light;
            float max_light_level = -1;
            Colour max_light_level_colour = {};

            while ((light = PyIter_Next(iter)))
            {
                long lx = PyLong_AsLong(PyDict_GetItemString(light, "x"));
                long ly = PyLong_AsLong(PyDict_GetItemString(light, "y"));
                long l_radius = PyLong_AsLong(PyDict_GetItemString(light, "radius"));
                float light_distance = lit(x, y, lx, ly, l_radius);
                if (light_distance < 1)
                {
                    Colour light_colour_rgb = PyColour_AsColour(PyDict_GetItemString(light, "colour"));
                    Colour light_colour_hsv = rgb_to_hsv(&light_colour_rgb);

                    Colour this_light_pixel_colour_hsv = lerp_colour(&light_colour_hsv, light_distance, colour_behind);
                    Colour this_light_pixel_colour_rgb = hsv_to_rgb(&this_light_pixel_colour_hsv);
                    float light_level = lightness(&this_light_pixel_colour_rgb);

                    if (light_level > max_light_level)
                    {
                        max_light_level = light_level;
                        max_light_level_colour = this_light_pixel_colour_rgb;
                    }
                }
                ++i;
            }

            // Get brightest light
            if (max_light_level >= 0)
            {
                result = max_light_level_colour;
            }
            else
            {
                result = hsv_to_rgb(colour_behind);
            }
        }
        else
        {
            result = *colour_behind;

            PyObject *iter = PyObject_GetIter(lights);
            PyObject *light;

            while ((light = PyIter_Next(iter)))
            {
                long lx = PyLong_AsLong(PyDict_GetItemString(light, "x"));
                long ly = PyLong_AsLong(PyDict_GetItemString(light, "y"));
                long l_radius = PyLong_AsLong(PyDict_GetItemString(light, "radius"));
                if (lit(x, y, lx, ly, l_radius) < 1)
                {
                    result = CYAN;
                    break;
                }
            }
        }
    }

    return result;
}


Colour
sky(long x, long y, long world_x, PyObject *map, PyObject *slice_heights, PyObject *bk_objects, Colour *sky_colour, PyObject *lights, Settings *settings)
{
    Colour result;
    result.r = -1;

    PyObject *iter = PyObject_GetIter(bk_objects);
    PyObject *object;

    while ((object = PyIter_Next(iter)))
    {
        long ox = PyLong_AsLong(PyDict_GetItemString(object, "x"));
        long oy = PyLong_AsLong(PyDict_GetItemString(object, "y"));
        long o_width= PyLong_AsLong(PyDict_GetItemString(object, "width"));
        long o_height= PyLong_AsLong(PyDict_GetItemString(object, "height"));

        if (x <= ox && ox < (x + o_width) &&
            y <= oy && oy < (y + o_height) &&
            (world_gen_height - y) > PyLong_AsLong(PyDict_GetItem(slice_heights, PyLong_FromLong(world_x + x))))
        {
            result = PyColour_AsColour(PyDict_GetItemString(object, "colour"));
            break;
        }
    }

    if (result.r < 0)
    {
        result = get_sky_colour(x, y, world_x, map, slice_heights, lights, sky_colour, settings);
    }

    return result;
}


wchar_t
get_char(long x, long y, PyObject *map, BlockData *pixel)
{
    // TODO: Impement a cache in get_block?

    wchar_t left_block_key = get_block(x-1, y, map);
    wchar_t right_block_key = get_block(x+1, y, map);
    wchar_t below_block_key = get_block(x, y+1, map);

    wchar_t character = pixel->character;

    if (below_block_key == 0 || !(get_block_data(below_block_key)->solid))
    {
        if (left_block_key != 0 && (get_block_data(left_block_key)->solid) && pixel->character_left != 0)
        {
            character = pixel->character_left;
        }
        else if (right_block_key != 0 && (get_block_data(right_block_key)->solid) && pixel->character_right != 0)
        {
            character = pixel->character_right;
        }
    }

    return character;
}


bool
printable_char_eq(PrintableChar *a, PrintableChar *b)
{
    return (a->character == b->character &&
            colour_eq(&(a->fg), &(b->fg)) &&
            colour_eq(&(a->bg), &(b->bg)) &&
            a->style == b->style);
}


void
get_obj_pixel(long x, long y, PyObject *objects, wchar_t *obj_key_result, Colour *obj_colour_result)
{
    PyObject *iter = PyObject_GetIter(objects);
    PyObject *object;

    while ((object = PyIter_Next(iter)))
    {
        long ox = PyLong_AsLong(PyDict_GetItemString(object, "x"));
        long oy = PyLong_AsLong(PyDict_GetItemString(object, "y"));

        if (ox == x && oy == y)
        {
            wchar_t c = PyString_AsChar(PyDict_GetItemString(object, "char"));
            Colour rgb = PyColour_AsColour(PyDict_GetItemString(object, "colour"));

            if (rgb.r == -1)
            {
                rgb = get_block_data(c)->colours.fg;
            }

            *obj_key_result = c;
            *obj_colour_result = rgb;
            return;
        }
    }
}


void
apply_block_lightness(Colour *result, float lightness)
{
    /*
       Applies a 0-1 lightness to a block colour
    */
    Colour hsv = rgb_to_hsv(result);
    hsv.v *= lightness;
    *result = hsv_to_rgb(&hsv);
}


void
get_lighting_buffer_pixel(LightingBuffer *lighting_buffer, int x, int y, struct PixelLighting **result)
{
    *result = lighting_buffer->screen + y * width + x;
}


void
create_lit_block(long x, long y, long world_x, long world_y, PyObject *map,
                 wchar_t pixel_f_key, PyObject *objects, LightingBuffer *lighting_buffer,
                 Settings *settings, PrintableChar *result)
{
    result->bg.r = -1;
    result->fg.r = -1;

    bool light_bg = false;
    bool light_fg = false;

    // Add block bg if not clear
    BlockData *pixel_f = get_block_data(pixel_f_key);
    if (pixel_f->colours.bg.r >= 0)
    {
        result->bg = pixel_f->colours.bg;
        light_bg = true;
    }

    // Add object fg if object, else block fg
    wchar_t obj_key = 0;
    Colour obj_colour;
    get_obj_pixel(x, world_y, objects, &obj_key, &obj_colour);
    if (obj_key != 0)
    {
        result->character = obj_key;
        result->fg = obj_colour;
        light_fg = false;
    }
    else
    {
        result->character = get_char(world_x, world_y, map, pixel_f);

        if (pixel_f->colours.fg.r >= 0)
        {
            result->fg = pixel_f->colours.fg;
            light_fg = true;
        }
    }

    if ((settings->fancy_lights > 0) &&
        (light_bg || light_fg))
    {
        struct PixelLighting *lighting_pixel;
        get_lighting_buffer_pixel(lighting_buffer, x, y, &lighting_pixel);

        float lightness = lighting_pixel->lightness;
        if (light_bg)
        {
            apply_block_lightness(&result->bg, lightness);
        }
        if (light_fg)
        {
            apply_block_lightness(&result->fg, lightness);
        }
    }

    result->style = pixel_f->colours.style;
}


bool
is_light_behind_a_solid_block(long lx, long ly, long l_height, long l_width, PyObject *map, long left_edge)
{
    bool result = true;

    long x, y;
    for (x = lx; x < lx+l_width; ++x)
    {
        for (y = ly; y > ly-l_height; --y)
        {
            wchar_t block_key = get_block(left_edge + x, y, map);

            if (block_key == 0 ||
                !get_block_data(block_key)->solid)
            {
                result = false;
                break;
            }
        }
    }

    return result;
}


bool
check_light_z(PyObject *py_light, Light *light, long left_edge, long top_edge, PyObject *map, PyObject *slice_heights)
{
    /*
        Lights with z of:
            -2 are not added to the lighting buffer as they are just graphical lights, like the moon.
            -1 are added to the lighting buffer IF they are above the ground, and not behind a solid block.
            0  are always added to the lighting buffer.
    */

    bool result = false;

    if (light->z == -2)
    {
        result = false;
    }
    else if (light->z == -1)
    {
        long buffer_ly = light->y - top_edge;

        // Check light source is above ground

        float ground_height_world = PyFloat_AsDouble(PyDict_GetItem(slice_heights, PyLong_FromLong(left_edge + light->x)));
        float ground_height_buffer = (world_gen_height - ground_height_world) - top_edge;

        if (buffer_ly < ground_height_buffer)
        {
            // Check light source is not behind a solid block

            long l_width = get_long_from_PyDict_or(py_light, "source_width", 1);
            long l_height = get_long_from_PyDict_or(py_light, "source_height", 1);

            if (!is_light_behind_a_solid_block(light->x, light->y, l_height, l_width, map, left_edge))
            {
                result = true;
            }
        }
    }
    else if (light->z == 0)
    {
        result = true;
    }

    return result;
}


void
add_light_pixel_lightness_to_lighting_buffer(int current_frame, struct PixelLighting *pixel, long x, long y, float light_distance, Light *light)
{
    // TODO: Figure out whether this would be better before (old version) or after inverting the distance?
    light_distance *= lightness(&light->rgb);

    float this_lightness = 1 - light_distance;
    // this_lightness *= lightness(&light->rgb);

    if (pixel->lightness < this_lightness ||
        pixel->lightness_set_on_frame != current_frame)
    {
        pixel->lightness = this_lightness;
        pixel->lightness_set_on_frame = current_frame;
    }
}


void
add_light_pixel_colour_to_lighting_buffer(int current_frame, struct PixelLighting *pixel, long x, long y, float light_distance, Light *light, PyObject *map, Colour *sky_colour, long left_edge, long top_edge)
{
    /*
        Adds the colour of the light's pixel for the light's light-radius' to the lighting buffer.
    */

    // Check if the background for this pixel is visible
    bool visible = false;

    // First, if the background for this pixel has already been set this frame, then the check has already passed.
    if (pixel->background_colour_set_on_frame == current_frame)
    {
        visible = true;
    }
    else
    {
        // Check if there is no block or a block without a clear background at this position.
        wchar_t block_key = get_block(left_edge+x, top_edge+y, map);
        if (block_key == 0 ||
            get_block_data(block_key)->colours.bg.r == -1)
        {
            visible = true;
        }
    }

    if (visible)
    {
        Colour hsv = lerp_colour(&light->hsv, light_distance, sky_colour);
        Colour rgb = hsv_to_rgb(&hsv);

        float pixel_background_colour_lightness = lightness(&rgb);

        if (pixel->background_colour_lightness < pixel_background_colour_lightness ||
            pixel->background_colour_set_on_frame != current_frame)
        {
            pixel->background_colour = rgb;
            pixel->background_colour_lightness = pixel_background_colour_lightness;
            pixel->background_colour_set_on_frame = current_frame;
        }
    }

}


void
add_daylight_lightness_to_lighting_buffer(LightingBuffer *lighting_buffer, PyObject *lights, PyObject *slice_heights, float day, long left_edge, long top_edge)
{
    /*
        Fills in all the gaps of the lightness lighting buffer with daylight, also overwrites darker than daylight parts.
    */
    long x, y;
    for (x = 0; x < width; ++x)
    {
        float ground_height_world = PyFloat_AsDouble(PyDict_GetItem(slice_heights, PyLong_FromLong(left_edge+x)));
        float ground_height_buffer = (world_gen_height - ground_height_world) - top_edge;

        for (y = 0; y < height; ++y)
        {
            float lightness;

            if (y < ground_height_buffer)
            {
                // Above ground
                lightness = day;
            }
            else if (y < ground_height_buffer + 3)
            {
                // Surface fade

                int d_ground = y - ground_height_buffer;
                float ground_fade = fmin(1.0f, ((float)d_ground / 3.0f));

                lightness = lerp(day, ground_fade, 0.0f);
            }
            else
            {
                // Underground
                lightness = 0;
            }

            struct PixelLighting *pixel;
            get_lighting_buffer_pixel(lighting_buffer, x, y, &pixel);

            if (pixel->lightness < lightness ||
                pixel->lightness_set_on_frame != lighting_buffer->current_frame)
            {
                pixel->lightness = lightness;
                pixel->lightness_set_on_frame = lighting_buffer->current_frame;
            }

            // TODO: Assert pixel->lightness_set_on_frame == lighting_buffer->current_frame
        }
    }
}


void
create_lighting_buffer(LightingBuffer *lighting_buffer, PyObject *lights, PyObject *map, PyObject *slice_heights, float day, Colour *sky_colour, long left_edge, long top_edge)
{
    /*
        - Store the lightness value for every block, calculated from the max of:
          - Lights (passed in from python)
            - Including sun (not moon), when sun is above ground height and not behind a block
          - Day value, fading to 0 at the ground

        - Also stores the background colour for pixels where the block in the map has a clear bg.
          - The colour of the light at radius `r` from the pixel is calculated with:
              hsv_to_rgb( lerp_colour( rgb_to_hsv(light_colour), r, sky_colour ) )
          - The colour is then selected by taking the max lightness of that colour from all the lights reaching this pixel.
    */

    ++lighting_buffer->current_frame;

    PyObject *iter = PyObject_GetIter(lights);
    PyObject *py_light;
    while ((py_light = PyIter_Next(iter)))
    {
        Light light = {
            .x = PyLong_AsLong(PyDict_GetItemString(py_light, "x")),
            .y = PyLong_AsLong(PyDict_GetItemString(py_light, "y")),
            .z = PyLong_AsLong(PyDict_GetItemString(py_light, "z")),
            .radius = PyLong_AsLong(PyDict_GetItemString(py_light, "radius")),
            .rgb = PyColour_AsColour(PyDict_GetItemString(py_light, "colour"))
        };
        light.hsv = rgb_to_hsv(&light.rgb);

        bool add_lightness = check_light_z(py_light, &light, left_edge, top_edge, map, slice_heights);

        long buffer_ly = light.y - top_edge;
        long x, y;
        for (x = light.x - light.radius; x <= light.x + light.radius; ++x)
        {
            for (y = buffer_ly - light.radius; y <= buffer_ly + light.radius; ++y)
            {
                // Is pixel on screen?
                if ((x >= 0 && x < width) &&
                    (y >= 0 && y < height))
                {
                    float light_distance = lit(x, y, light.x, buffer_ly, light.radius);
                    if (light_distance < 1)
                    {
                        struct PixelLighting *lighting_pixel;
                        get_lighting_buffer_pixel(lighting_buffer, x, y, &lighting_pixel);

                        if (add_lightness)
                        {
                            add_light_pixel_lightness_to_lighting_buffer(lighting_buffer->current_frame, lighting_pixel, x, y, light_distance, &light);
                        }

                        add_light_pixel_colour_to_lighting_buffer(lighting_buffer->current_frame, lighting_pixel, x, y, light_distance, &light, map, sky_colour, left_edge, top_edge);
                    }
                }
            }
        }
    }

    add_daylight_lightness_to_lighting_buffer(lighting_buffer, lights, slice_heights, day, left_edge, top_edge);
}


bool
terminal_out(ScreenBuffer *frame, PrintableChar *c, long x, long y, Settings *settings)
{
    size_t frame_pos = y * width + x;
    if (!printable_char_eq(last_frame + frame_pos, c) || resize || redraw_all)
    {
        last_frame[frame_pos] = *c;

        size_t old_cur_pos = frame->cur_pos;
        frame->cur_pos += pos_str(x, y, frame->buffer + frame->cur_pos);
        frame->cur_pos += colour_str(c, frame->buffer + frame->cur_pos, settings);

        if (frame->cur_pos >= frame->size)
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Exceeded frame buffer size in terminal_out!");
            return false;
        }
        if (frame->cur_pos - old_cur_pos >= (COLOUR_CODE_MAX_LEN + POS_STR_FORMAT_MAX_LEN))
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Block string length exceeded allocated space!");
            return false;
        }
    }

    return true;
}


bool
setup_frame(ScreenBuffer *frame, LightingBuffer *lighting_buffer, long new_width, long new_height)
{
    resize = false;
    if (new_width != width)
    {
        resize = true;
        width = new_width;
    }
    if (new_height != height)
    {
        resize = true;
        height = new_height;
    }

    if (resize)
    {
        frame->size = width * height * (POS_STR_FORMAT_MAX_LEN + COLOUR_CODE_MAX_LEN);
        frame->buffer = (wchar_t *)realloc(frame->buffer, frame->size * sizeof(wchar_t));
        if (!frame->buffer)
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Could not allocate frame buffer!");
            return false;
        }

        last_frame = (PrintableChar *)realloc(last_frame, width * height * sizeof(PrintableChar));
        if (!last_frame)
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Could not allocate last frame buffer!");
            return false;
        }

        lighting_buffer->screen = (struct PixelLighting *)realloc(lighting_buffer->screen, width * height * sizeof(struct PixelLighting));
        if (!lighting_buffer->screen)
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Could not allocate lighting map!");
            return false;
        }
    }

    frame->cur_pos = 0;
    return true;
}


static PyObject *
render_map(PyObject *self, PyObject *args)
{
    static ScreenBuffer frame;
    static LightingBuffer lighting_buffer = {.current_frame = 0};

    float day;

    long left_edge,
         right_edge,
         top_edge,
         bottom_edge;

    PyObject *map,
             *slice_heights,
             *objects,
             *bk_objects,
             *py_sky_colour,
             *lights,
             *py_settings;

    if (!PyArg_ParseTuple(args, "OO(ll)(ll)OOOfOOl:render", &map, &slice_heights,
            &left_edge, &right_edge, &top_edge, &bottom_edge, &objects, &bk_objects,
            &py_sky_colour, &day, &lights, &py_settings, &redraw_all))
    {
        PyErr_SetString(C_RENDERER_EXCEPTION, "Could not parse arguments!");
        return NULL;
    }

    Colour sky_colour = PyColour_AsColour(py_sky_colour);
    Settings settings = {
        .terminal_output = PyLong_AsLong(PyDict_GetItemString(py_settings, "terminal_output")),
        .fancy_lights = PyLong_AsLong(PyDict_GetItemString(py_settings, "fancy_lights")),
        .colours = PyLong_AsLong(PyDict_GetItemString(py_settings, "colours"))
    };

    long cur_width = right_edge - left_edge;
    long cur_height = bottom_edge - top_edge;

    if (!setup_frame(&frame, &lighting_buffer, cur_width, cur_height))
        return NULL;

    if (!PyDict_Check(map))
    {
        PyErr_SetString(C_RENDERER_EXCEPTION, "Map is not a dict!");
        return NULL;
    }

    // Create lighting buffer

    create_lighting_buffer(&lighting_buffer, lights, map, slice_heights, day, &sky_colour, left_edge, top_edge);

    // Print lit blocks and background

    Py_ssize_t i = 0;
    PyObject *world_x, *column;

    while (PyDict_Next(map, &i, &world_x, &column))
    {
        long world_x_l = PyLong_AsLong(world_x);
        if (!(world_x_l >= left_edge && world_x_l < right_edge))
            continue;

        if (!PyList_Check(column))
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "Column is not a list!");
            return NULL;
        }

        long x = world_x_l - left_edge;

        PyObject *iter = PyObject_GetIter(column);
        PyObject *py_pixel;
        long world_y_l = 0;

        while ((py_pixel = PyIter_Next(iter)))
        {
            if (world_y_l >= top_edge && world_y_l < bottom_edge)
            {
                long y = world_y_l - top_edge;

                wchar_t pixel = PyString_AsChar(py_pixel);
                if (!pixel)
                {
                    PyErr_SetString(C_RENDERER_EXCEPTION, "Cannot get char from pixel!");
                    return NULL;
                }

                PrintableChar printable_char;
                create_lit_block(x, y, world_x_l, world_y_l, map, pixel, objects, &lighting_buffer, &settings, &printable_char);

                if (settings.terminal_output > 0)
                {
                    if (!terminal_out(&frame, &printable_char, x, y, &settings))
                        return NULL;
                }
            }

            ++world_y_l;
            Py_XDECREF(py_pixel);
        }

        Py_XDECREF(iter);
    }

    if (settings.terminal_output > 0)
    {
        frame.buffer[frame.cur_pos] = L'\0';
        int n_wprintf_written = wprintf(frame.buffer);

        if (n_wprintf_written != frame.cur_pos)
        {
            PyErr_SetString(C_RENDERER_EXCEPTION, "wfprint messed up!");
            return NULL;
        }
        fflush(stdout);
    }

    Py_RETURN_NONE;
}


static PyMethodDef render_c_methods[] = {
    {"render_map", render_map, METH_VARARGS, PyDoc_STR("render_map(map, edges, edges_y, slice_heights, objects, bk_objects, sky_colour, day, lights, settings, redraw_all) -> None")},
    {NULL, NULL}  /* sentinel */
};

PyDoc_STRVAR(module_doc, "The super-duper-fast renderer");

static struct PyModuleDef render_c_module = {
    PyModuleDef_HEAD_INIT,
    "render",
    module_doc,
    -1,
    render_c_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit_render_c(void)
{
    PyObject *m = NULL;

    // Create the module and add the functions
    m = PyModule_Create(&render_c_module);
    if (m == NULL)
    {
        Py_XDECREF(m);
    }

    C_RENDERER_EXCEPTION = PyErr_NewException("render_c.RendererException", NULL, NULL);

    return m;
}
