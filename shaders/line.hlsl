// SDF line shader
// Renders thick polylines with dashes and borders using signed distance fields.
// Quad vertices are generated from SV_VertexID (no vertex buffer).
// Polyline vertex data is read from a fragment storage buffer.

#ifdef VERTEX_SHADER
cbuffer LineUniforms : register(b0, space1)
#else
cbuffer LineUniforms : register(b0, space3)
#endif
{
    float4x4 projection_matrix;
    float4x4 view_matrix;
    float4 bounds_min_max;         // xy = world min, zw = world max (expanded by margin)
    float4 line_color;             // RGBA
    float4 border_color;           // RGBA
    float2 viewport_size;          // pixels
    float2 world_to_screen_scale;  // pixels per world unit (y is negative for Y flip)
    float2 world_to_screen_offset; // screen-space position of world origin
    float line_half_width;         // pixels
    float border_width;            // pixels (outside of path direction)
    float dash_length;             // pixels (0 = solid line)
    float gap_length;              // pixels
    float fill_width;              // pixels (inside/left of path direction)
    uint segment_count;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 world_pos : TEXCOORD0;
};

PSInput vertex_main(uint vertex_id : SV_VertexID)
{
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

    float2 world_pos = lerp(bounds_min_max.xy, bounds_min_max.zw, uv);

    PSInput output;
    output.position = mul(projection_matrix, mul(view_matrix, float4(world_pos, 0, 1)));
    output.world_pos = world_pos;
    return output;
}

// Polyline vertices: xy = world position, zw = unused
StructuredBuffer<float4> polyline : register(t0, space2);

float4 fragment_main(PSInput input) : SV_Target
{
    float2 screen_pos = input.world_pos * world_to_screen_scale + world_to_screen_offset;

    // Find nearest segment and compute distance along path.
    // Also test if fragment is inside a convex polygon by checking
    // the cross product against every segment (not just the nearest).
    // The winding factor accounts for the Y-flip in screen coordinates.
    float winding = sign(world_to_screen_scale.x * world_to_screen_scale.y);
    float min_dist = 1e10;
    float path_dist = 0;
    float cumulative = 0;
    bool inside = true;

    for(uint i = 0; i < segment_count; i++)
    {
        float2 a = polyline[i].xy * world_to_screen_scale + world_to_screen_offset;
        float2 b = polyline[i + 1].xy * world_to_screen_scale + world_to_screen_offset;

        float2 ab = b - a;
        float len_sq = dot(ab, ab);
        float t = (len_sq > 1e-8) ? saturate(dot(screen_pos - a, ab) / len_sq) : 0;
        float dist = length(screen_pos - (a + t * ab));
        float seg_len = sqrt(len_sq);

        if(dist < min_dist)
        {
            min_dist = dist;
            path_dist = cumulative + t * seg_len;
        }

        float cross_val = ab.x * (screen_pos.y - a.y) - ab.y * (screen_pos.x - a.x);
        if(cross_val * winding < 0) inside = false;

        cumulative += seg_len;
    }

    // For convex CCW polygons with fill_width set, use fill_width on the inside
    float effective_border = inside ? fill_width : border_width;

    // SDF for perpendicular distance from line center
    float perp_sdf = min_dist - line_half_width;

    // SDF for dash pattern (negative = inside dash, positive = in gap)
    float dash_sdf = -1e10;
    if(dash_length > 0)
    {
        float cycle = dash_length + gap_length;
        float pos_in_cycle = fmod(path_dist, cycle);

        if(pos_in_cycle < dash_length)
        {
            dash_sdf = -min(pos_in_cycle, dash_length - pos_in_cycle);
        }
        else
        {
            dash_sdf = min(pos_in_cycle - dash_length, cycle - pos_in_cycle);
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
    float4 color = (combined > 0) ? border_color : line_color;

    // Anti-aliasing at outer edge
    float aa = 1.0 - smoothstep(effective_border - 1.0, effective_border, combined);
    color.a *= aa;

    return color;
}
