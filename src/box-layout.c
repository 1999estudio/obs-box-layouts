#include "box-layout.h"

#include <obs-frontend-api.h>

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <media-io/audio-io.h>
#include <pthread.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_BOXES 6
#define MAX_CHILDREN (MAX_BOXES + 1)
#define ROUNDED_CORNER_SEGMENTS 12
#define ROUNDED_PERIMETER_POINTS (4 * (ROUNDED_CORNER_SEGMENTS + 1))
#define PI_F 3.14159265358979323846f

enum layout_preset {
	PRESET_SINGLE = 0,
	PRESET_TWO_COLUMNS,
	PRESET_TWO_ROWS,
	PRESET_THREE_COLUMNS,
	PRESET_HERO_LEFT,
	PRESET_GRID_FOUR,
	PRESET_HERO_TOP,
	PRESET_GRID_SIX,
	PRESET_CUSTOM,
};

enum resize_edges {
	RESIZE_NONE = 0,
	RESIZE_LEFT = 1 << 0,
	RESIZE_RIGHT = 1 << 1,
	RESIZE_TOP = 1 << 2,
	RESIZE_BOTTOM = 1 << 3,
};

enum content_fit_mode {
	FIT_FILL = 0,
	FIT_CONTAIN,
	FIT_STRETCH,
	FIT_MANUAL,
};

struct box_rect {
	float x;
	float y;
	float width;
	float height;
};

struct layout_box {
	obs_source_t *source;
	char *source_name;
	bool active;
	float zoom;
	float pan_x;
	float pan_y;
	int fit_mode;
	float radius;
	float border_width;
	uint32_t border_color;
	float custom_x;
	float custom_y;
	float custom_width;
	float custom_height;
	int z_index;
	bool lock_aspect;
};

struct box_layout {
	obs_source_t *context;
	pthread_mutex_t mutex;
	uint32_t width;
	uint32_t height;
	int preset;
	int custom_box_count;
	float gap;
	uint32_t background_color;
	obs_source_t *background;
	char *background_name;
	bool background_active;
	struct layout_box boxes[MAX_BOXES];
	gs_texrender_t *texrender;
	gs_texture_t *blank_texture;
	gs_effect_t *effect;
	gs_eparam_t *image_param;
	gs_eparam_t *box_size_param;
	gs_eparam_t *uv_scale_param;
	gs_eparam_t *uv_offset_param;
	gs_eparam_t *radius_param;
	gs_eparam_t *border_width_param;
	gs_eparam_t *border_color_param;
	bool effect_ready;
	bool graphics_warning_logged;
	bool dragging;
	bool drag_content;
	bool drag_converted;
	bool drag_move_logged;
	int drag_box;
	uint32_t drag_edges;
	int32_t drag_start_x;
	int32_t drag_start_y;
	struct box_rect drag_start_rect;
	float drag_start_pan_x;
	float drag_start_pan_y;
};

struct box_render_state {
	obs_source_t *source;
	float zoom;
	float pan_x;
	float pan_y;
	int fit_mode;
	float radius;
	float border_width;
	uint32_t border_color;
};

static int preset_box_count(int preset)
{
	switch (preset) {
	case PRESET_SINGLE:
		return 1;
	case PRESET_TWO_COLUMNS:
	case PRESET_TWO_ROWS:
		return 2;
	case PRESET_THREE_COLUMNS:
	case PRESET_HERO_LEFT:
		return 3;
	case PRESET_GRID_FOUR:
	case PRESET_HERO_TOP:
		return 4;
	case PRESET_GRID_SIX:
	case PRESET_CUSTOM:
	default:
		return 6;
	}
}

static int layout_box_count(const struct box_layout *layout)
{
	int count = layout->preset == PRESET_CUSTOM ? layout->custom_box_count : preset_box_count(layout->preset);
	count = count < 1 ? 1 : count;
	return count > MAX_BOXES ? MAX_BOXES : count;
}

static void set_rect(struct box_rect *rect, float x, float y, float width, float height)
{
	rect->x = x;
	rect->y = y;
	rect->width = fmaxf(width, 1.0f);
	rect->height = fmaxf(height, 1.0f);
}

static int calculate_rects(const struct box_layout *layout, struct box_rect rects[MAX_BOXES])
{
	const float w = (float)layout->width;
	const float h = (float)layout->height;
	if (layout->preset == PRESET_CUSTOM) {
		const int count = layout_box_count(layout);
		for (int i = 0; i < count; i++) {
			const struct layout_box *box = &layout->boxes[i];
			const float x = fmaxf(0.0f, fminf(box->custom_x, w - 1.0f));
			const float y = fmaxf(0.0f, fminf(box->custom_y, h - 1.0f));
			set_rect(&rects[i], x, y, fminf(box->custom_width, w - x), fminf(box->custom_height, h - y));
		}
		return count;
	}

	const float g = fmaxf(layout->gap, 0.0f);
	const float half_w = (w - g) * 0.5f;
	const float half_h = (h - g) * 0.5f;
	const float third_w = (w - 2.0f * g) / 3.0f;

	switch (layout->preset) {
	case PRESET_SINGLE:
		set_rect(&rects[0], 0.0f, 0.0f, w, h);
		break;
	case PRESET_TWO_COLUMNS:
		set_rect(&rects[0], 0.0f, 0.0f, half_w, h);
		set_rect(&rects[1], half_w + g, 0.0f, half_w, h);
		break;
	case PRESET_TWO_ROWS:
		set_rect(&rects[0], 0.0f, 0.0f, w, half_h);
		set_rect(&rects[1], 0.0f, half_h + g, w, half_h);
		break;
	case PRESET_THREE_COLUMNS:
		for (int i = 0; i < 3; i++)
			set_rect(&rects[i], i * (third_w + g), 0.0f, third_w, h);
		break;
	case PRESET_HERO_LEFT: {
		const float hero_w = (w - g) * 0.666667f;
		const float side_w = w - hero_w - g;
		set_rect(&rects[0], 0.0f, 0.0f, hero_w, h);
		set_rect(&rects[1], hero_w + g, 0.0f, side_w, half_h);
		set_rect(&rects[2], hero_w + g, half_h + g, side_w, half_h);
		break;
	}
	case PRESET_GRID_FOUR:
		set_rect(&rects[0], 0.0f, 0.0f, half_w, half_h);
		set_rect(&rects[1], half_w + g, 0.0f, half_w, half_h);
		set_rect(&rects[2], 0.0f, half_h + g, half_w, half_h);
		set_rect(&rects[3], half_w + g, half_h + g, half_w, half_h);
		break;
	case PRESET_HERO_TOP:
		set_rect(&rects[0], 0.0f, 0.0f, w, (h - g) * 0.666667f);
		for (int i = 0; i < 3; i++)
			set_rect(&rects[i + 1], i * (third_w + g), (h - g) * 0.666667f + g, third_w,
				 h - ((h - g) * 0.666667f + g));
		break;
	case PRESET_GRID_SIX:
	default:
		for (int row = 0; row < 2; row++)
			for (int col = 0; col < 3; col++)
				set_rect(&rects[row * 3 + col], col * (third_w + g), row * (half_h + g), third_w,
					 half_h);
		break;
	}

	return layout_box_count(layout);
}

static void remove_child(struct box_layout *layout, obs_source_t **slot, bool *active)
{
	if (!*slot)
		return;

	if (*active)
		obs_source_remove_active_child(layout->context, *slot);
	obs_source_release(*slot);
	*slot = NULL;
	*active = false;
}

static void replace_child(struct box_layout *layout, obs_source_t **slot, bool *active, const char *name,
			  bool should_be_active)
{
	if (*slot && !obs_source_removed(*slot) && name && strcmp(obs_source_get_name(*slot), name) == 0) {
		if (*active && !should_be_active) {
			obs_source_remove_active_child(layout->context, *slot);
			*active = false;
		} else if (!*active && should_be_active) {
			if (!obs_source_add_active_child(layout->context, *slot)) {
				blog(LOG_WARNING, "[obs-box-layouts] rejected recursive child '%s'", name);
				obs_source_release(*slot);
				*slot = NULL;
				return;
			}
			*active = true;
		}
		return;
	}

	remove_child(layout, slot, active);
	if (!name || !*name)
		return;

	obs_source_t *candidate = obs_get_source_by_name(name);
	if (!candidate || candidate == layout->context ||
	    !(obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO)) {
		if (candidate)
			obs_source_release(candidate);
		return;
	}

	if (should_be_active && !obs_source_add_active_child(layout->context, candidate)) {
		blog(LOG_WARNING, "[obs-box-layouts] rejected recursive child '%s'", name);
		obs_source_release(candidate);
		return;
	}

	*slot = candidate;
	*active = should_be_active;
}

static void box_key(char *buffer, size_t size, int index, const char *suffix)
{
	snprintf(buffer, size, "box_%d_%s", index + 1, suffix);
}

static void replace_name(char **destination, const char *name)
{
	bfree(*destination);
	*destination = (name && *name) ? bstrdup(name) : NULL;
}

static void box_layout_update(void *data, obs_data_t *settings)
{
	struct box_layout *layout = data;
	char key[64];

	pthread_mutex_lock(&layout->mutex);
	layout->width = (uint32_t)obs_data_get_int(settings, "width");
	layout->height = (uint32_t)obs_data_get_int(settings, "height");
	layout->preset = (int)obs_data_get_int(settings, "preset");
	layout->custom_box_count = (int)obs_data_get_int(settings, "custom_box_count");
	layout->custom_box_count = layout->custom_box_count < 1 ? 1 : layout->custom_box_count;
	layout->custom_box_count = layout->custom_box_count > MAX_BOXES ? MAX_BOXES : layout->custom_box_count;
	layout->gap = (float)obs_data_get_double(settings, "gap");
	layout->background_color = (uint32_t)obs_data_get_int(settings, "background_color");
	const char *background_name = obs_data_get_string(settings, "background_source");
	replace_name(&layout->background_name, background_name);
	replace_child(layout, &layout->background, &layout->background_active, background_name, true);

	const int active_box_count = layout_box_count(layout);
	for (int i = 0; i < MAX_BOXES; i++) {
		box_key(key, sizeof(key), i, "source");
		const char *source_name = obs_data_get_string(settings, key);
		replace_name(&layout->boxes[i].source_name, source_name);
		replace_child(layout, &layout->boxes[i].source, &layout->boxes[i].active, source_name,
			      i < active_box_count);
		box_key(key, sizeof(key), i, "zoom");
		layout->boxes[i].zoom = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "pan_x");
		layout->boxes[i].pan_x = (float)obs_data_get_double(settings, key) / 100.0f;
		box_key(key, sizeof(key), i, "pan_y");
		layout->boxes[i].pan_y = (float)obs_data_get_double(settings, key) / 100.0f;
		box_key(key, sizeof(key), i, "fit_mode");
		layout->boxes[i].fit_mode = (int)obs_data_get_int(settings, key);
		box_key(key, sizeof(key), i, "radius");
		layout->boxes[i].radius = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "border_width");
		layout->boxes[i].border_width = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "border_color");
		layout->boxes[i].border_color = (uint32_t)obs_data_get_int(settings, key);
		box_key(key, sizeof(key), i, "custom_x");
		layout->boxes[i].custom_x = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "custom_y");
		layout->boxes[i].custom_y = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "custom_width");
		layout->boxes[i].custom_width = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "custom_height");
		layout->boxes[i].custom_height = (float)obs_data_get_double(settings, key);
		box_key(key, sizeof(key), i, "z_index");
		layout->boxes[i].z_index = (int)obs_data_get_int(settings, key);
		box_key(key, sizeof(key), i, "lock_aspect");
		layout->boxes[i].lock_aspect = obs_data_get_bool(settings, key);
	}
	pthread_mutex_unlock(&layout->mutex);
}

static void box_layout_save(void *data, obs_data_t *settings)
{
	struct box_layout *layout = data;
	char key[64];
	pthread_mutex_lock(&layout->mutex);
	const char *background_name = layout->background ? obs_source_get_name(layout->background)
							 : layout->background_name;
	obs_data_set_string(settings, "background_source", background_name ? background_name : "");
	for (int i = 0; i < MAX_BOXES; i++) {
		box_key(key, sizeof(key), i, "source");
		const char *source_name = layout->boxes[i].source ? obs_source_get_name(layout->boxes[i].source)
								  : layout->boxes[i].source_name;
		obs_data_set_string(settings, key, source_name ? source_name : "");
	}
	pthread_mutex_unlock(&layout->mutex);
}

static void box_layout_tick(void *data, float seconds)
{
	struct box_layout *layout = data;
	UNUSED_PARAMETER(seconds);
	pthread_mutex_lock(&layout->mutex);
	if ((!layout->background || obs_source_removed(layout->background)) && layout->background_name)
		replace_child(layout, &layout->background, &layout->background_active, layout->background_name, true);

	const int active_box_count = layout_box_count(layout);
	for (int i = 0; i < MAX_BOXES; i++) {
		struct layout_box *box = &layout->boxes[i];
		if ((!box->source || obs_source_removed(box->source)) && box->source_name)
			replace_child(layout, &box->source, &box->active, box->source_name, i < active_box_count);
	}
	pthread_mutex_unlock(&layout->mutex);
}

static void box_layout_defaults(obs_data_t *settings)
{
	char key[64];
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "preset", PRESET_TWO_COLUMNS);
	obs_data_set_default_int(settings, "custom_box_count", 2);
	obs_data_set_default_double(settings, "gap", 24.0);
	obs_data_set_default_int(settings, "background_color", 0xFF171717);

	for (int i = 0; i < MAX_BOXES; i++) {
		box_key(key, sizeof(key), i, "zoom");
		obs_data_set_default_double(settings, key, 1.0);
		box_key(key, sizeof(key), i, "pan_x");
		obs_data_set_default_double(settings, key, 0.0);
		box_key(key, sizeof(key), i, "pan_y");
		obs_data_set_default_double(settings, key, 0.0);
		box_key(key, sizeof(key), i, "fit_mode");
		obs_data_set_default_int(settings, key, FIT_FILL);
		box_key(key, sizeof(key), i, "radius");
		obs_data_set_default_double(settings, key, 24.0);
		box_key(key, sizeof(key), i, "border_width");
		obs_data_set_default_double(settings, key, 3.0);
		box_key(key, sizeof(key), i, "border_color");
		obs_data_set_default_int(settings, key, 0xFFFFFFFF);
		box_key(key, sizeof(key), i, "custom_x");
		obs_data_set_default_double(settings, key, 100.0 + i * 60.0);
		box_key(key, sizeof(key), i, "custom_y");
		obs_data_set_default_double(settings, key, 100.0 + i * 60.0);
		box_key(key, sizeof(key), i, "custom_width");
		obs_data_set_default_double(settings, key, 640.0);
		box_key(key, sizeof(key), i, "custom_height");
		obs_data_set_default_double(settings, key, 360.0);
		box_key(key, sizeof(key), i, "z_index");
		obs_data_set_default_int(settings, key, i);
		box_key(key, sizeof(key), i, "lock_aspect");
		obs_data_set_default_bool(settings, key, false);
	}
}

static void *box_layout_create(obs_data_t *settings, obs_source_t *source)
{
	struct box_layout *layout = bzalloc(sizeof(*layout));
	layout->context = source;
	if (pthread_mutex_init_recursive(&layout->mutex) != 0) {
		blog(LOG_ERROR, "[obs-box-layouts] could not initialize source mutex");
		bfree(layout);
		return NULL;
	}

	char *effect_path = obs_module_file("box-layout.effect");
	char *error = NULL;
	const uint32_t transparent_pixel = 0;
	const uint8_t *pixel_data[] = {(const uint8_t *)&transparent_pixel};
	obs_enter_graphics();
	layout->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	layout->blank_texture = gs_texture_create(1, 1, GS_RGBA, 1, pixel_data, 0);
	if (effect_path)
		layout->effect = gs_effect_create_from_file(effect_path, &error);
	obs_leave_graphics();
	bfree(effect_path);

	if (layout->effect) {
		layout->image_param = gs_effect_get_param_by_name(layout->effect, "image");
		layout->box_size_param = gs_effect_get_param_by_name(layout->effect, "boxSize");
		layout->uv_scale_param = gs_effect_get_param_by_name(layout->effect, "uvScale");
		layout->uv_offset_param = gs_effect_get_param_by_name(layout->effect, "uvOffset");
		layout->radius_param = gs_effect_get_param_by_name(layout->effect, "radius");
		layout->border_width_param = gs_effect_get_param_by_name(layout->effect, "borderWidth");
		layout->border_color_param = gs_effect_get_param_by_name(layout->effect, "borderColor");
		layout->effect_ready = layout->image_param && layout->box_size_param && layout->uv_scale_param &&
				       layout->uv_offset_param && layout->radius_param && layout->border_width_param &&
				       layout->border_color_param;
	}

	if (!layout->texrender || !layout->blank_texture || !layout->effect_ready) {
		blog(LOG_WARNING, "[obs-box-layouts] custom effect unavailable; using native rounded-box renderer: %s",
		     error ? error : "required graphics resource or effect parameter is missing");
	}
	bfree(error);

	box_layout_update(layout, settings);
	return layout;
}

static void box_layout_destroy(void *data)
{
	struct box_layout *layout = data;
	if (!layout)
		return;

	pthread_mutex_lock(&layout->mutex);
	remove_child(layout, &layout->background, &layout->background_active);
	bfree(layout->background_name);
	for (int i = 0; i < MAX_BOXES; i++) {
		remove_child(layout, &layout->boxes[i].source, &layout->boxes[i].active);
		bfree(layout->boxes[i].source_name);
	}
	pthread_mutex_unlock(&layout->mutex);

	obs_enter_graphics();
	gs_texrender_destroy(layout->texrender);
	gs_texture_destroy(layout->blank_texture);
	gs_effect_destroy(layout->effect);
	obs_leave_graphics();
	pthread_mutex_destroy(&layout->mutex);
	bfree(layout);
}

static gs_texture_t *render_child(struct box_layout *layout, obs_source_t *source)
{
	if (!source || !layout->texrender)
		return NULL;

	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (!width || !height)
		return NULL;

	/* Direct3D keeps the previous shader-resource binding cached after a
	 * texrender texture becomes a render target. Explicitly unbind it before
	 * beginning the next child render so the texture is rebound for drawing. */
	gs_load_texture(NULL, 0);
	gs_texrender_reset(layout->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(layout->texrender, width, height)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(layout->texrender);
	}
	gs_blend_state_pop();
	return gs_texrender_get_texture(layout->texrender);
}

static void calculate_cover_uv(uint32_t source_width, uint32_t source_height, float box_width, float box_height,
			       float zoom, float pan_x, float pan_y, struct vec2 *scale, struct vec2 *offset)
{
	const float source_aspect = (float)source_width / (float)source_height;
	const float box_aspect = box_width / box_height;
	scale->x = 1.0f;
	scale->y = 1.0f;

	if (source_aspect > box_aspect)
		scale->x = box_aspect / source_aspect;
	else
		scale->y = source_aspect / box_aspect;

	zoom = fmaxf(zoom, 1.0f);
	scale->x = fminf(scale->x / zoom, 1.0f);
	scale->y = fminf(scale->y / zoom, 1.0f);
	offset->x = (1.0f - scale->x) * (0.5f + 0.5f * fmaxf(-1.0f, fminf(1.0f, pan_x)));
	offset->y = (1.0f - scale->y) * (0.5f + 0.5f * fmaxf(-1.0f, fminf(1.0f, pan_y)));
}

static size_t rounded_perimeter(const struct box_rect *rect, float radius, struct vec2 points[ROUNDED_PERIMETER_POINTS])
{
	const float max_radius = fminf(rect->width, rect->height) * 0.5f;
	radius = fminf(fmaxf(radius, 0.0f), max_radius);

	const float centers_x[] = {rect->x + radius, rect->x + rect->width - radius, rect->x + rect->width - radius,
				   rect->x + radius};
	const float centers_y[] = {rect->y + radius, rect->y + radius, rect->y + rect->height - radius,
				   rect->y + rect->height - radius};
	const float start_angles[] = {PI_F, -PI_F * 0.5f, 0.0f, PI_F * 0.5f};
	size_t point = 0;
	for (size_t corner = 0; corner < 4; corner++) {
		for (size_t segment = 0; segment <= ROUNDED_CORNER_SEGMENTS; segment++) {
			const float angle =
				start_angles[corner] + (PI_F * 0.5f * (float)segment / (float)ROUNDED_CORNER_SEGMENTS);
			vec2_set(&points[point++], centers_x[corner] + cosf(angle) * radius,
				 centers_y[corner] + sinf(angle) * radius);
		}
	}
	return point;
}

static void textured_vertex(const struct vec2 *point, const struct box_rect *rect, const struct vec2 *uv_scale,
			    const struct vec2 *uv_offset)
{
	const float u = uv_offset->x + ((point->x - rect->x) / rect->width) * uv_scale->x;
	const float v = uv_offset->y + ((point->y - rect->y) / rect->height) * uv_scale->y;
	gs_texcoord(u, v, 0);
	gs_vertex2f(point->x, point->y);
}

static void draw_rounded_texture(gs_texture_t *texture, const struct box_rect *rect, float radius,
				 const struct vec2 *uv_scale, const struct vec2 *uv_offset)
{
	struct vec2 perimeter[ROUNDED_PERIMETER_POINTS];
	const size_t count = rounded_perimeter(rect, radius, perimeter);
	struct vec2 center;
	vec2_set(&center, rect->x + rect->width * 0.5f, rect->y + rect->height * 0.5f);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);
	while (gs_effect_loop(effect, "Draw")) {
		gs_render_start(false);
		for (size_t i = 0; i < count; i++) {
			const size_t next = (i + 1) % count;
			textured_vertex(&center, rect, uv_scale, uv_offset);
			textured_vertex(&perimeter[i], rect, uv_scale, uv_offset);
			textured_vertex(&perimeter[next], rect, uv_scale, uv_offset);
		}
		gs_render_stop(GS_TRIS);
	}
}

static void solid_vertex(const struct vec2 *point)
{
	gs_vertex2f(point->x, point->y);
}

static void draw_rounded_border(const struct box_rect *rect, float radius, float border_width, uint32_t border_color)
{
	const float max_border = fminf(rect->width, rect->height) * 0.5f;
	border_width = fminf(fmaxf(border_width, 0.0f), max_border);
	if (border_width < 0.5f)
		return;

	struct vec2 outer[ROUNDED_PERIMETER_POINTS];
	struct vec2 inner[ROUNDED_PERIMETER_POINTS];
	const size_t outer_count = rounded_perimeter(rect, radius, outer);
	struct box_rect inner_rect = {
		.x = rect->x + border_width,
		.y = rect->y + border_width,
		.width = rect->width - border_width * 2.0f,
		.height = rect->height - border_width * 2.0f,
	};

	struct vec4 color;
	vec4_from_rgba(&color, border_color);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(effect, "color");
	gs_effect_set_vec4(color_param, &color);

	if (inner_rect.width < 1.0f || inner_rect.height < 1.0f) {
		struct vec2 center;
		vec2_set(&center, rect->x + rect->width * 0.5f, rect->y + rect->height * 0.5f);
		while (gs_effect_loop(effect, "Solid")) {
			gs_render_start(false);
			for (size_t i = 0; i < outer_count; i++) {
				const size_t next = (i + 1) % outer_count;
				solid_vertex(&center);
				solid_vertex(&outer[i]);
				solid_vertex(&outer[next]);
			}
			gs_render_stop(GS_TRIS);
		}
		return;
	}

	rounded_perimeter(&inner_rect, fmaxf(radius - border_width, 0.0f), inner);

	while (gs_effect_loop(effect, "Solid")) {
		gs_render_start(false);
		for (size_t i = 0; i < ROUNDED_PERIMETER_POINTS; i++) {
			const size_t next = (i + 1) % ROUNDED_PERIMETER_POINTS;
			solid_vertex(&outer[i]);
			solid_vertex(&inner[i]);
			solid_vertex(&inner[next]);
			solid_vertex(&outer[i]);
			solid_vertex(&inner[next]);
			solid_vertex(&outer[next]);
		}
		gs_render_stop(GS_TRIS);
	}
}

static bool intersect_rects(const struct box_rect *first, const struct box_rect *second, struct box_rect *result)
{
	const float left = fmaxf(first->x, second->x);
	const float top = fmaxf(first->y, second->y);
	const float right = fminf(first->x + first->width, second->x + second->width);
	const float bottom = fminf(first->y + first->height, second->y + second->height);
	if (right <= left || bottom <= top)
		return false;
	set_rect(result, left, top, right - left, bottom - top);
	return true;
}

static void calculate_contained_rect(uint32_t source_width, uint32_t source_height, const struct box_rect *box,
				     float zoom, float pan_x, float pan_y, struct box_rect *destination)
{
	const float scale = fminf(box->width / (float)source_width, box->height / (float)source_height);
	zoom = fminf(fmaxf(zoom, 0.1f), 4.0f);
	destination->width = (float)source_width * scale * zoom;
	destination->height = (float)source_height * scale * zoom;
	const float travel_x = fabsf(box->width - destination->width) * 0.5f;
	const float travel_y = fabsf(box->height - destination->height) * 0.5f;
	pan_x = fmaxf(-1.0f, fminf(1.0f, pan_x));
	pan_y = fmaxf(-1.0f, fminf(1.0f, pan_y));
	destination->x = box->x + (box->width - destination->width) * 0.5f - pan_x * travel_x;
	destination->y = box->y + (box->height - destination->height) * 0.5f - pan_y * travel_y;
}

static void draw_native_masked_texture(gs_texture_t *texture, uint32_t source_width, uint32_t source_height,
				       const struct box_rect *rect, int fit_mode, float zoom, float pan_x, float pan_y,
				       float radius, float border_width, uint32_t border_color)
{
	if (texture && source_width && source_height) {
		struct vec2 uv_scale;
		struct vec2 uv_offset;
		if (fit_mode == FIT_FILL) {
			calculate_cover_uv(source_width, source_height, rect->width, rect->height, zoom, pan_x, pan_y,
					   &uv_scale, &uv_offset);
			draw_rounded_texture(texture, rect, radius, &uv_scale, &uv_offset);
		} else if (fit_mode == FIT_STRETCH) {
			vec2_set(&uv_scale, 1.0f, 1.0f);
			vec2_zero(&uv_offset);
			draw_rounded_texture(texture, rect, radius, &uv_scale, &uv_offset);
		} else {
			struct box_rect destination;
			struct box_rect visible;
			const bool manual = fit_mode == FIT_MANUAL;
			calculate_contained_rect(source_width, source_height, rect, manual ? zoom : 1.0f,
						 manual ? pan_x : 0.0f, manual ? pan_y : 0.0f, &destination);
			if (intersect_rects(&destination, rect, &visible)) {
				vec2_set(&uv_scale, visible.width / destination.width,
					 visible.height / destination.height);
				vec2_set(&uv_offset, (visible.x - destination.x) / destination.width,
					 (visible.y - destination.y) / destination.height);
				const bool fills_box = fabsf(visible.x - rect->x) < 0.5f &&
						       fabsf(visible.y - rect->y) < 0.5f &&
						       fabsf(visible.width - rect->width) < 0.5f &&
						       fabsf(visible.height - rect->height) < 0.5f;
				draw_rounded_texture(texture, &visible, fills_box ? radius : 0.0f, &uv_scale,
						     &uv_offset);
			}
		}
	}
	draw_rounded_border(rect, radius, border_width, border_color);
	gs_load_texture(NULL, 0);
}

static void draw_masked_texture(struct box_layout *layout, gs_texture_t *texture, uint32_t source_width,
				uint32_t source_height, const struct box_rect *rect, int fit_mode, float zoom,
				float pan_x, float pan_y, float radius, float border_width, uint32_t border_color)
{
	if (!layout->effect_ready || fit_mode != FIT_FILL) {
		if (!layout->graphics_warning_logged) {
			blog(LOG_WARNING, "[obs-box-layouts] using native rounded-box renderer");
			layout->graphics_warning_logged = true;
		}
		draw_native_masked_texture(texture, source_width, source_height, rect, fit_mode, zoom, pan_x, pan_y,
					   radius, border_width, border_color);
		return;
	}

	struct vec2 box_size;
	struct vec2 uv_scale;
	struct vec2 uv_offset;
	struct vec4 border;
	vec2_set(&box_size, rect->width, rect->height);
	vec2_set(&uv_scale, 1.0f, 1.0f);
	vec2_zero(&uv_offset);
	vec4_from_rgba(&border, border_color);

	if (source_width && source_height)
		calculate_cover_uv(source_width, source_height, rect->width, rect->height, zoom, pan_x, pan_y,
				   &uv_scale, &uv_offset);

	const float max_radius = fminf(rect->width, rect->height) * 0.5f;
	gs_effect_set_texture(layout->image_param, texture ? texture : layout->blank_texture);
	gs_effect_set_vec2(layout->box_size_param, &box_size);
	gs_effect_set_vec2(layout->uv_scale_param, &uv_scale);
	gs_effect_set_vec2(layout->uv_offset_param, &uv_offset);
	gs_effect_set_float(layout->radius_param, fminf(fmaxf(radius, 0.0f), max_radius));
	gs_effect_set_float(layout->border_width_param, fmaxf(border_width, 0.0f));
	gs_effect_set_vec4(layout->border_color_param, &border);

	gs_matrix_push();
	gs_matrix_translate3f(rect->x, rect->y, 0.0f);
	while (gs_effect_loop(layout->effect, "Draw"))
		gs_draw_sprite(NULL, 0, (uint32_t)rect->width, (uint32_t)rect->height);
	gs_matrix_pop();
	gs_load_texture(NULL, 0);
}

static void draw_background_color(uint32_t width, uint32_t height, uint32_t background_color)
{
	struct vec4 color;
	vec4_from_rgba(&color, background_color);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(effect, "color");
	gs_effect_set_vec4(color_param, &color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(NULL, 0, width, height);
}

static void calculate_render_order(const struct box_layout *layout, int count, int order[MAX_BOXES])
{
	for (int i = 0; i < count; i++)
		order[i] = i;
	if (layout->preset != PRESET_CUSTOM)
		return;

	for (int i = 1; i < count; i++) {
		const int value = order[i];
		int position = i;
		while (position > 0 && layout->boxes[order[position - 1]].z_index > layout->boxes[value].z_index) {
			order[position] = order[position - 1];
			position--;
		}
		order[position] = value;
	}
}

static void box_layout_render(void *data, gs_effect_t *unused)
{
	struct box_layout *layout = data;
	struct box_rect rects[MAX_BOXES] = {0};
	int order[MAX_BOXES] = {0};
	struct box_render_state boxes[MAX_BOXES] = {0};
	obs_source_t *background = NULL;
	uint32_t width;
	uint32_t height;
	uint32_t background_color;
	int count;
	UNUSED_PARAMETER(unused);
	if (!layout)
		return;

	pthread_mutex_lock(&layout->mutex);
	width = layout->width;
	height = layout->height;
	background_color = layout->background_color;
	if (layout->background)
		background = obs_source_get_ref(layout->background);
	count = calculate_rects(layout, rects);
	calculate_render_order(layout, count, order);
	for (int i = 0; i < count; i++) {
		struct layout_box *box = &layout->boxes[i];
		if (box->source)
			boxes[i].source = obs_source_get_ref(box->source);
		boxes[i].zoom = box->zoom;
		boxes[i].pan_x = box->pan_x;
		boxes[i].pan_y = box->pan_y;
		boxes[i].fit_mode = box->fit_mode;
		boxes[i].radius = box->radius;
		boxes[i].border_width = box->border_width;
		boxes[i].border_color = box->border_color;
	}
	pthread_mutex_unlock(&layout->mutex);

	draw_background_color(width, height, background_color);

	if (background) {
		gs_texture_t *texture = render_child(layout, background);
		if (texture) {
			struct box_rect canvas = {0.0f, 0.0f, (float)width, (float)height};
			draw_masked_texture(layout, texture, obs_source_get_width(background),
					    obs_source_get_height(background), &canvas, FIT_FILL, 1.0f, 0.0f, 0.0f,
					    0.0f, 0.0f, 0);
		}
		obs_source_release(background);
	}

	for (int position = 0; position < count; position++) {
		const int i = order[position];
		struct box_render_state *box = &boxes[i];
		gs_texture_t *texture = render_child(layout, box->source);
		const uint32_t source_width = box->source ? obs_source_get_width(box->source) : 0;
		const uint32_t source_height = box->source ? obs_source_get_height(box->source) : 0;
		draw_masked_texture(layout, texture, source_width, source_height, &rects[i], box->fit_mode, box->zoom,
				    box->pan_x, box->pan_y, box->radius, box->border_width, box->border_color);
		if (box->source)
			obs_source_release(box->source);
	}
}

static uint32_t box_layout_width(void *data)
{
	const struct box_layout *layout = data;
	return layout ? layout->width : 0;
}

static uint32_t box_layout_height(void *data)
{
	const struct box_layout *layout = data;
	return layout ? layout->height : 0;
}

static void persist_box_transform(struct box_layout *layout, int index, bool geometry, bool content)
{
	if (index < 0 || index >= MAX_BOXES)
		return;

	struct layout_box snapshot;
	pthread_mutex_lock(&layout->mutex);
	snapshot = layout->boxes[index];
	pthread_mutex_unlock(&layout->mutex);

	obs_data_t *settings = obs_source_get_settings(layout->context);
	char key[64];
	if (geometry) {
		box_key(key, sizeof(key), index, "custom_x");
		obs_data_set_double(settings, key, snapshot.custom_x);
		box_key(key, sizeof(key), index, "custom_y");
		obs_data_set_double(settings, key, snapshot.custom_y);
		box_key(key, sizeof(key), index, "custom_width");
		obs_data_set_double(settings, key, snapshot.custom_width);
		box_key(key, sizeof(key), index, "custom_height");
		obs_data_set_double(settings, key, snapshot.custom_height);
	}
	if (content) {
		box_key(key, sizeof(key), index, "zoom");
		obs_data_set_double(settings, key, snapshot.zoom);
		box_key(key, sizeof(key), index, "pan_x");
		obs_data_set_double(settings, key, snapshot.pan_x * 100.0f);
		box_key(key, sizeof(key), index, "pan_y");
		obs_data_set_double(settings, key, snapshot.pan_y * 100.0f);
	}
	obs_source_update(layout->context, settings);
	obs_data_release(settings);
}

static void persist_custom_layout(struct box_layout *layout)
{
	struct box_rect rects[MAX_BOXES] = {0};
	int count;
	pthread_mutex_lock(&layout->mutex);
	count = layout_box_count(layout);
	for (int i = 0; i < count; i++) {
		rects[i].x = layout->boxes[i].custom_x;
		rects[i].y = layout->boxes[i].custom_y;
		rects[i].width = layout->boxes[i].custom_width;
		rects[i].height = layout->boxes[i].custom_height;
	}
	pthread_mutex_unlock(&layout->mutex);

	obs_data_t *settings = obs_source_get_settings(layout->context);
	obs_data_set_int(settings, "preset", PRESET_CUSTOM);
	obs_data_set_int(settings, "custom_box_count", count);
	char key[64];
	for (int i = 0; i < count; i++) {
		box_key(key, sizeof(key), i, "custom_x");
		obs_data_set_double(settings, key, rects[i].x);
		box_key(key, sizeof(key), i, "custom_y");
		obs_data_set_double(settings, key, rects[i].y);
		box_key(key, sizeof(key), i, "custom_width");
		obs_data_set_double(settings, key, rects[i].width);
		box_key(key, sizeof(key), i, "custom_height");
		obs_data_set_double(settings, key, rects[i].height);
	}
	obs_source_update(layout->context, settings);
	obs_data_release(settings);
}

static void convert_current_preset_to_custom(struct box_layout *layout)
{
	if (layout->preset == PRESET_CUSTOM)
		return;
	struct box_rect rects[MAX_BOXES] = {0};
	const int count = calculate_rects(layout, rects);
	for (int i = 0; i < count; i++) {
		layout->boxes[i].custom_x = rects[i].x;
		layout->boxes[i].custom_y = rects[i].y;
		layout->boxes[i].custom_width = rects[i].width;
		layout->boxes[i].custom_height = rects[i].height;
	}
	layout->custom_box_count = count;
	layout->preset = PRESET_CUSTOM;
}

static int hit_test_box(struct box_layout *layout, int32_t x, int32_t y, uint32_t *edges, struct box_rect *hit_rect)
{
	struct box_rect rects[MAX_BOXES] = {0};
	int order[MAX_BOXES] = {0};
	const int count = calculate_rects(layout, rects);
	calculate_render_order(layout, count, order);
	for (int position = count - 1; position >= 0; position--) {
		const int index = order[position];
		const struct box_rect *rect = &rects[index];
		const float right = rect->x + rect->width;
		const float bottom = rect->y + rect->height;
		if ((float)x < rect->x || (float)x > right || (float)y < rect->y || (float)y > bottom)
			continue;

		const float threshold = fminf(18.0f, fminf(rect->width, rect->height) / 3.0f);
		uint32_t result = RESIZE_NONE;
		if (fabsf((float)x - rect->x) <= threshold)
			result |= RESIZE_LEFT;
		else if (fabsf((float)x - right) <= threshold)
			result |= RESIZE_RIGHT;
		if (fabsf((float)y - rect->y) <= threshold)
			result |= RESIZE_TOP;
		else if (fabsf((float)y - bottom) <= threshold)
			result |= RESIZE_BOTTOM;
		*edges = result;
		*hit_rect = *rect;
		return index;
	}
	return -1;
}

static void box_layout_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
				   uint32_t click_count)
{
	struct box_layout *layout = data;
	if (!layout || !event || type != MOUSE_LEFT)
		return;

	if (mouse_up) {
		pthread_mutex_lock(&layout->mutex);
		const bool should_persist = layout->dragging;
		const int index = layout->drag_box;
		const bool content = layout->drag_content;
		const bool converted = layout->drag_converted;
		layout->dragging = false;
		pthread_mutex_unlock(&layout->mutex);
		if (should_persist) {
			blog(LOG_INFO, "[obs-box-layouts] editor mouse-up box=%d content=%s converted=%s", index + 1,
			     content ? "yes" : "no", converted ? "yes" : "no");
			if (converted)
				persist_custom_layout(layout);
			else
				persist_box_transform(layout, index, !content, content);
		}
		return;
	}

	pthread_mutex_lock(&layout->mutex);
	uint32_t edges = RESIZE_NONE;
	struct box_rect rect;
	const int index = hit_test_box(layout, event->x, event->y, &edges, &rect);
	if (index < 0) {
		blog(LOG_INFO, "[obs-box-layouts] editor mouse-down outside boxes x=%d y=%d canvas=%ux%u", event->x,
		     event->y, layout->width, layout->height);
		layout->dragging = false;
		pthread_mutex_unlock(&layout->mutex);
		return;
	}

	if (click_count >= 2) {
		blog(LOG_INFO, "[obs-box-layouts] editor double-click reset box=%d", index + 1);
		layout->boxes[index].zoom = 1.0f;
		layout->boxes[index].pan_x = 0.0f;
		layout->boxes[index].pan_y = 0.0f;
		layout->dragging = false;
		pthread_mutex_unlock(&layout->mutex);
		persist_box_transform(layout, index, false, true);
		return;
	}

	const bool content_drag = (event->modifiers & (INTERACT_COMMAND_KEY | INTERACT_CONTROL_KEY)) != 0;
	const bool converted = !content_drag && layout->preset != PRESET_CUSTOM;
	if (converted)
		convert_current_preset_to_custom(layout);
	if (event->modifiers & INTERACT_SHIFT_KEY)
		edges = RESIZE_RIGHT | RESIZE_BOTTOM;

	layout->dragging = true;
	layout->drag_content = content_drag;
	layout->drag_converted = converted;
	layout->drag_move_logged = false;
	layout->drag_box = index;
	layout->drag_edges = edges;
	layout->drag_start_x = event->x;
	layout->drag_start_y = event->y;
	layout->drag_start_rect = rect;
	layout->drag_start_pan_x = layout->boxes[index].pan_x;
	layout->drag_start_pan_y = layout->boxes[index].pan_y;
	blog(LOG_INFO, "[obs-box-layouts] editor mouse-down box=%d x=%d y=%d modifiers=%d mode=%s edges=%u", index + 1,
	     event->x, event->y, event->modifiers,
	     content_drag ? "pan-content" : (edges == RESIZE_NONE ? "move-box" : "resize-box"), edges);
	pthread_mutex_unlock(&layout->mutex);
}

static void box_layout_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct box_layout *layout = data;
	if (!layout || !event || mouse_leave)
		return;

	pthread_mutex_lock(&layout->mutex);
	if (!layout->dragging || (!layout->drag_content && layout->preset != PRESET_CUSTOM)) {
		pthread_mutex_unlock(&layout->mutex);
		return;
	}

	const int index = layout->drag_box;
	struct layout_box *box = &layout->boxes[index];
	const struct box_rect start = layout->drag_start_rect;
	const float dx = (float)(event->x - layout->drag_start_x);
	const float dy = (float)(event->y - layout->drag_start_y);
	const float canvas_width = (float)layout->width;
	const float canvas_height = (float)layout->height;
	const float minimum_size = 40.0f;
	if (!layout->drag_move_logged && (fabsf(dx) >= 1.0f || fabsf(dy) >= 1.0f)) {
		blog(LOG_INFO, "[obs-box-layouts] editor drag received box=%d dx=%.1f dy=%.1f", index + 1, dx, dy);
		layout->drag_move_logged = true;
	}
	if (layout->drag_content) {
		box->pan_x = fmaxf(-1.0f, fminf(1.0f, layout->drag_start_pan_x - 2.0f * dx / start.width));
		box->pan_y = fmaxf(-1.0f, fminf(1.0f, layout->drag_start_pan_y - 2.0f * dy / start.height));
		pthread_mutex_unlock(&layout->mutex);
		return;
	}

	if (layout->drag_edges == RESIZE_NONE) {
		box->custom_x = fmaxf(0.0f, fminf(start.x + dx, canvas_width - start.width));
		box->custom_y = fmaxf(0.0f, fminf(start.y + dy, canvas_height - start.height));
		pthread_mutex_unlock(&layout->mutex);
		return;
	}

	float left = start.x;
	float right = start.x + start.width;
	float top = start.y;
	float bottom = start.y + start.height;
	if (layout->drag_edges & RESIZE_LEFT)
		left = fmaxf(0.0f, fminf(start.x + dx, right - minimum_size));
	if (layout->drag_edges & RESIZE_RIGHT)
		right = fminf(canvas_width, fmaxf(start.x + start.width + dx, left + minimum_size));
	if (layout->drag_edges & RESIZE_TOP)
		top = fmaxf(0.0f, fminf(start.y + dy, bottom - minimum_size));
	if (layout->drag_edges & RESIZE_BOTTOM)
		bottom = fminf(canvas_height, fmaxf(start.y + start.height + dy, top + minimum_size));

	const bool horizontal = (layout->drag_edges & (RESIZE_LEFT | RESIZE_RIGHT)) != 0;
	const bool vertical = (layout->drag_edges & (RESIZE_TOP | RESIZE_BOTTOM)) != 0;
	if (box->lock_aspect && horizontal && vertical && start.height > 0.0f) {
		const float aspect = start.width / start.height;
		float new_width = right - left;
		float new_height = bottom - top;
		if (fabsf(new_width - start.width) > fabsf(new_height - start.height) * aspect)
			new_height = new_width / aspect;
		else
			new_width = new_height * aspect;

		if (layout->drag_edges & RESIZE_LEFT)
			left = right - new_width;
		else
			right = left + new_width;
		if (layout->drag_edges & RESIZE_TOP)
			top = bottom - new_height;
		else
			bottom = top + new_height;
		left = fmaxf(0.0f, left);
		top = fmaxf(0.0f, top);
		right = fminf(canvas_width, right);
		bottom = fminf(canvas_height, bottom);
	}

	box->custom_x = left;
	box->custom_y = top;
	box->custom_width = fmaxf(minimum_size, right - left);
	box->custom_height = fmaxf(minimum_size, bottom - top);
	pthread_mutex_unlock(&layout->mutex);
}

static void box_layout_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	struct box_layout *layout = data;
	UNUSED_PARAMETER(x_delta);
	if (!layout || !event || !y_delta)
		return;

	pthread_mutex_lock(&layout->mutex);
	uint32_t edges;
	struct box_rect rect;
	const int index = hit_test_box(layout, event->x, event->y, &edges, &rect);
	if (index >= 0) {
		const float minimum_zoom = layout->boxes[index].fit_mode == FIT_MANUAL ? 0.1f : 1.0f;
		layout->boxes[index].zoom =
			fmaxf(minimum_zoom, fminf(4.0f, layout->boxes[index].zoom + (y_delta > 0 ? 0.1f : -0.1f)));
	}
	pthread_mutex_unlock(&layout->mutex);
	if (index >= 0) {
		blog(LOG_INFO, "[obs-box-layouts] editor wheel box=%d delta=%d", index + 1, y_delta);
		persist_box_transform(layout, index, false, true);
	}
}

static void box_layout_focus(void *data, bool focus)
{
	UNUSED_PARAMETER(data);
	blog(LOG_INFO, "[obs-box-layouts] visual editor focus=%s", focus ? "yes" : "no");
}

static bool child_already_added(obs_source_t *children[MAX_CHILDREN], size_t count, obs_source_t *source)
{
	for (size_t i = 0; i < count; i++)
		if (children[i] == source)
			return true;
	return false;
}

static size_t collect_child_refs(struct box_layout *layout, obs_source_t *children[MAX_CHILDREN])
{
	size_t count = 0;
	pthread_mutex_lock(&layout->mutex);
	if (layout->background) {
		obs_source_t *source = obs_source_get_ref(layout->background);
		if (source)
			children[count++] = source;
	}

	const int box_count = layout_box_count(layout);
	for (int i = 0; i < box_count; i++) {
		obs_source_t *source = layout->boxes[i].source;
		if (source && !child_already_added(children, count, source)) {
			source = obs_source_get_ref(source);
			if (source)
				children[count++] = source;
		}
	}
	pthread_mutex_unlock(&layout->mutex);
	return count;
}

static void release_child_refs(obs_source_t *children[MAX_CHILDREN], size_t count)
{
	for (size_t i = 0; i < count; i++)
		obs_source_release(children[i]);
}

static void box_layout_enum_sources(void *data, obs_source_enum_proc_t callback, void *param)
{
	struct box_layout *layout = data;
	obs_source_t *children[MAX_CHILDREN];
	if (!layout || !callback)
		return;
	const size_t count = collect_child_refs(layout, children);
	for (size_t i = 0; i < count; i++)
		callback(layout->context, children[i], param);
	release_child_refs(children, count);
}

static bool box_layout_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
				    uint32_t mixers, size_t channels, size_t sample_rate)
{
	struct box_layout *layout = data;
	obs_source_t *children[MAX_CHILDREN];
	uint64_t min_ts = 0;
	if (!layout)
		return false;

	const size_t count = collect_child_refs(layout, children);
	for (size_t i = 0; i < count; i++) {
		obs_source_t *child = children[i];
		if (!(obs_source_get_output_flags(child) & OBS_SOURCE_AUDIO) || obs_source_audio_pending(child))
			continue;
		const uint64_t ts = obs_source_get_audio_timestamp(child);
		if (ts && (!min_ts || ts < min_ts))
			min_ts = ts;
	}

	if (!min_ts) {
		release_child_refs(children, count);
		return false;
	}

	for (size_t i = 0; i < count; i++) {
		obs_source_t *child = children[i];
		if (!(obs_source_get_output_flags(child) & OBS_SOURCE_AUDIO) || obs_source_audio_pending(child))
			continue;
		const uint64_t ts = obs_source_get_audio_timestamp(child);
		if (!ts || ts < min_ts)
			continue;
		const size_t offset = (size_t)ns_to_audio_frames(sample_rate, ts - min_ts);
		if (offset >= AUDIO_OUTPUT_FRAMES)
			continue;

		struct obs_source_audio_mix child_audio;
		obs_source_get_audio_mix(child, &child_audio);
		for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
			if (!(mixers & (1U << mix)))
				continue;
			for (size_t channel = 0; channel < channels; channel++) {
				float *out = audio_output->output[mix].data[channel];
				float *in = child_audio.output[mix].data[channel];
				if (!out || !in)
					continue;
				for (size_t frame = 0; frame < AUDIO_OUTPUT_FRAMES - offset; frame++)
					out[offset + frame] += in[frame];
			}
		}
	}
	release_child_refs(children, count);
	*ts_out = min_ts;
	return true;
}

struct source_list_context {
	obs_property_t *property;
	obs_source_t *self;
};

static bool add_source_to_list(void *data, obs_source_t *source)
{
	struct source_list_context *context = data;
	if (source == context->self || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
		return true;
	const char *name = obs_source_get_name(source);
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
		char display_name[512];
		snprintf(display_name, sizeof(display_name), "%s %s", obs_module_text("Source.ScenePrefix"), name);
		obs_property_list_add_string(context->property, display_name, name);
	} else {
		obs_property_list_add_string(context->property, name, name);
	}
	return true;
}

static void populate_source_list(obs_property_t *property, obs_source_t *self)
{
	struct source_list_context context = {property, self};
	obs_property_list_add_string(property, obs_module_text("None"), "");
	obs_enum_scenes(add_source_to_list, &context);
	obs_enum_sources(add_source_to_list, &context);
}

static bool preset_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const int preset = (int)obs_data_get_int(settings, "preset");
	const bool custom = preset == PRESET_CUSTOM;
	int visible_count = custom ? (int)obs_data_get_int(settings, "custom_box_count") : preset_box_count(preset);
	visible_count = visible_count < 1 ? 1 : visible_count;
	visible_count = visible_count > MAX_BOXES ? MAX_BOXES : visible_count;
	obs_property_t *custom_count = obs_properties_get(properties, "custom_box_count");
	if (custom_count)
		obs_property_set_visible(custom_count, custom);
	obs_property_t *gap = obs_properties_get(properties, "gap");
	if (gap)
		obs_property_set_visible(gap, !custom);

	static const char *geometry_suffixes[] = {"custom_x", "custom_y",    "custom_width", "custom_height",
						  "z_index",  "lock_aspect", "custom_help"};
	char key[64];
	for (int i = 0; i < MAX_BOXES; i++) {
		box_key(key, sizeof(key), i, "group");
		obs_property_t *group = obs_properties_get(properties, key);
		if (group)
			obs_property_set_visible(group, i < visible_count);
		for (size_t suffix = 0; suffix < OBS_COUNTOF(geometry_suffixes); suffix++) {
			box_key(key, sizeof(key), i, geometry_suffixes[suffix]);
			obs_property_t *geometry = obs_properties_get(properties, key);
			if (geometry)
				obs_property_set_visible(geometry, custom);
		}
	}
	return true;
}

static bool open_visual_editor(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	struct box_layout *layout = data;
	if (!layout || !layout->context)
		return false;
	blog(LOG_INFO, "[obs-box-layouts] opening visual editor for '%s'", obs_source_get_name(layout->context));
	obs_frontend_open_source_interaction(layout->context);
	return false;
}

static obs_properties_t *box_layout_properties(void *data)
{
	struct box_layout *layout = data;
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, "visual_editor_help", obs_module_text("Editor.Help"), OBS_TEXT_INFO);
	obs_properties_add_button2(properties, "open_visual_editor", obs_module_text("Editor.Open"), open_visual_editor,
				   layout);
	obs_property_t *preset = obs_properties_add_list(properties, "preset", obs_module_text("Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preset, obs_module_text("Preset.Single"), PRESET_SINGLE);
	obs_property_list_add_int(preset, obs_module_text("Preset.TwoColumns"), PRESET_TWO_COLUMNS);
	obs_property_list_add_int(preset, obs_module_text("Preset.TwoRows"), PRESET_TWO_ROWS);
	obs_property_list_add_int(preset, obs_module_text("Preset.ThreeColumns"), PRESET_THREE_COLUMNS);
	obs_property_list_add_int(preset, obs_module_text("Preset.HeroLeft"), PRESET_HERO_LEFT);
	obs_property_list_add_int(preset, obs_module_text("Preset.GridFour"), PRESET_GRID_FOUR);
	obs_property_list_add_int(preset, obs_module_text("Preset.HeroTop"), PRESET_HERO_TOP);
	obs_property_list_add_int(preset, obs_module_text("Preset.GridSix"), PRESET_GRID_SIX);
	obs_property_list_add_int(preset, obs_module_text("Preset.Custom"), PRESET_CUSTOM);
	obs_property_set_modified_callback(preset, preset_modified);

	obs_properties_add_int(properties, "width", obs_module_text("Canvas.Width"), 320, 7680, 1);
	obs_properties_add_int(properties, "height", obs_module_text("Canvas.Height"), 180, 4320, 1);
	obs_property_t *custom_count = obs_properties_add_int_slider(
		properties, "custom_box_count", obs_module_text("Custom.BoxCount"), 1, MAX_BOXES, 1);
	obs_property_set_modified_callback(custom_count, preset_modified);
	obs_properties_add_float_slider(properties, "gap", obs_module_text("Gap"), 0.0, 200.0, 1.0);
	obs_properties_add_color_alpha(properties, "background_color", obs_module_text("Background.Color"));
	obs_property_t *background = obs_properties_add_list(properties, "background_source",
							     obs_module_text("Background.Source"), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_STRING);
	populate_source_list(background, layout ? layout->context : NULL);

	char key[64];
	char title[64];
	for (int i = 0; i < MAX_BOXES; i++) {
		obs_properties_t *group = obs_properties_create();
		box_key(key, sizeof(key), i, "source");
		obs_property_t *source = obs_properties_add_list(group, key, obs_module_text("Box.Source"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		populate_source_list(source, layout ? layout->context : NULL);
		box_key(key, sizeof(key), i, "fit_mode");
		obs_property_t *fit_mode = obs_properties_add_list(group, key, obs_module_text("Box.FitMode"),
								   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(fit_mode, obs_module_text("Fit.Fill"), FIT_FILL);
		obs_property_list_add_int(fit_mode, obs_module_text("Fit.Contain"), FIT_CONTAIN);
		obs_property_list_add_int(fit_mode, obs_module_text("Fit.Stretch"), FIT_STRETCH);
		obs_property_list_add_int(fit_mode, obs_module_text("Fit.Manual"), FIT_MANUAL);
		box_key(key, sizeof(key), i, "zoom");
		obs_properties_add_float_slider(group, key, obs_module_text("Box.Zoom"), 0.1, 4.0, 0.01);
		box_key(key, sizeof(key), i, "pan_x");
		obs_properties_add_float_slider(group, key, obs_module_text("Box.PanX"), -100.0, 100.0, 1.0);
		box_key(key, sizeof(key), i, "pan_y");
		obs_properties_add_float_slider(group, key, obs_module_text("Box.PanY"), -100.0, 100.0, 1.0);
		box_key(key, sizeof(key), i, "radius");
		obs_properties_add_float_slider(group, key, obs_module_text("Box.Radius"), 0.0, 500.0, 1.0);
		box_key(key, sizeof(key), i, "border_width");
		obs_properties_add_float_slider(group, key, obs_module_text("Box.BorderWidth"), 0.0, 100.0, 1.0);
		box_key(key, sizeof(key), i, "border_color");
		obs_properties_add_color_alpha(group, key, obs_module_text("Box.BorderColor"));
		box_key(key, sizeof(key), i, "custom_help");
		obs_properties_add_text(group, key, obs_module_text("Custom.GeometryHelp"), OBS_TEXT_INFO);
		box_key(key, sizeof(key), i, "custom_x");
		obs_properties_add_float(group, key, obs_module_text("Custom.X"), 0.0, 7680.0, 1.0);
		box_key(key, sizeof(key), i, "custom_y");
		obs_properties_add_float(group, key, obs_module_text("Custom.Y"), 0.0, 4320.0, 1.0);
		box_key(key, sizeof(key), i, "custom_width");
		obs_properties_add_float(group, key, obs_module_text("Custom.Width"), 40.0, 7680.0, 1.0);
		box_key(key, sizeof(key), i, "custom_height");
		obs_properties_add_float(group, key, obs_module_text("Custom.Height"), 40.0, 4320.0, 1.0);
		box_key(key, sizeof(key), i, "z_index");
		obs_properties_add_int_slider(group, key, obs_module_text("Custom.Layer"), 0, MAX_BOXES - 1, 1);
		box_key(key, sizeof(key), i, "lock_aspect");
		obs_properties_add_bool(group, key, obs_module_text("Custom.LockAspect"));

		snprintf(title, sizeof(title), "%s %d", obs_module_text("Box"), i + 1);
		box_key(key, sizeof(key), i, "group");
		obs_properties_add_group(properties, key, title, OBS_GROUP_NORMAL, group);
	}
	if (layout) {
		obs_data_t *settings = obs_source_get_settings(layout->context);
		preset_modified(properties, preset, settings);
		obs_data_release(settings);
	}

	return properties;
}

static const char *box_layout_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BoxLayout.SourceName");
}

struct obs_source_info box_layout_source_info = {
	.id = "box_layout_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE | OBS_SOURCE_SRGB |
			OBS_SOURCE_INTERACTION,
	.get_name = box_layout_name,
	.create = box_layout_create,
	.destroy = box_layout_destroy,
	.update = box_layout_update,
	.save = box_layout_save,
	.get_defaults = box_layout_defaults,
	.get_properties = box_layout_properties,
	.video_render = box_layout_render,
	.video_tick = box_layout_tick,
	.get_width = box_layout_width,
	.get_height = box_layout_height,
	.audio_render = box_layout_audio_render,
	.mouse_click = box_layout_mouse_click,
	.mouse_move = box_layout_mouse_move,
	.mouse_wheel = box_layout_mouse_wheel,
	.focus = box_layout_focus,
	.enum_active_sources = box_layout_enum_sources,
	.enum_all_sources = box_layout_enum_sources,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
};
