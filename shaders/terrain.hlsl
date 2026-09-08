struct VSInput
{
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float3 position : TEXCOORD2;
};

#ifdef VERTEX_SHADER
cbuffer Uniforms : register(b0, space1)
#else
cbuffer Uniforms : register(b0, space3)
#endif
{
    float4x4 projection_matrix;
    float4x4 view_matrix;
    float4x4 model_matrix;
    float4x4 texture_matrix;
    float4x4 color_matrix;
    float4 outline_color;
    int2 texture_size;
    float y_min;
    float y_max;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

PSInput vertex_main(VSInput input)
{
    PSInput output;
    output.texcoord = input.texcoord;
    output.position = mul(projection_matrix, mul(view_matrix, float4(input.position, 1.0)));
    return output;
}

Texture2D height_texture : register(t0, space2);
SamplerState height_sampler : register(s0, space2);

float4 fragment_main(PSInput input) : SV_Target
{
    const float height_m = height_texture.Sample(height_sampler, input.texcoord).r * 65535.0 - 1000.0;
    const float grey = 0.35 + 0.25 * saturate((height_m + 1000.0) / 8000.0);
    return float4(grey, grey, grey, 0.55);
}
