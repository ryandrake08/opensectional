// SDF line shader — instanced rendering
// Renders thick polylines and circles using signed distance fields.
// One instance per primitive. Quad vertices generated from SV_VertexID.
// Per-instance data (bounds, style, primitive type) read from metadata buffer.
// Polyline points packed into a single storage buffer.

// Primitive types
#define PRIMITIVE_POLYLINE 0
#define PRIMITIVE_CIRCLE   1

// Shared uniforms (same for all instances)
#ifdef VERTEX_SHADER
cbuffer SharedUniforms : register(b0, space1)
#else
cbuffer SharedUniforms : register(b0, space3)
#endif
{
    float4x4 projection_matrix;
    float4x4 view_matrix;
    float2 viewport_size;
    float2 world_to_screen_scale;
    float2 world_to_screen_offset;
    float2 _pad0;
};

// Per-instance metadata (must match polyline_metadata_gpu layout)
struct PolylineMetadata
{
    float4 bounds_min_max;     // xy = world min, zw = world max (no margin)
    float4 line_color;         // RGBA
    float4 border_color;       // RGBA
    float line_half_width;     // pixels
    float border_width;        // pixels (outside of path direction)
    float dash_length;         // pixels (0 = solid line)
    float gap_length;          // pixels
    float fill_width;          // pixels (inside/left of path direction)
    uint segment_count;
    uint point_offset;
    uint primitive_type;       // 0 = polyline, 1 = circle
    float2 circle_center;     // world-space Mercator (for circles)
    float circle_radius;       // world-space Mercator (for circles)
    float _pad2;
};

// Vertex stage: metadata at vertex storage slot 0
// Fragment stage: packed points at slot 0, metadata at slot 1
#ifdef VERTEX_SHADER
StructuredBuffer<PolylineMetadata> metadata : register(t0, space0);
StructuredBuffer<float4> polyline : register(t1, space0);
#else
StructuredBuffer<float4> polyline : register(t0, space2);
StructuredBuffer<PolylineMetadata> metadata : register(t1, space2);
#endif

struct PSInput
{
    float4 position : SV_Position;
    float2 world_pos : TEXCOORD0;
    nointerpolation uint instance_id : TEXCOORD1;
};

PSInput vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    PolylineMetadata meta = metadata[instance_id];

    // Expand bounds by line margin in world space
    float effective_fill = (meta.fill_width > 0) ? meta.fill_width : meta.border_width;
    float margin_pixels = meta.line_half_width + max(meta.border_width, effective_fill);
    float margin_x = margin_pixels / abs(world_to_screen_scale.x);
    float margin_y = margin_pixels / abs(world_to_screen_scale.y);

    float4 bounds = meta.bounds_min_max;
    bounds.xy -= float2(margin_x, margin_y);
    bounds.zw += float2(margin_x, margin_y);

    // Two triangles forming a quad
    float2 uv;
    switch(vertex_id)
    {
    case 0: uv = float2(0, 0); break;
    case 1: uv = float2(1, 0); break;
    case 2: uv = float2(0, 1); break;
    case 3: uv = float2(1, 0); break;
    case 4: uv = float2(1, 1); break;
    case 5: uv = float2(0, 1); break;
    default: uv = float2(0, 0); break;
    }

    float2 world_pos = lerp(bounds.xy, bounds.zw, uv);

    PSInput output;
    output.position = mul(projection_matrix, mul(view_matrix, float4(world_pos, 0, 1)));
    output.world_pos = world_pos;
    output.instance_id = instance_id;
    return output;
}

float4 fragment_main(PSInput input) : SV_Target
{
    PolylineMetadata meta = metadata[input.instance_id];

    float2 screen_pos = input.world_pos * world_to_screen_scale + world_to_screen_offset;

    float min_dist = 1e10;
    float path_dist = 0;
    bool inside = true;

    if(meta.primitive_type == PRIMITIVE_CIRCLE)
    {
        // Analytical circle SDF — O(1) per fragment
        float2 center_s = meta.circle_center * world_to_screen_scale + world_to_screen_offset;
        float radius_s = meta.circle_radius * abs(world_to_screen_scale.x);
        float d = length(screen_pos - center_s);
        min_dist = abs(d - radius_s);
        inside = (d < radius_s);
        path_dist = atan2(screen_pos.y - center_s.y, screen_pos.x - center_s.x) * radius_s;
    }
    else
    {
        // Polyline SDF — loop over segments
        uint base = meta.point_offset;
        float winding = sign(world_to_screen_scale.x * world_to_screen_scale.y);

        for(uint i = 0; i < meta.segment_count; i++)
        {
            float2 a = polyline[base + i].xy * world_to_screen_scale + world_to_screen_offset;
            float2 b = polyline[base + i + 1].xy * world_to_screen_scale + world_to_screen_offset;

            float2 ab = b - a;
            float len_sq = dot(ab, ab);
            float t = (len_sq > 1e-8) ? saturate(dot(screen_pos - a, ab) / len_sq) : 0;
            float dist = length(screen_pos - (a + t * ab));

            if(dist < min_dist)
            {
                min_dist = dist;
                // Dash phase from world position projected onto segment direction
                float2 world_ab = ab / world_to_screen_scale;
                float world_len_sq = dot(world_ab, world_ab);
                float2 world_dir = (world_len_sq > 1e-8) ? world_ab * rsqrt(world_len_sq) : float2(1, 0);
                if(world_dir.x < 0 || (world_dir.x == 0 && world_dir.y < 0)) world_dir = -world_dir;
                path_dist = dot(input.world_pos, world_dir) * length(world_to_screen_scale * world_dir);
            }

            float cross_val = ab.x * (screen_pos.y - a.y) - ab.y * (screen_pos.x - a.x);
            if(cross_val * winding < 0) inside = false;
        }
    }

    // For convex CCW polygons with fill_width set, use fill_width on the inside
    float effective_border = inside ? meta.fill_width : meta.border_width;

    // SDF for perpendicular distance from line center
    float perp_sdf = min_dist - meta.line_half_width;

    // SDF for dash pattern (negative = inside dash, positive = in gap)
    float dash_sdf = -1e10;
    if(meta.dash_length > 0)
    {
        float cycle = meta.dash_length + meta.gap_length;
        float pos_in_cycle = fmod(path_dist, cycle);
        if(pos_in_cycle < 0) pos_in_cycle += cycle;

        if(pos_in_cycle < meta.dash_length)
        {
            dash_sdf = -min(pos_in_cycle, meta.dash_length - pos_in_cycle);
        }
        else
        {
            dash_sdf = min(pos_in_cycle - meta.dash_length, cycle - pos_in_cycle);
        }
    }

    // Combined SDF: intersection of line body and dash region
    float combined = max(perp_sdf, dash_sdf);

    // Discard fragments outside border
    if(combined > effective_border)
    {
        discard;
    }

    // Border vs line interior
    float4 color = (combined > 0) ? meta.border_color : meta.line_color;

    // Anti-aliasing at outer edge
    float aa = 1.0 - smoothstep(effective_border - 1.0, effective_border, combined);
    color.a *= aa;

    return color;
}
