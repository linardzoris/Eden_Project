#include "StdAfx.h"

#include "../xrLC_Light/xrMU_Model.h"
#include "OGF_Face.h"
#include "Build.h"

#define	TRY(a) try { a; } catch (...) { clMsg("* E: %s", #a); }

// O(1) vertex pointer -> model index lookup; replaces per-face std::find over the
// whole model vertex array which was O(F*V)
typedef xr_hash_map<const _vertex*, u32>	MUVertexIdMap;

void MModel_face2OGF_Vertices( const _face &FF, OGF_Vertex	V[3], const xrMU_Model &model, const MUVertexIdMap& vertex_ids )
{
	for (u32 k=0; k<3; k++)
	{
		_vertex*	_V		= FF.v[k];	
		auto		it		= vertex_ids.find(_V);
		R_ASSERT2	(it != vertex_ids.end(), "MU model face references vertex outside of the model");
		u32			id		= it->second;
		V[k].P			= _V->P;
		V[k].N			= _V->N; 
		V[k].Color		= model.color[id];
		V[k].T.set		(0,0,0);	//.
		V[k].B.set		(0,0,0);	//.
		V[k].UV.push_back(FF.tc[k]);
	}
}

void OGF_AddFace( OGF &ogf, const _face &FF, const xrMU_Model& model, const MUVertexIdMap& vertex_ids )
{
	OGF_Vertex		V[3];

	MModel_face2OGF_Vertices( FF, V, model, vertex_ids );
	// build face
	TRY				( ogf._BuildFace( V[0], V[1], V[2] ) );
	V[0].UV.clear();	V[1].UV.clear();	V[2].UV.clear();
}

void calc_ogf( xrMU_Model &	mu_model )
{
	// Build vertex id map once per model
	MUVertexIdMap	vertex_ids;
	vertex_ids.reserve	(mu_model.m_vertices.size());
	for (u32 vid = 0; vid < mu_model.m_vertices.size(); vid++)
		vertex_ids.emplace	(mu_model.m_vertices[vid], vid);

	// Build OGFs
	for (xrMU_Model::v_subdivs_it it=mu_model.m_subdivs.begin(); it!=mu_model.m_subdivs.end(); it++)
	{
		OGF*		pOGF	= new OGF ();
		b_material*	M		= &(pBuild->materials()[it->material]);	// and it's material
		R_ASSERT	(M);

		// MU face vertices are bit-copied from the model's vertex pool - enable
		// bit-exact O(1) dedup (the legacy path was O(V) per face)
		pOGF->bHashDedup	= true;

		try {
			// Common data
			pOGF->Sector		= 0;
			pOGF->material		= it->material;

			// Collect textures
			OGF_Texture			T;
			TRY					(T.name		= pBuild->textures()[M->surfidx].name);
			TRY					(T.pBuildSurface = &(pBuild->textures()[M->surfidx]));
			TRY					(pOGF->textures.push_back(T));

			// Collect faces & vertices
			try {
				xrMU_Model::v_faces_it	_beg	= mu_model.m_faces.begin() + it->start;
				xrMU_Model::v_faces_it	_end	= _beg + it->count;
				for (xrMU_Model::v_faces_it Fit =_beg; Fit!=_end; Fit++)
				{
					_face*	FF		= *Fit;
					R_ASSERT			(FF);
					OGF_AddFace( *pOGF, *FF, mu_model, vertex_ids ); 
				}
			} catch (...) {  clMsg("* ERROR: MU2OGF, model %s, *faces*",*(mu_model.m_name)); }
		} catch (...)
		{
			clMsg("* ERROR: MU2OGF, 1st part, model %s",*(mu_model.m_name));
		}

		try {
			pOGF->Optimize			();
		} catch (...)	{ clMsg	("* ERROR: MU2OGF, [optimize], model %s",*(mu_model.m_name)); }
		try {
			pOGF->CalcBounds		();
		} catch (...)	{ clMsg	("* ERROR: MU2OGF, [bounds], model %s",*(mu_model.m_name)); }
		try {
			pOGF->CalculateTB		();
		} catch (...)	{ clMsg	("* ERROR: MU2OGF, [calc_tb], model %s",*(mu_model.m_name)); }
		try {
			pOGF->MakeProgressive	(c_PM_MetricLimit_mu);
		} catch (...)	{ clMsg	("* ERROR: MU2OGF, [progressive], model %s",*(mu_model.m_name)); }
		try {
			pOGF->Stripify			();
		} catch (...)	{ clMsg	("* ERROR: MU2OGF, [stripify], model %s",*(mu_model.m_name)); }

		it->ogf		=	pOGF;
	}
}
