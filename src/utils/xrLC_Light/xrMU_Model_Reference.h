#pragma once

#include "base_color.h"
 
class xrMU_Model;
struct FaceDataIntel;

namespace CDB { class CollectorPacked; }
  
class XRLC_LIGHT_API xrMU_Reference
{
public:
	xrMU_Model*				model;
    Fmatrix					xform;
    Flags32					flags;
	u16						sector;

	// MU lighting options (from the level scene object)
	bool					bake_unique;				// fit c_scale/c_bias for this instance alone
	float					hemi_scale_per_instance;	// applied instead of the model scale when bake_unique

	xr_vector<base_color>	color;

	base_color_c			c_scale;
	base_color_c			c_bias;
public:
							xrMU_Reference		(): model(0), sector(u16(-1)), flags(Flags32().assign(0)), xform(Fidentity), bake_unique(false), hemi_scale_per_instance(1.f){}

	void					Load				( IReader& fs, xr_vector<xrMU_Model*>& mu_models );
	void					calc_lighting		();
	// Bake at this instance's world position and fit c_scale/c_bias; stores the
	// result as the shared per-model fit consumed by non-unique references.
	void					fit_reference_lighting();

	void					export_cform_game	(CDB::CollectorPacked& CL);
	void					export_cform_rcast	(CDB::CollectorPacked& CL); 

	void					export_cform_rcast_new(xr_vector<FaceDataIntel>& faces);
	

};
 