#include "stdafx.h"

#include "xrMU_Model_Reference.h"
#include "xrMU_Model.h"

#include "light_point.h"
#include "xrFace.h"
#include "xrLC_GlobalData.h"

template<typename T, typename T2>
void vfComputeLinearRegression(xr_vector<T>& A, xr_vector<T>& B, T2& C, T2& D)
{
	u32 N = (u32)A.size();
	T sx = T(0), sy = T(0), sxy = T(0), sx2 = T(0), l_tDenominator;

	sx = std::accumulate(A.begin(), A.end(), sx);
	sy = std::accumulate(B.begin(), B.end(), sy);
	sxy = std::inner_product(A.begin(), A.end(), B.begin(), sxy);
	sx2 = std::inner_product(A.begin(), A.end(), A.begin(), sx2);

	l_tDenominator = T(N) * sx2 - sx * sx;

	C = T2(0);
	D = T2(0);

	if (_abs(l_tDenominator) > EPS_S)
	{
		C = T2((T(N) * sxy - sx * sy) / l_tDenominator);
	}

	if (N)
	{
		D = T2((sy - C * sx) / T(N));
	}
}

template <typename T, typename T2>
T simple_optimize(xr_vector<T>& A, xr_vector<T>& B, T2& _scale, T2& _bias)
{
	T accum;
	u32 it;

	T scale = _scale;
	T bias = _bias;
	T error = flt_max;
	T elements = T(A.size());
	u32 count = 0;

	for (;;)
	{
		count++;
		if (count > 128)
		{
			_scale = (T2)scale;
			_bias = (T2)bias;
			return error;
		}

		T old_scale = scale;
		T old_bias = bias;

		//1. scale
		u32 _ok = 0;
		for (accum = 0, it = 0; it < A.size(); it++)
			if (_abs(A[it]) > EPS_L)
			{
				accum += (B[it] - bias) / A[it];
				_ok += 1;
			}
		T s = _ok ? (accum / _ok) : scale;

		//2. bias
		T b = bias;
		if (_abs(scale) > EPS)
		{
			for (accum = 0, it = 0; it < A.size(); it++)
				accum += B[it] - A[it] / scale;
			b = accum / elements;
		}

		// mix
		T conv = 7;
		scale = ((conv - 1) * scale + s) / conv;
		bias = ((conv - 1) * bias + b) / conv;

		// error
		for (accum = 0, it = 0; it < A.size(); it++)
			accum += B[it] - (A[it] * scale + bias);
		T err = accum / elements;

		if (err < error)
		{
			// continue?
			error = err;
			if (error < EPS)
			{
				_scale = (T2)scale;
				_bias = (T2)bias;
				return error;
			}
		}
		else
		{
			// exit
			_scale = (T2)old_scale;
			_bias = (T2)old_bias;
			return error;
		}
	}
}

void o_test(int iA, int iB, int count, base_color* A, base_color* B, float& C, float& D)
{
	xr_vector<double>	_A, _B;
	_A.resize(count);
	_B.resize(count);
	for (int it = 0; it < count; it++)
	{
		base_color_c _a;	A[it]._get(_a);	float* f_a = (float*)&_a;
		base_color_c _b;	B[it]._get(_b);	float* f_b = (float*)&_b;
		_A[it] = f_a[iA];
		_B[it] = f_b[iB];
	}
	// C=1, D=0;
	simple_optimize(_A, _B, C, D);
}

void xrMU_Reference::fit_reference_lighting()
{
	model->calc_lighting(color, xform, inlc_global_data()->RCAST_Model(), inlc_global_data()->L_static(), (inlc_global_data()->b_nosun() ? LP_dont_sun : 0) | LP_DEFAULT, false);

	R_ASSERT(color.size() == model->color.size());

	// A*C + D = B
	// build data
	{
		xr_vector<double> A;	A.resize(color.size());
		xr_vector<double> B;	B.resize(color.size());
		float* _s = (float*)&c_scale;
		float* _b = (float*)&c_bias;
		for (u32 i = 0; i < 5; i++) {
			for (u32 it = 0; it < color.size(); it++)
			{
				base_color_c		__A;	model->color[it]._get(__A);
				base_color_c		__B;	color[it]._get(__B);
				A[it] = (__A.hemi);
				B[it] = ((float*)&__B)[i];
			}
			vfComputeLinearRegression(A, B, _s[i], _b[i]);
		}

		for (u32 index = 0; index < 5; index++)
			o_test(4, index, (u32)color.size(), &model->color.front(), &color.front(), _s[index], _b[index]);
	}

	// Store as the shared per-model fit (only non-unique references publish it:
	// unique instances fit in parallel and must never race on the cache). The
	// hemi scale is applied by the consumer, the cache keeps the raw regression.
	if (!bake_unique)
	{
		model->c_scale_ref		= c_scale;
		model->c_bias_ref		= c_bias;
		model->reference_fitted	= true;

		clMsg("[MU] model '%s': hemi fit scale=%.3f bias=%.3f (ref scale=%.2f)",
			*model->m_name, c_scale.hemi, c_bias.hemi, model->hemi_scale_reference);
	}
}

// The user-facing hemi scale is a multiplier on the FINAL hemi brightness:
// hemi_final = (A*C + D) * s, which means the scale must be applied to both the
// regression scale AND the bias. Multiplying only c_scale.hemi gets dominated
// by the (often large) bias term - e.g. trees under open sky fit with a small
// C and a large D, so 0.5 vs 0.1 looked nearly identical without this.
static void apply_hemi_scale(base_color_c& scale, base_color_c& bias, float s)
{
	scale.hemi	*= s;
	bias.hemi	*= s;
}

void xrMU_Reference::calc_lighting()
{
	// Non-unique instances share the per-model reference fit (one bake per model
	// instead of one per instance) - this is what keeps big MU scenes cheap.
	if (!bake_unique && model->reference_fitted)
	{
		c_scale			= model->c_scale_ref;
		c_bias			= model->c_bias_ref;
		apply_hemi_scale(c_scale, c_bias, model->hemi_scale_reference);

		// Keep 'color' populated: the LOD billboard color path in export_ogf
		// interpolates from it. Reconstruct the world bake by applying the fit
		// to the model-space bake (same transform the runtime applies).
		color.resize(model->color.size());
		for (u32 i = 0; i < color.size(); i++)
		{
			base_color_c	c;
			model->color[i]._get(c);
			c.rgb.mul(c_scale.rgb);
			c.rgb.add(c_bias.rgb);
			c.sun			= c.sun * c_scale.sun + c_bias.sun;
			c.hemi			= c.hemi * c_scale.hemi + c_bias.hemi;
			color[i]._set(c);
		}
		return;
	}

	fit_reference_lighting();

	if (bake_unique)
		apply_hemi_scale(c_scale, c_bias, hemi_scale_per_instance); // instance-level tuning replaces the model scale
	else
		apply_hemi_scale(c_scale, c_bias, model->hemi_scale_reference);
}
