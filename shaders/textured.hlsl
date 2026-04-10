// Textured shader with Y-axis scissor clipping
// Clips fragments outside y_min to y_max bounds

// Vertex input structure (matches EFIS vertex_format_T2F_C4UB_V3F)
struct VSInput
{
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;      // Normalized from UBYTE4
    float3 position : TEXCOORD2;
};

// Universal uniform buffer structure (all fields, uses only what's needed)
// SDL3 GPU expects: vertex uniforms in set 1, fragment uniforms in set 3
#ifdef VERTEX_SHADER
cbuffer Uniforms : register(b0, space1)
#else
cbuffer Uniforms : register(b0, space3)
#endif
{
    // Core transformation matrices
    float4x4 projection_matrix;
    float4x4 view_matrix;
    float4x4 model_matrix;
    float4x4 texture_matrix;
    float4x4 color_matrix;

    // Text outline parameters (unused in textured shader)
    float4 outline_color;
    int2 texture_size;

    // Y-cut parameters (scissor bounds)
    float y_min;
    float y_max;
};

// Vertex output / Fragment input structure
struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 texcoord : TEXCOORD0;
    float model_y : TEXCOORD1;     // Model-space Y for scissor test
};

// Vertex shader - passes model Y to fragment for scissor test
PSInput vertex_main(VSInput input)
{
    PSInput output;

    // Apply color matrix to vertex color
    output.color = mul(color_matrix, input.color);

    // Apply texture matrix to texcoord
    output.texcoord = mul(texture_matrix, float4(input.texcoord, 0.0, 1.0));

    // Pass model-space Y coordinate for fragment scissor test
    output.model_y = input.position.y;

    // Transform position: projection * view * model * position
    float4 pos = float4(input.position, 1.0);
    output.position = mul(projection_matrix, mul(view_matrix, mul(model_matrix, pos)));

    return output;
}

// Texture and sampler (bound at runtime)
// SDL3 GPU expects fragment samplers/textures in descriptor set 2 (space2)
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

// Fragment shader - discards fragments outside y_min/y_max bounds
float4 fragment_main(PSInput input) : SV_Target
{
    // Scissor test: discard fragments outside Y bounds
    clip(input.model_y - y_min);  // Discard if model_y < y_min
    clip(y_max - input.model_y);  // Discard if model_y > y_max

    float4 texColor = tex.Sample(samp, input.texcoord.xy);
    return input.color * texColor;
}
