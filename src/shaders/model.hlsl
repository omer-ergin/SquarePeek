// model.hlsl — precompiled with fxc at build time (CMakeLists.txt, renderer.cpp).
cbuffer CameraCB : register(b0)
{
    row_major float4x4 gViewProj;
    row_major float4x4 gView;
    float4 gBaseColor;
    float4 gParams;    // (overhang highlight 0/1, kOverhangCos = cos(44°), plate contact mm, flat colour 0/1)
    float4 gParams2;   // (image width, height, -, -)
};

struct VSIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float3 normalView : TEXCOORD0;
    float3 normalModel : TEXCOORD2;
    float3 positionModel : TEXCOORD3;
};

VSOut VSMain(VSIn input)
{
    VSOut output;
    float4 world = float4(input.position, 1.0f);
    output.position = mul(world, gViewProj);
    output.normalView = mul(input.normal, (float3x3)gView);
    output.normalModel = input.normal;
    output.positionModel = input.position;
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    if (gParams.w > 0.5f)
        return float4(gBaseColor.rgb, 1.0f);

    float3 normal = normalize(input.normalView);
    // Right-handed view space: the camera looks down -Z; orthographic, so the direction to the eye is constant.
    float3 toEye = float3(0.0f, 0.0f, 1.0f);

    if (dot(normal, toEye) < 0.0f)
        normal = -normal;

    float3 keyDir = normalize(float3(-0.35f, 0.45f, 0.82f));
    float3 fillDir = normalize(float3(0.65f, -0.20f, 0.55f));

    float3 base = gBaseColor.rgb;
    if (gParams.x > 0.5f && normalize(input.normalModel).z < -gParams.y && input.positionModel.z > gParams.z)
        base = float3(0.96f, 0.34f, 0.12f);

    float key = saturate(dot(normal, keyDir));
    float fill = saturate(dot(normal, fillDir));
    float3 color = base * (0.32f + 0.72f * key + 0.20f * fill);

    float3 halfway = normalize(keyDir + toEye);
    color += pow(saturate(dot(normal, halfway)), 48.0f) * 0.20f;

    float rim = pow(1.0f - saturate(dot(normal, toEye)), 4.0f) * 0.05f;
    color += rim;

    return float4(saturate(color), 1.0f);
}
