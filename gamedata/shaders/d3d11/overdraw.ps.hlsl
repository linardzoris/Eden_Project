// Overdraw debug layer (rs_overdraw): flat pixel shader used by the overdraw
// replay pass. Every rasterized fragment accumulates a fixed step into
// rt_Overdraw with additive blending. Depth state comes from the replayed
// pass state (LEQUAL + depth write, replayed in the main pass draw order), so
// the accumulated value approximates the real pass per-pixel shader
// executions. Final color banding is done in combine_1.ps.hlsl.
struct p_overdraw
{
    float4 hpos : SV_POSITION;
};

float4 main(p_overdraw I) : SV_Target
{
    // 1/20 per fragment: 20 accumulated layers reach the 100% (red) band.
    return float4(0.05f, 0.0f, 0.0f, 0.0f);
}
