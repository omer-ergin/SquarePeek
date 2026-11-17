// blit.hlsl — precompiled with fxc at build time (CMakeLists.txt, renderer.cpp).
Texture2D gSource : register(t0);
SamplerState gSampler : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSFullscreen(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    VSOut output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = corner;
    return output;
}

float4 PSBlit(VSOut input) : SV_TARGET
{
    return gSource.Sample(gSampler, float2(input.uv.x, 1.0f - input.uv.y));
}
