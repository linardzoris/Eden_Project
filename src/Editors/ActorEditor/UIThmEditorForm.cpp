#include "stdafx.h"
#include "UIThmEditorForm.h"
#include "imgui.h"
#include "../xrEUI/EditorLocalization.h"

// The xr_token tables live in ETextureParams.cpp (xrECore) but are not
// exported from the DLL; duplicate them here under local names for the UI only
static xr_token ae_tparam_token[] = {
	{ "Advanced",  STextureParams::kMIPFilterAdvanced },
	{ "Point",     STextureParams::kMIPFilterPoint },
	{ "Box",       STextureParams::kMIPFilterBox },
	{ "Triangle",  STextureParams::kMIPFilterTriangle },
	{ "Quadratic", STextureParams::kMIPFilterQuadratic },
	{ "Cubic",     STextureParams::kMIPFilterCubic },
	{ "Catrom",    STextureParams::kMIPFilterCatrom },
	{ "Mitchell",  STextureParams::kMIPFilterMitchell },
	{ "Gaussian",  STextureParams::kMIPFilterGaussian },
	{ "Sinc",      STextureParams::kMIPFilterSinc },
	{ "Bessel",    STextureParams::kMIPFilterBessel },
	{ "Hanning",   STextureParams::kMIPFilterHanning },
	{ "Hamming",   STextureParams::kMIPFilterHamming },
	{ "Blackman",  STextureParams::kMIPFilterBlackman },
	{ "Kaiser",    STextureParams::kMIPFilterKaiser },
	{ 0, 0 }
};

static xr_token ae_ttype_token[] = {
	{ "2D Texture", STextureParams::ttImage },
	{ "Cube Map",   STextureParams::ttCubeMap },
	{ "Bump Map",   STextureParams::ttBumpMap },
	{ "Normal Map", STextureParams::ttNormalMap },
	{ "Terrain",    STextureParams::ttTerrain },
	{ 0, 0 }
};

static xr_token ae_tfmt_token[] = {
	{ "DXT1",                    STextureParams::tfDXT1 },
	{ "DXT1 Alpha",              STextureParams::tfADXT1 },
	{ "DXT3",                    STextureParams::tfDXT3 },
	{ "DXT5",                    STextureParams::tfDXT5 },
	{ "DBC7",                    STextureParams::tfBC7 },
	{ "16 bit (1:5:5:5)",        STextureParams::tf1555 },
	{ "16 bit (5:6:5)",          STextureParams::tf565 },
	{ "32 bit (8:8:8:8)",        STextureParams::tfRGBA },
	{ "8 bit (alpha)",           STextureParams::tfA8 },
	{ "8 bit (luminance)",       STextureParams::tfL8 },
	{ "16 bit (alpha:luminance)",STextureParams::tfA8L8 },
	{ 0, 0 }
};

static xr_token ae_tmtl_token[] = {
	{ "OrenNayar <-> Blin",   STextureParams::tmOrenNayar_Blin },
	{ "Blin <-> Phong",       STextureParams::tmBlin_Phong },
	{ "Phong <-> Metal",      STextureParams::tmPhong_Metal },
	{ "Metal <-> OrenNayar",  STextureParams::tmMetal_OrenNayar },
	{ "PBR",                  STextureParams::tmPBR_Material },
	{ 0, 0 }
};

static xr_token ae_tbmode_token[] = {
	{ "None",         STextureParams::tbmNone },
	{ "Use",          STextureParams::tbmUse },
	{ "Use parallax", STextureParams::tbmUseParallax },
	{ 0, 0 }
};

// same value as EThumbnailTexture.cpp (not exposed in a header)
#define THM_TEXTURE_VERSION 0x0012
// same values as EThumbnailObject.cpp (not exposed in a header)
#define THM_OBJECT_VERSION  0x0012
#define THM_CHUNK_OBJECTPARAM 0x0816

UIThmEditorForm* UIThmEditorForm::Form = nullptr;

UIThmEditorForm::UIThmEditorForm()
{
}

UIThmEditorForm::~UIThmEditorForm()
{
}

// ----------------------------------------------------------------------------
// mixed-selection widget helpers
//
// When several list entries are selected and a field's value differs between
// them ("mixed"), the widget frame is tinted orange; combos show "(mixed)" as
// the preview text. Any edit still applies to the whole selection.
// ----------------------------------------------------------------------------

// label shown by combos when the value is mixed across the selection
static const char* MixedLabel()
{
	return Loc("(mixed)");
}

// orange tint over the normal frame color: applied via PushStyleColor, the
// alpha keeps the underlying frame visible
static ImVec4 MixedFrameColor()
{
	ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
	c.x = c.x * 0.3f + 0.70f;
	c.y = c.y * 0.3f + 0.45f;
	c.z = c.z * 0.3f + 0.10f;
	c.w = 0.85f;
	return c;
}

struct MixedStyle
{
	MixedStyle(bool mixed)
	{
		if (mixed)
		{
			ImGui::PushStyleColor(ImGuiCol_FrameBg, MixedFrameColor());
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, MixedFrameColor());
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, MixedFrameColor());
			m_pops = 3;
		}
	}
	~MixedStyle()
	{
		if (m_pops)
			ImGui::PopStyleColor(m_pops);
	}
	int m_pops = 0;
};

// helper: combo fed by an xr_token table, keeps raw numeric value in *value;
// shows "(mixed)" as preview text when mixed
static bool TokenCombo(const char* label, u32& value, const xr_token* tokens, bool mixed = false)
{
	int current = -1;
	int count = 0;
	for (; tokens[count].name; ++count)
		if ((u32)tokens[count].id == value)
			current = count;
	if (current < 0)
		current = 0;

	bool changed = false;
	if (ImGui::BeginCombo(label, mixed ? MixedLabel() : tokens[current].name))
	{
		for (int i = 0; i < count; ++i)
		{
			bool selected = (i == current);
			if (ImGui::Selectable(tokens[i].name, selected))
			{
				value = (u32)tokens[i].id;
				changed = true;
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	return changed;
}

static bool InputIntX(const char* label, int* value, bool mixed)
{
	MixedStyle style(mixed);
	return ImGui::InputInt(label, value);
}

static bool DragFloatX(const char* label, float* value, float speed, float min_v, float max_v, const char* fmt, bool mixed)
{
	MixedStyle style(mixed);
	return ImGui::DragFloat(label, value, speed, min_v, max_v, fmt);
}

static bool SliderIntX(const char* label, int* value, int min_v, int max_v, bool mixed)
{
	MixedStyle style(mixed);
	return ImGui::SliderInt(label, value, min_v, max_v);
}

static bool InputTextX(const char* label, char* buf, size_t buf_size, bool mixed)
{
	MixedStyle style(mixed);
	return ImGui::InputText(label, buf, buf_size);
}

// checkbox with tri-state display: when mixed the frame is tinted and the
// checkmark shows the primary value; clicking toggles from the primary value
// and the result lands in *out
static bool CheckboxX(const char* label, bool value, bool mixed, bool& out)
{
	bool v = value;
	bool changed;
	{
		MixedStyle style(mixed);
		changed = ImGui::Checkbox(label, &v);
	}
	if (changed)
		out = v;
	return changed;
}

static void ColorToText(char* buf, size_t size, u32 color)
{
	sprintf_s(buf, size, "%08X", color);
}

static bool TextToColor(const char* text, u32& color)
{
	u32 v = 0;
	if (sscanf_s(text, "%x", &v) == 1)
	{
		color = v;
		return true;
	}
	return false;
}

// ----------------------------------------------------------------------------
// file IO
// ----------------------------------------------------------------------------

// chunk-header walk result used to classify a .thm file before parsing
struct ThmScan
{
	bool have_type = false;
	u32  type = 0;
	bool have_texparam = false;
	bool have_objparam = false;
	u32  faces = 0;
	u32  verts = 0;
	bool has_data = false;   // THM_CHUNK_DATA present (preview blob)
	u32  data_id = 0;        // chunk id incl. CFS_CompressMark
	xr_vector<char> blob;    // raw payload, round-tripped untouched
};

// iterate over all chunk headers; captures TYPE, the presence of the texture/
// object param chunks and the raw preview data blob (object thm preview must
// survive round-trips byte-identical)
static void ScanThmChunks(IReader& F, ThmScan& s)
{
	while (!F.eof())
	{
		u32 id = F.r_u32();
		u32 sz = F.r_u32();
		u32 pos = F.tell();
		switch (id & ~CFS_CompressMark)
		{
		case THM_CHUNK_TYPE:
			s.have_type = true;
			s.type = F.r_u32();
			break;
		case THM_CHUNK_TEXTUREPARAM:
			s.have_texparam = true;
			break;
		case THM_CHUNK_OBJECTPARAM:
			s.have_objparam = true;
			s.faces = F.r_u32();
			s.verts = F.r_u32();
			break;
		case THM_CHUNK_DATA:
			s.has_data = true;
			s.data_id = id;
			if (sz)
			{
				s.blob.resize(sz);
				F.r(s.blob.data(), sz);
			}
			break;
		}
		F.seek(pos + sz);
	}
}

// load any .thm by absolute path (may live outside the FS roots, so read via
// CRT and parse in memory instead of FS.r_open); kind auto-detected from the
// THM_CHUNK_TYPE payload (ETTexture / ETObject)
bool UIThmEditorForm::ParseThm(LPCSTR path, ThmEntry& out)
{
	FILE* file = fopen(path, "rb");
	if (!file)
	{
		ELog.Msg(mtError, "THM Editor: can't open file '%s'.", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	long fsize = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (fsize <= 0)
	{
		ELog.Msg(mtError, "THM Editor: empty file '%s'.", path);
		fclose(file);
		return false;
	}
	xr_vector<char> buf((size_t)fsize);
	if (fread(buf.data(), 1, (size_t)fsize, file) != (size_t)fsize)
	{
		fclose(file);
		ELog.Msg(mtError, "THM Editor: read failed '%s'.", path);
		return false;
	}
	fclose(file);

	IReader F(buf.data(), (u32)buf.size());

	u16 version = 0;
	if (!F.r_chunk(THM_CHUNK_VERSION, &version) || version != THM_TEXTURE_VERSION)
	{
		ELog.Msg(mtError, "THM Editor: unsupported version (0x%04X), file: %s", version, path);
		return false;
	}

	// classify the file
	IReader scan(buf.data(), (u32)buf.size());
	ThmScan s;
	ScanThmChunks(scan, s);

	if (!s.have_type)
	{
		ELog.Msg(mtError, "THM Editor: THM_CHUNK_TYPE missing, file: %s", path);
		return false;
	}

	out.path = path;
	out.dirty = false;
	out.thm_type = s.type;

	if (s.type == ECustomThumbnail::ETObject)
	{
		if (!s.have_objparam)
		{
			ELog.Msg(mtError, "THM Editor: THM_CHUNK_OBJECTPARAM missing, file: %s", path);
			return false;
		}
		out.kind = KindObject;
		out.params.Clear();
		out.face_count = s.faces;
		out.vertex_count = s.verts;
		out.has_data = s.has_data;
		out.data_id = s.data_id;
		out.blob = s.blob;
		return true;
	}

	if (s.type == ECustomThumbnail::ETTexture)
	{
		if (!s.have_texparam)
		{
			ELog.Msg(mtError, "THM Editor: THM_CHUNK_TEXTUREPARAM missing, file: %s", path);
			return false;
		}
		// re-parse from a fresh reader so chunk positions are untouched
		IReader F2(buf.data(), (u32)buf.size());
		F2.find_chunk(THM_CHUNK_TYPE); // sanity: already verified by the scan
		F2.r_u32();
		out.kind = KindTexture;
		out.face_count = 0;
		out.vertex_count = 0;
		out.has_data = false;
		out.data_id = 0;
		out.blob.clear();
		out.params.Clear();
		out.params.Load(F2);
		return true;
	}

	ELog.Msg(mtError, "THM Editor: unsupported thm type %u, file: %s", s.type, path);
	return false;
}

// write an entry back using the exact editor THM chunk layout of its kind
bool UIThmEditorForm::WriteThm(ThmEntry& e)
{
	CMemoryWriter F;
	F.open_chunk(THM_CHUNK_VERSION);
	F.w_u16(THM_TEXTURE_VERSION);
	F.close_chunk();

	if (e.kind == KindObject)
	{
		// preview blob goes back untouched, chunk id including the
		// compression flag preserved
		if (e.has_data && !e.blob.empty())
			F.w_chunk(e.data_id, e.blob.data(), (u32)e.blob.size());

		F.open_chunk(THM_CHUNK_TYPE);
		F.w_u32(e.thm_type);
		F.close_chunk();

		F.open_chunk(THM_CHUNK_OBJECTPARAM);
		F.w_u32(e.face_count);
		F.w_u32(e.vertex_count);
		F.close_chunk();
	}
	else
	{
		F.open_chunk(THM_CHUNK_TYPE);
		F.w_u32(e.thm_type);
		F.close_chunk();

		e.params.Save(F);
	}

	if (!F.save_to(e.path.c_str()))
	{
		ELog.Msg(mtError, "THM Editor: can't save '%s'.", e.path.c_str());
		return false;
	}
	e.dirty = false;
	ELog.Msg(mtInformation, "THM Editor: saved '%s'.", e.path.c_str());
	return true;
}

void UIThmEditorForm::ResetEdits()
{
	xr_strcpy(m_DetailName, sizeof(m_DetailName), m_Params.detail_name.c_str());
	xr_strcpy(m_BumpName, sizeof(m_BumpName), m_Params.bump_name.c_str());
	xr_strcpy(m_ExtNormalMap, sizeof(m_ExtNormalMap), m_Params.ext_normal_map_name.c_str());
	ColorToText(m_BorderColor, sizeof(m_BorderColor), m_Params.border_color);
	ColorToText(m_FadeColor, sizeof(m_FadeColor), m_Params.fade_color);
}

void UIThmEditorForm::DoLoad()
{
	xr_string fn;
	// multi-select open dialog: result is a comma-separated list of paths
	if (!EFS.GetOpenName("$game_textures$", fn, true, 0, -1, "*.thm"))
		return;

	SStringVec lst;
	_SequenceToList(lst, fn.c_str());

	m_Entries.clear();
	m_Selection.clear();
	m_Anchor = -1;

	u32 loaded = 0;
	for (const auto& s : lst)
	{
		xr_string path = FS.fix_path(s);
		ThmEntry e;
		if (ParseThm(path.c_str(), e))
		{
			m_Entries.push_back(e);
			++loaded;
		}
	}

	if (!m_Entries.empty())
		SelectOnly(0);
	ELog.Msg(mtInformation, "THM Editor: imported %u of %u thm files.", loaded, (u32)lst.size());
}

// batch-import object .thm files from the $objects$ tree; appends to the
// current list (kind is auto-detected per file)
void UIThmEditorForm::DoLoadObject()
{
	xr_string fn;
	if (!EFS.GetOpenName("$objects$", fn, true, 0, -1, "*.thm"))
		return;

	SStringVec lst;
	_SequenceToList(lst, fn.c_str());

	u32 loaded = 0;
	for (const auto& s : lst)
	{
		xr_string path = FS.fix_path(s);
		ThmEntry e;
		if (ParseThm(path.c_str(), e))
		{
			m_Entries.push_back(e);
			++loaded;
		}
	}

	if (loaded)
	{
		// select the newly imported block
		m_Selection.clear();
		m_Anchor = -1;
		for (int i = (int)m_Entries.size() - (int)loaded; i < (int)m_Entries.size(); ++i)
			m_Selection.push_back(i);
	}
	ELog.Msg(mtInformation, "THM Editor: imported %u of %u object thm files.", loaded, (u32)lst.size());
}

void UIThmEditorForm::DoSaveSelected(bool save_as)
{
	if (!HasSelection())
		return;

	if (save_as)
	{
		if (m_Selection.size() != 1)
			return; // save-as needs exactly one target
		ThmEntry& e = m_Entries[m_Selection[0]];
		xr_string fn = e.path;
		if (!EFS.GetSaveName("$game_textures$", fn, 0, -1, "*.thm"))
			return;
		e.path = fn;
		WriteThm(e);
		return;
	}

	u32 saved = 0;
	for (int i : m_Selection)
		if (m_Entries[i].dirty && WriteThm(m_Entries[i]))
			++saved;
	if (!saved)
		ELog.Msg(mtInformation, "THM Editor: nothing to save in selection.");
}

void UIThmEditorForm::DoSaveAll()
{
	u32 saved = 0;
	for (ThmEntry& e : m_Entries)
		if (e.dirty && WriteThm(e))
			++saved;
	if (!saved)
		ELog.Msg(mtInformation, "THM Editor: nothing to save.");
	else
		ELog.Msg(mtInformation, "THM Editor: saved %u files.", saved);
}

// ----------------------------------------------------------------------------
// selection
// ----------------------------------------------------------------------------

bool UIThmEditorForm::Selected(int i) const
{
	for (int s : m_Selection)
		if (s == i)
			return true;
	return false;
}

void UIThmEditorForm::SelectOnly(int i)
{
	m_Selection.clear();
	m_Selection.push_back(i);
	m_Anchor = i;
}

void UIThmEditorForm::ToggleSelect(int i)
{
	for (size_t k = 0; k < m_Selection.size(); ++k)
	{
		if (m_Selection[k] == i)
		{
			m_Selection.erase(m_Selection.begin() + k);
			return; // keep the anchor so shift+click can re-add
		}
	}
	// insert keeping the index list sorted
	size_t pos = 0;
	while (pos < m_Selection.size() && m_Selection[pos] < i)
		++pos;
	m_Selection.insert(m_Selection.begin() + pos, i);
	m_Anchor = i;
}

void UIThmEditorForm::RangeSelect(int i)
{
	int a = (m_Anchor >= 0) ? m_Anchor : i;
	int lo = (a < i) ? a : i;
	int hi = (a > i) ? a : i;
	m_Selection.clear();
	for (int j = lo; j <= hi; ++j)
		m_Selection.push_back(j);
}

// ----------------------------------------------------------------------------
// UI
// ----------------------------------------------------------------------------

void UIThmEditorForm::DrawFileList()
{
	if (m_Entries.empty())
		return;

	ImGui::Separator();
	ImGui::BeginChild("thm_file_list", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 6.0f), ImGuiChildFlags_FrameStyle);
	for (int i = 0; i < (int)m_Entries.size(); ++i)
	{
		const xr_string& p = m_Entries[i].path;
		size_t slash = p.find_last_of("\\/");
		xr_string name = p.substr(slash + 1); // npos+1 == 0, keeps the full path
		if (m_Entries[i].kind == KindObject)
			name += " [obj]";
		if (m_Entries[i].dirty)
			name += " *";

		bool sel = Selected(i);
		if (ImGui::Selectable(name.c_str(), sel))
		{
			ImGuiIO& io = ImGui::GetIO();
			if (io.KeyShift)
				RangeSelect(i);
			else if (io.KeyCtrl)
				ToggleSelect(i);
			else
				SelectOnly(i);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", p.c_str());
	}
	ImGui::EndChild();

	u32 dirty = 0;
	for (const ThmEntry& e : m_Entries)
		if (e.dirty)
			++dirty;
	ImGui::Text("%s: %u, %s: %u", Loc("Files"), (u32)m_Entries.size(), Loc("Modified"), dirty);
}

void UIThmEditorForm::DrawBasic()
{
	if (ImGui::CollapsingHeader(Loc("Texture Parameters"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		u32 v = m_Params.type;
		if (TokenCombo(Loc("Type"), v, ae_ttype_token, FieldMixed([](const STextureParams& p) { return p.type; })))
			ForSelected([&](STextureParams& p) { p.type = (STextureParams::ETType)v; });

		v = m_Params.fmt;
		if (TokenCombo(Loc("Format"), v, ae_tfmt_token, FieldMixed([](const STextureParams& p) { return p.fmt; })))
			ForSelected([&](STextureParams& p) { p.fmt = (STextureParams::ETFormat)v; });

		v = m_Params.mip_filter;
		if (TokenCombo(Loc("Mip Filter"), v, ae_tparam_token, FieldMixed([](const STextureParams& p) { return p.mip_filter; })))
			ForSelected([&](STextureParams& p) { p.mip_filter = v; });

		if (InputIntX(Loc("Width"), &m_Params.width, FieldMixed([](const STextureParams& p) { return p.width; })))
			ForSelected([&](STextureParams& p) { p.width = m_Params.width; });
		if (InputIntX(Loc("Height"), &m_Params.height, FieldMixed([](const STextureParams& p) { return p.height; })))
			ForSelected([&](STextureParams& p) { p.height = m_Params.height; });
	}
}

void UIThmEditorForm::DrawFlags()
{
	if (ImGui::CollapsingHeader(Loc("Flags"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		struct FlagEntry { const char* label; u32 flag; };
		const FlagEntry flags[] = {
			{ "Generate Mip Maps",  STextureParams::flGenerateMipMaps },
			{ "Binary Alpha",       STextureParams::flBinaryAlpha },
			{ "Alpha Border",       STextureParams::flAlphaBorder },
			{ "Color Border",       STextureParams::flColorBorder },
			{ "Fade To Color",      STextureParams::flFadeToColor },
			{ "Fade To Alpha",      STextureParams::flFadeToAlpha },
			{ "Dither Color",       STextureParams::flDitherColor },
			{ "Dither Each MIP",    STextureParams::flDitherEachMIPLevel },
		};
		for (const auto& f : flags)
		{
			bool on = m_Params.flags.is(f.flag);
			bool mixed = FieldMixed([f](const STextureParams& p) { return !!p.flags.is(f.flag); });
			bool newval = false;
			if (CheckboxX(Loc(f.label), on, mixed, newval))
				ForSelected([&](STextureParams& p) { p.flags.set(f.flag, newval); });
		}
		ImGui::Separator();
		const FlagEntry advanced[] = {
			{ "Diffuse Detail",   STextureParams::flDiffuseDetail },
			{ "Implicit Lighted", STextureParams::flImplicitLighted },
			{ "Has Alpha",        STextureParams::flHasAlpha },
			{ "Bump Detail",      STextureParams::flBumpDetail },
		};
		for (const auto& f : advanced)
		{
			bool on = m_Params.flags.is(f.flag);
			bool mixed = FieldMixed([f](const STextureParams& p) { return !!p.flags.is(f.flag); });
			bool newval = false;
			if (CheckboxX(Loc(f.label), on, mixed, newval))
				ForSelected([&](STextureParams& p) { p.flags.set(f.flag, newval); });
		}
	}
}

void UIThmEditorForm::DrawColors()
{
	if (ImGui::CollapsingHeader(Loc("Border and Fade")))
	{
		if (InputTextX(Loc("Border color"), m_BorderColor, sizeof(m_BorderColor), FieldMixed([](const STextureParams& p) { return p.border_color; })))
		{
			u32 color = m_Params.border_color;
			if (TextToColor(m_BorderColor, color))
				ForSelected([&](STextureParams& p) { p.border_color = color; });
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(ARGB hex)");

		if (InputTextX(Loc("Fade color"), m_FadeColor, sizeof(m_FadeColor), FieldMixed([](const STextureParams& p) { return p.fade_color; })))
		{
			u32 color = m_Params.fade_color;
			if (TextToColor(m_FadeColor, color))
				ForSelected([&](STextureParams& p) { p.fade_color = color; });
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(ARGB hex)");

		if (InputIntX(Loc("Fade amount"), (int*)&m_Params.fade_amount, FieldMixed([](const STextureParams& p) { return p.fade_amount; })))
			ForSelected([&](STextureParams& p) { p.fade_amount = m_Params.fade_amount; });

		int delay = m_Params.fade_delay;
		if (SliderIntX(Loc("Fade delay"), &delay, 0, 255, FieldMixed([](const STextureParams& p) { return (int)p.fade_delay; })))
		{
			u8 v = (u8)delay;
			ForSelected([&](STextureParams& p) { p.fade_delay = v; });
		}
	}
}

void UIThmEditorForm::DrawDetail()
{
	if (ImGui::CollapsingHeader(Loc("Detail")))
	{
		if (InputTextX(Loc("Detail name"), m_DetailName, sizeof(m_DetailName), FieldMixedStr([](const STextureParams& p) { return p.detail_name; })))
			ForSelected([&](STextureParams& p) { p.detail_name = m_DetailName; });

		if (DragFloatX(Loc("Detail scale"), &m_Params.detail_scale, 0.01f, 0.0f, 100.0f, "%.3f", FieldMixed([](const STextureParams& p) { return p.detail_scale; })))
			ForSelected([&](STextureParams& p) { p.detail_scale = m_Params.detail_scale; });
	}
}

void UIThmEditorForm::DrawMaterialBump()
{
	if (ImGui::CollapsingHeader(Loc("Material and Bump"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		u32 v = m_Params.material;
		if (TokenCombo(Loc("Material"), v, ae_tmtl_token, FieldMixed([](const STextureParams& p) { return p.material; })))
			ForSelected([&](STextureParams& p) { p.material = (STextureParams::ETMaterial)v; });

		if (DragFloatX(Loc("Material weight"), &m_Params.material_weight, 0.01f, 0.0f, 1.0f, "%.3f", FieldMixed([](const STextureParams& p) { return p.material_weight; })))
			ForSelected([&](STextureParams& p) { p.material_weight = m_Params.material_weight; });

		ImGui::Separator();

		v = m_Params.bump_mode;
		if (TokenCombo(Loc("Bump mode"), v, ae_tbmode_token, FieldMixed([](const STextureParams& p) { return p.bump_mode; })))
			ForSelected([&](STextureParams& p) { p.bump_mode = (STextureParams::ETBumpMode)v; });

		if (DragFloatX(Loc("Bump virtual height"), &m_Params.bump_virtual_height, 0.001f, 0.0f, 1.0f, "%.4f", FieldMixed([](const STextureParams& p) { return p.bump_virtual_height; })))
			ForSelected([&](STextureParams& p) { p.bump_virtual_height = m_Params.bump_virtual_height; });

		if (InputTextX(Loc("Bump name"), m_BumpName, sizeof(m_BumpName), FieldMixedStr([](const STextureParams& p) { return p.bump_name; })))
			ForSelected([&](STextureParams& p) { p.bump_name = m_BumpName; });

		if (InputTextX(Loc("Ext. normal map"), m_ExtNormalMap, sizeof(m_ExtNormalMap), FieldMixedStr([](const STextureParams& p) { return p.ext_normal_map_name; })))
			ForSelected([&](STextureParams& p) { p.ext_normal_map_name = m_ExtNormalMap; });
	}
}

// object thumbnail panel: face/vertex counters from THM_CHUNK_OBJECTPARAM;
// the preview image is round-tripped untouched, not editable here
void UIThmEditorForm::DrawObject()
{
	if (ImGui::CollapsingHeader(Loc("Object Parameters"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (InputIntX(Loc("Faces"), &m_FaceCount, FieldMixedEntry([](const ThmEntry& e) { return (int)e.face_count; })))
		{
			int v = m_FaceCount;
			ForSelectedKind(KindObject, [&](ThmEntry& e) { e.face_count = (u32)v; });
		}
		if (InputIntX(Loc("Vertexes"), &m_VertexCount, FieldMixedEntry([](const ThmEntry& e) { return (int)e.vertex_count; })))
		{
			int v = m_VertexCount;
			ForSelectedKind(KindObject, [&](ThmEntry& e) { e.vertex_count = (u32)v; });
		}
		ImGui::Separator();
		ImGui::TextDisabled("%s", Loc("Object preview image is kept unchanged"));
	}
}

void UIThmEditorForm::Draw()
{
	// sync the edit mirror from the primary selection before widgets run
	int prim = PrimaryIndex();
	if (prim >= 0)
	{
		const ThmEntry& primary = m_Entries[prim];
		if (primary.kind == KindObject)
		{
			m_FaceCount = (int)primary.face_count;
			m_VertexCount = (int)primary.vertex_count;
		}
		else
		{
			m_Params = primary.params;
			ResetEdits();
		}
	}

	if (ImGui::Button(Loc("Load...")))
		DoLoad();
	ImGui::SameLine();
	if (ImGui::Button(Loc("Import Object THM...")))
		DoLoadObject();
	ImGui::SameLine();

	ImGui::BeginDisabled(!HasSelection());
	if (ImGui::Button(Loc("Save")))
		DoSaveSelected(false);
	ImGui::EndDisabled();
	ImGui::SameLine();

	{
		bool can_save_as = (m_Selection.size() == 1);
		if (!can_save_as)
			ImGui::BeginDisabled();
		if (ImGui::Button(Loc("Save as ...")))
			DoSaveSelected(true);
		if (!can_save_as)
			ImGui::EndDisabled();
	}
	ImGui::SameLine();

	// lights up as soon as the list exists
	{
		bool can_save_all = !m_Entries.empty();
		if (!can_save_all)
			ImGui::BeginDisabled();
		if (ImGui::Button(Loc("Save All")))
			DoSaveAll();
		if (!can_save_all)
			ImGui::EndDisabled();
	}
	ImGui::SameLine();

	if (ImGui::Button(Loc("Close")))
		bOpen = false;
	ImGui::SameLine();
	if (ImGui::Button(Loc("Batch THM -> LTX")))
		ConvertThmToLtx();
	ImGui::SameLine();
	if (ImGui::Button(Loc("Batch LTX -> THM")))
		ConvertLtxToThm();

	if (!m_Entries.empty())
	{
		xr_string title;
		if (m_Selection.size() == 1)
		{
			const ThmEntry& e = m_Entries[m_Selection[0]];
			title = e.dirty ? "* " : "";
			title += e.path;
			if (e.kind == KindObject)
				title += "  [obj]";
		}
		else
		{
			char buf[128];
			sprintf_s(buf, sizeof(buf), "%s: %d", Loc("Selected"), (int)m_Selection.size());
			title = buf;
		}
		ImGui::TextUnformatted(title.c_str());
	}
	else
		ImGui::TextDisabled(Loc("No file loaded"));

	DrawFileList();

	if (prim >= 0)
	{
		ImGui::Separator();
		ImGui::BeginChild("thm_params", ImVec2(0, 0), ImGuiChildFlags_None);
		if (SelectionAll(KindObject))
			DrawObject();
		else if (SelectionAll(KindTexture))
		{
			DrawBasic();
			DrawFlags();
			DrawColors();
			DrawDetail();
			DrawMaterialBump();
		}
		else
			ImGui::TextDisabled("%s", Loc("Mixed texture and object selection"));
		ImGui::EndChild();
	}
}

void UIThmEditorForm::Update()
{
	if (!Form)
		return;

	if (Form->bOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(520, 760), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(420, 400));
		if (ImGui::Begin(Loc("THM Editor"), &Form->bOpen))
		{
			Form->Draw();
		}
		else
		{
			Form->bOpen = false;
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}
	else
	{
		xr_delete(Form);
	}
}

void UIThmEditorForm::Show()
{
	if (Form)
	{
		Form->bOpen = true;
		return;
	}
	Form = new UIThmEditorForm();
}
