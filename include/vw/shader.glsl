// vw's rectangle shader interface. Include it at the top of a compute shader
// and compile with vw-shaderc:
//
//   #version 450
//   #define VW_USE_BACKDROP   // only if you sample what was drawn before
//   #define VW_USE_DATA       // only if you read shader_data
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
  vec4 clip_params;    // clip radius, data offset (words), data length, 0
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
