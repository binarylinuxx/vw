// vw's rectangle shader interface. Include it at the top of a compute shader
// and compile with vw-shaderc:
//
//   #version 450
//   #define VW_USE_BACKDROP   // only if you sample what was drawn before
//   #define VW_USE_DATA       // only if you read shader_data
//   #define VW_USE_TIME       // only if you read vw_time(): redraws every frame
//   #define VW_USE_TARGETS    // a shader pass with named images (output, inputs)
//   #include <vw/shader.glsl>
//
//   void main() {
//     ivec2 pixel; vec2 local;
//     if (!vw_begin(pixel, local)) return;
//     float d = vw_rounded_box(local - vw_size() * 0.5, vw_size() * 0.5, vw_radius());
//     vw_blend(pixel, vw.color * vw_coverage(d));
//   }
//
// Declaring the backdrop or the data buffer is what tells vw the shader needs
// them: vw-shaderc records it in the .vws file, and the renderer only copies
// the surface into the backdrop for shaders that sample it.

#ifndef VW_SHADER_GLSL
#define VW_SHADER_GLSL
// Pass images find their geometry and inputs after the pass's data.
#if defined(VW_USE_TARGETS) && !defined(VW_USE_DATA)
#define VW_USE_DATA
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Binding 0: the surface, premultiplied RGBA.
layout(rgba8, set = 0, binding = 0) uniform image2D vw_target;

layout(push_constant) uniform VwRectangle {
  vec4 bounds;         // x, y, width, height in surface pixels
  vec4 params;         // radius, border width, gradient mode, exact coverage
  vec4 color;          // straight (not premultiplied) RGBA
  vec4 color2;
  vec4 border_color;
  vec4 clip;           // x, y, width, height; width 0 means no clip
  vec4 clip_params;    // clip radius, data offset (words), data length, time
  vec4 shader_params;  // RectangleProps::shader_params
} vw;

#ifdef VW_USE_DATA
// Binding 1: RectangleProps::shader_data, as raw 32-bit words.
layout(std430, set = 0, binding = 1) readonly buffer VwData { uint words[]; } vw_data_buffer;
// Element i of shader_data.
float vw_data(uint index) {
  return uintBitsToFloat(vw_data_buffer.words[uint(vw.clip_params.y) + index]);
}
uint vw_data_length() { return uint(vw.clip_params.z); }
#endif

#ifdef VW_USE_BACKDROP
// Binding 2: everything drawn before this rectangle, read-only.
layout(rgba8, set = 0, binding = 2) readonly uniform image2D vw_backdrop_image;
vec4 vw_backdrop(ivec2 pixel) {
  return imageLoad(vw_backdrop_image, clamp(pixel, ivec2(0), imageSize(vw_backdrop_image) - 1));
}
#endif

#ifdef VW_USE_TARGETS
// Shader passes with named images (ShaderPass::output and ::inputs).
// Binding 3: the rectangle's named images, float RGBA, each the rectangle
// grown by target_padding on every side. Binding 4: what a pass with an
// output writes; vw copies it into the output's image after the pass, so a
// pass may read the image it replaces.
layout(rgba32f, set = 0, binding = 3) readonly uniform image2D vw_targets[8];
layout(rgba32f, set = 0, binding = 4) writeonly uniform image2D vw_pass_output;

// After the pass's own data: padding, image width, height, input count,
// then each input's image.
float vw_pass_word(uint index) {
  return uintBitsToFloat(vw_data_buffer.words[uint(vw.clip_params.y) + uint(vw.clip_params.z) + index]);
}
float vw_target_padding() { return vw_pass_word(0u); }
vec2 vw_target_size() { return vec2(vw_pass_word(1u), vw_pass_word(2u)); }
int vw_input_count() { return int(vw_pass_word(3u)); }

// Where the images' pixel (0, 0) is on the surface.
vec2 vw_target_origin() { return floor(vw.bounds.xy) - vw_target_padding(); }
// A surface position in image pixels, and back.
vec2 vw_target_position(vec2 surface) { return surface - vw_target_origin(); }
vec2 vw_surface_position(vec2 image) { return image + vw_target_origin(); }

// A pass with an output runs once for each pixel of its image: the pixel,
// false past the image.
bool vw_pass_begin(out ivec2 pixel) {
  pixel = ivec2(gl_GlobalInvocationID.xy);
  return all(lessThan(pixel, ivec2(vw_target_size())));
}
// A pass pixel's centre relative to the rectangle's top-left, as vw_begin's
// `local`: negative or past vw_size() in the padding.
vec2 vw_pass_local(ivec2 pixel) { return vec2(pixel) + 0.5 - vw_target_padding(); }
// Writes the pass's output at an image pixel.
void vw_store(ivec2 pixel, vec4 value) { imageStore(vw_pass_output, pixel, value); }

// Image arrays take constant indices everywhere; a switch keeps it so.
vec4 vw_image_load(int image, ivec2 pixel) {
  switch (image) {
    case 0: return imageLoad(vw_targets[0], pixel);
    case 1: return imageLoad(vw_targets[1], pixel);
    case 2: return imageLoad(vw_targets[2], pixel);
    case 3: return imageLoad(vw_targets[3], pixel);
    case 4: return imageLoad(vw_targets[4], pixel);
    case 5: return imageLoad(vw_targets[5], pixel);
    case 6: return imageLoad(vw_targets[6], pixel);
    default: return imageLoad(vw_targets[7], pixel);
  }
}
// Input `which` (its place in ShaderPass::inputs) at an image pixel,
// clamped to the image.
vec4 vw_input_load(int which, ivec2 pixel) {
  int image = int(vw_pass_word(4u + uint(which)));
  return vw_image_load(image, clamp(pixel, ivec2(0), ivec2(vw_target_size()) - 1));
}
// The same between pixels, bilinear, at a position in image pixels (a
// pixel's centre is at +0.5).
vec4 vw_input_sample(int which, vec2 position) {
  vec2 p = position - 0.5;
  ivec2 i = ivec2(floor(p));
  vec2 f = fract(p);
  vec4 top = mix(vw_input_load(which, i), vw_input_load(which, i + ivec2(1, 0)), f.x);
  vec4 bottom = mix(vw_input_load(which, i + ivec2(0, 1)), vw_input_load(which, i + ivec2(1, 1)), f.x);
  return mix(top, bottom, f.y);
}
#endif

#ifdef VW_USE_TIME
// Seconds since the app started drawing, the same for every shader in a
// frame. While a shader that reads it is on screen, its surface redraws
// every frame.
float vw_time() { return vw.clip_params.w; }
#endif

vec2 vw_size() { return vw.bounds.zw; }
float vw_radius() { return vw.params.x; }
float vw_param(int index) { return vw.shader_params[index]; }

// Signed distance from p to a box of half-size b with corner radius r,
// centred on the origin: negative inside.
float vw_rounded_box(vec2 p, vec2 b, float r) {
  r = clamp(r, 0.0, min(b.x, b.y));
  vec2 q = abs(p) - b + r;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Antialiased coverage of a signed distance (one pixel wide edge).
float vw_coverage(float distance) { return clamp(0.5 - distance, 0.0, 1.0); }

// The pixel this invocation draws. False when it lies outside the rectangle,
// the surface or the parent clip; return at once then.
bool vw_begin(out ivec2 pixel, out vec2 local) {
  ivec2 offset = ivec2(gl_GlobalInvocationID.xy);
  local = vec2(offset) + 0.5;
  pixel = ivec2(floor(vw.bounds.xy)) + offset;
  if (any(greaterThanEqual(offset, ivec2(ceil(vw.bounds.zw))))) return false;
  if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, imageSize(vw_target)))) return false;
  if (vw.clip.z > 0.0 && vw.clip.w > 0.0) {
    vec2 half_clip = vw.clip.zw * 0.5;
    vec2 at = vec2(pixel) + 0.5 - vw.clip.xy - half_clip;
    if (vw_rounded_box(at, half_clip, vw.clip_params.x) > 0.0) return false;
  }
  return true;
}

// Composites premultiplied colour over what is already there.
void vw_blend(ivec2 pixel, vec4 premultiplied) {
  if (premultiplied.a <= 0.0) return;
  vec4 current = imageLoad(vw_target, pixel);
  imageStore(vw_target, pixel, premultiplied + current * (1.0 - premultiplied.a));
}

// Straight RGBA at an opacity, ready for vw_blend.
vec4 vw_premultiply(vec4 color, float amount) {
  float a = color.a * clamp(amount, 0.0, 1.0);
  return vec4(color.rgb * a, a);
}

#endif
