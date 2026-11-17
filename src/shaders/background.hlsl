// background.hlsl — precompiled with fxc at build time (CMakeLists.txt, renderer.cpp).
cbuffer BackgroundCB : register(b0)
{
    float4 gTopColor;
    float4 gBottomColor;
    float4 gParams;
};

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

float4 PSBackground(VSOut input) : SV_TARGET
{
    float3 color = lerp(gBottomColor.rgb, gTopColor.rgb, saturate(input.uv.y));
    return float4(color, 1.0f);
}
