// SCOPE LENS PICTURE (2026-11-14, prototype, hooks/scope_view.h): one eye's half of the frame gets a magnified picture
// inside the scope's lens, sampled from the side-by-side frame (the other eye's half: its view of the sight line's far
// field isn't blocked by the scope). Compiled offline (build_scope_lens.cmd -> scope_lens_vs.h / scope_lens_ps.h), so the
// DLL needs no d3dcompiler.
//   uv' = uv * Map.xy + Map.zw   -- this half's uv to the source frame's uv
//   inside ((uv - Circle.xy) / (Circle.z, Circle.z * Circle.w)) <= 1, with a dark rim
// HOLOGRAPHIC RETICLE (2026-11-17): a duplex crosshair on the impact point (Reticle.xy, this half's uv; drawn while
// Reticle.w is 1) -- where this eye sees the laser's point, which the picture is anchored on. It moves inside the lens
// as the eye moves, and stays on the target.
// RED DOT (Reticle.w 2, 2026-11-22): no picture, a red dot of radius Reticle.z (uv x) on the impact point, inside the lens.
// The draws are alpha-blended (the picture is opaque: alpha 1).

cbuffer Lens : register(b0)
{
    float4 Map;
    float4 Circle;
    float4 Reticle;
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
    if (Reticle.w > 1.5)
    {
        // RED DOT SIGHT (2026-11-22, hooks/scope_view.h): no picture, only a glowing dot on the impact point (Reticle.xy),
        // Reticle.z across in uv x, inside the lens (the window, clipped above). Blended over the frame: soft edge, a
        // brighter core and a faint glow.
        float2 q = (i.uv - Reticle.xy) / float2(Reticle.z, Reticle.z * Circle.w);
        float r = length(q);
        float pixel = max(fwidth(i.uv.x) / Reticle.z, 1e-4);
        float core = saturate((1.0 - r) / pixel + 0.5);
        float glow = 0.35 * saturate(1.0 - (r - 1.0) / 1.5);
        float a = max(core, glow * glow);
        clip(a - 0.004);
        float3 c = lerp(float3(1.0, 0.08, 0.04), float3(1.0, 0.7, 0.62), saturate(1.0 - r * 1.8) * core);
        return float4(c, a);
    }
    float3 c = Source.SampleLevel(Linear, i.uv * Map.xy + Map.zw, 0).rgb;
    c *= saturate((1.0 - r2) * 6.0); // the lens's dark rim
    if (Reticle.w > 0.5)
    {
        // In the lens's own units (its radius = 1 both ways): fine lines through the impact point, heavy posts from 40%
        // of the radius out; at least about a pixel wide.
        float2 p = (i.uv - Reticle.xy) / float2(Circle.z, Circle.z * Circle.w);
        float pixel = fwidth(i.uv.x) / Circle.z;
        float fine = max(0.008, pixel * 0.75);
        float heavy = max(0.035, pixel * 1.5);
        float2 a = abs(p);
        bool hair = a.x < fine || a.y < fine;
        bool post = (a.y < heavy && a.x > 0.4) || (a.x < heavy && a.y > 0.4);
        if (hair || post)
            c = float3(0.0, 0.0, 0.0);
    }
    return float4(c, 1.0);
}
