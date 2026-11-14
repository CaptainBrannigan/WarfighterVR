// SCOPE LENS PICTURE (2026-11-14, prototype, hooks/scope_view.h): one eye's half of the frame gets a magnified picture
// inside the scope's lens, sampled from the side-by-side frame (the other eye's half: its view of the sight line's far
// field isn't blocked by the scope). Compiled offline (build_scope_lens.cmd -> scope_lens_vs.h / scope_lens_ps.h), so the
// DLL needs no d3dcompiler.
//   uv' = uv * Map.xy + Map.zw   -- this half's uv to the source frame's uv
//   inside ((uv - Circle.xy) / (Circle.z, Circle.z * Circle.w)) <= 1, with a dark rim

cbuffer Lens : register(b0)
{
    float4 Map;
    float4 Circle;
};

Texture2D Source : register(t0);
SamplerState Linear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// One triangle covering the target: no vertex buffer.
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 d = (i.uv - Circle.xy) / float2(Circle.z, Circle.z * Circle.w);
    float r2 = dot(d, d);
    clip(1.0 - r2);
    float3 c = Source.SampleLevel(Linear, i.uv * Map.xy + Map.zw, 0).rgb;
    c *= saturate((1.0 - r2) * 6.0); // the lens's dark rim
    return float4(c, 1.0);
}
