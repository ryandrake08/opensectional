// Terrain shaded relief.
// The height texture is R16_UNORM: 1 m per LSB with a -1000 m bias, so a raw
// sample of 0 is the no-data sentinel and decodes to exactly -1000 m.

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
    int2 texture_size;             // height texture dimensions, skirt included
    float y_min;
    float y_max;
    float sun_azimuth;             // radians, clockwise from north
    float sun_altitude;            // radians above the horizon
    float vertical_exaggeration;
    float terrain_opacity;
    float terrain_texel_m;         // Web Mercator metres per height texel at this tile's zoom
    int terrain_mode;              // 0 hillshade, 1 hypsometric, 2 relative-to-cruise
    float hypso_min_m;             // elevation mapped to the ramp start
    float hypso_max_m;             // elevation mapped to the ramp end
    float taws_cruise_m;           // cruise altitude for relative-to-cruise mode
    float taws_warning_m;          // metres below cruise for the warning band
    float taws_caution_m;          // metres below cruise for the caution band
    float taws_clear_m;            // metres below cruise where terrain stops drawing
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float model_y : TEXCOORD1;     // Web Mercator y (metres), for the cos(lat) correction
};

PSInput vertex_main(VSInput input)
{
    PSInput output;
    output.texcoord = input.texcoord;
    output.model_y = input.position.y;
    output.position = mul(projection_matrix, mul(view_matrix, float4(input.position, 1.0)));
    return output;
}

Texture2D height_texture : register(t0, space2);
SamplerState height_sampler : register(s0, space2);
Texture2D ramp_texture : register(t1, space2);
SamplerState ramp_sampler : register(s1, space2);

static const float EARTH_RADIUS_M = 6378137.0;
static const float MIN_VALID_HEIGHT_M = -999.5; // anything lower is the no-data sentinel
static const float PI = 3.14159265;

float decode_height_m(float2 uv)
{
    return height_texture.Sample(height_sampler, uv).r * 65535.0 - 1000.0;
}

float4 fragment_main(PSInput input) : SV_Target
{
    float2 texel = 1.0 / float2(texture_size);

    float h_c = decode_height_m(input.texcoord);
    if(h_c < MIN_VALID_HEIGHT_M)
    {
        discard;
    }

    float h_e = decode_height_m(input.texcoord + float2(texel.x, 0.0));
    float h_w = decode_height_m(input.texcoord - float2(texel.x, 0.0));
    float h_s = decode_height_m(input.texcoord + float2(0.0, texel.y));
    float h_n = decode_height_m(input.texcoord - float2(0.0, texel.y));

    // A no-data neighbour is past the dataset's coverage; treat that side as
    // flat rather than letting it read as a cliff.
    if(h_e < MIN_VALID_HEIGHT_M) h_e = h_c;
    if(h_w < MIN_VALID_HEIGHT_M) h_w = h_c;
    if(h_s < MIN_VALID_HEIGHT_M) h_s = h_c;
    if(h_n < MIN_VALID_HEIGHT_M) h_n = h_c;

    // Web Mercator overstates ground distance by 1/cos(lat); cos(lat) = sech(y/R).
    float ground_texel_m = terrain_texel_m / cosh(input.model_y / EARTH_RADIUS_M);

    // Central-difference slope, rise over ground run (v increases southward).
    float dz_dx = (h_e - h_w) / (2.0 * ground_texel_m);
    float dz_dy = (h_s - h_n) / (2.0 * ground_texel_m);

    float slope = atan(vertical_exaggeration * sqrt(dz_dx * dz_dx + dz_dy * dz_dy));
    float aspect = atan2(dz_dy, -dz_dx);

    float zenith = (PI / 2.0) - sun_altitude;
    float azimuth = (PI / 2.0) - sun_azimuth; // compass (CW from north) -> math (CCW from east)

    float illum = cos(zenith) * cos(slope) + sin(zenith) * sin(slope) * cos(azimuth - aspect);
    illum = saturate(illum);

    float3 tint;
    if(terrain_mode == 1)
    {
        // Hypsometric: absolute elevation through the colour ramp.
        float t = saturate((h_c - hypso_min_m) / max(hypso_max_m - hypso_min_m, 1.0));
        tint = ramp_texture.Sample(ramp_sampler, float2(t, 0.5)).rgb;
    }
    else if(terrain_mode == 2)
    {
        // Relative to cruise altitude: terrain far below the aircraft is not
        // drawn; closer terrain steps through green / amber / red.
        float below = taws_cruise_m - h_c;
        if(below <= taws_warning_m)
        {
            tint = float3(0.85, 0.10, 0.10);
        }
        else if(below <= taws_caution_m)
        {
            tint = float3(0.90, 0.60, 0.10);
        }
        else if(below <= taws_clear_m)
        {
            tint = float3(0.15, 0.55, 0.15);
        }
        else
        {
            discard;
        }
    }
    else
    {
        tint = float3(1.0, 1.0, 1.0);
    }

    return float4(tint * illum, terrain_opacity);
}
