// edge.hlsl — precompiled with fxc at build time (CMakeLists.txt, renderer.cpp).
cbuffer EdgeCB : register(b0)
{
    row_major float4x4 gViewProj;
    float4 gEdgeColor;
    float4 gEdgeParams;   // (clip depth offset, -, -, -)
};

float4 VSEdge(float3 position : POSITION) : SV_POSITION
{
    float4 p = mul(float4(position, 1.0f), gViewProj);
    p.z -= gEdgeParams.x * p.w;
    return p;
}

float4 PSEdge(float4 position : SV_POSITION) : SV_TARGET
{
    return gEdgeColor;
}
