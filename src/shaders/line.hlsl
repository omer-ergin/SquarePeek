// line.hlsl — precompiled with fxc at build time (CMakeLists.txt, renderer.cpp).
cbuffer LineCB : register(b0)
{
    row_major float4x4 gViewProj;
    float4 gLineParams;   // (image width, height, clip depth offset, alpha multiplier)
};

struct VSIn
{
    float3 a : POSITION;
    float3 b : TEXCOORD0;
    float4 color : COLOR;
    float2 sideWidth : TEXCOORD1;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOut VSLine(VSIn input)
{
    float4 A = mul(float4(input.a, 1.0f), gViewProj);
    float4 B = mul(float4(input.b, 1.0f), gViewProj);
    float2 dir = (B.xy - A.xy) * gLineParams.xy;
    float len = length(dir);
    dir = len > 1e-5f ? dir / len : float2(1.0f, 0.0f);
    float2 n = float2(-dir.y, dir.x);
    A.xy += n * input.sideWidth.x * input.sideWidth.y / gLineParams.xy;
    A.z -= gLineParams.z;

    VSOut output;
    output.position = A;
    output.color = float4(input.color.rgb, input.color.a * gLineParams.w);
    return output;
}

float4 PSLine(VSOut input) : SV_TARGET
{
    return input.color;
}
