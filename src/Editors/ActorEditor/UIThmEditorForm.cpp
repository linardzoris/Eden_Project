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

UIThmEditorForm* UIThmEditorForm::Form = nullptr;

UIThmEditorForm::UIThmEditorForm()
{
}

UIThmEditorForm::~UIThmEditorForm()
{
}

// helper: combo fed by an xr_token table, keeps raw numeric value in *value
static bool TokenCombo(const char* label, u32& value, const xr_token* tokens)
{
	int current = -1;
	int count = 0;
	for (; tokens[count].name; ++count)
		if ((u32)tokens[count].id == value)
			current = count;
	if (current < 0)
		current = 0;

	bool changed = false;
	if (ImGui::BeginCombo(label, tokens[current].name))
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

// load any texture .thm by absolute path (may live outside the FS roots,
// so read via CRT and parse in memory instead of FS.r_open)
bool UIThmEditorForm::Load(LPCSTR path)
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

	if (!F.find_chunk(THM_CHUNK_TYPE))
	{
		ELog.Msg(mtError, "THM Editor: THM_CHUNK_TYPE missing, file: %s", path);
		return false;
	}
	u32 type = F.r_u32();
	if (type != ECustomThumbnail::ETTexture)
	{
		ELog.Msg(mtError, "THM Editor: not a texture thm (type %u), file: %s", type, path);
		return false;
	}

	m_Params.Clear();
	m_Params.Load(F);

	m_ThmType = type;
	m_FilePath = path;
	m_Dirty = false;
	ResetEdits();
	ELog.Msg(mtInformation, "THM Editor: loaded '%s'.", path);
	return true;
}

// write the loaded params back using the exact editor THM chunk layout
bool UIThmEditorForm::Save(LPCSTR path)
{
	// pull editable string fields back into the params
	m_Params.detail_name = m_DetailName;
	m_Params.bump_name = m_BumpName;
	m_Params.ext_normal_map_name = m_ExtNormalMap;

	CMemoryWriter F;
	F.open_chunk(THM_CHUNK_VERSION);
	F.w_u16(THM_TEXTURE_VERSION);
	F.close_chunk();

	F.open_chunk(THM_CHUNK_TYPE);
	F.w_u32(m_ThmType);
	F.close_chunk();

	m_Params.Save(F);

	if (!F.save_to(path))
	{
		ELog.Msg(mtError, "THM Editor: can't save '%s'.", path);
		return false;
	}
	m_FilePath = path;
	m_Dirty = false;
	ELog.Msg(mtInformation, "THM Editor: saved '%s'.", path);
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
	xr_string fn = m_FilePath;
	if (EFS.GetOpenName("$game_textures$", fn, false, 0, -1, "*.thm"))
		Load(fn.c_str());
}

void UIThmEditorForm::DoSave(bool save_as)
{
	if (!m_Dirty && !save_as && !m_FilePath.empty())
		return; // nothing to do

	xr_string fn = m_FilePath;
	if (save_as || fn.empty())
	{
		if (!EFS.GetSaveName("$game_textures$", fn, 0, -1, "*.thm"))
			return;
	}
	Save(fn.c_str());
}

void UIThmEditorForm::DrawBasic()
{
	if (ImGui::CollapsingHeader(Loc("Texture Parameters"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (TokenCombo(Loc("Type"), *(u32*)&m_Params.type, ae_ttype_token))
			m_Dirty = true;
		if (TokenCombo(Loc("Format"), *(u32*)&m_Params.fmt, ae_tfmt_token))
			m_Dirty = true;
		if (TokenCombo(Loc("Mip Filter"), m_Params.mip_filter, ae_tparam_token))
			m_Dirty = true;
		if (ImGui::InputInt(Loc("Width"), &m_Params.width))
			m_Dirty = true;
		if (ImGui::InputInt(Loc("Height"), &m_Params.height))
			m_Dirty = true;
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
			if (ImGui::Checkbox(Loc(f.label), &on))
			{
				m_Params.flags.set(f.flag, on);
				m_Dirty = true;
			}
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
			if (ImGui::Checkbox(Loc(f.label), &on))
			{
				m_Params.flags.set(f.flag, on);
				m_Dirty = true;
			}
		}
	}
}

void UIThmEditorForm::DrawColors()
{
	if (ImGui::CollapsingHeader(Loc("Border and Fade")))
	{
		if (ImGui::InputText(Loc("Border color"), m_BorderColor, sizeof(m_BorderColor), ImGuiInputTextFlags_CharsHexadecimal))
		{
			if (TextToColor(m_BorderColor, m_Params.border_color))
				m_Dirty = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(ARGB hex)");

		if (ImGui::InputText(Loc("Fade color"), m_FadeColor, sizeof(m_FadeColor), ImGuiInputTextFlags_CharsHexadecimal))
		{
			if (TextToColor(m_FadeColor, m_Params.fade_color))
				m_Dirty = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(ARGB hex)");

		if (ImGui::InputInt(Loc("Fade amount"), (int*)&m_Params.fade_amount))
			m_Dirty = true;
		int delay = m_Params.fade_delay;
		if (ImGui::SliderInt(Loc("Fade delay"), &delay, 0, 255))
		{
			m_Params.fade_delay = (u8)delay;
			m_Dirty = true;
		}
	}
}

void UIThmEditorForm::DrawDetail()
{
	if (ImGui::CollapsingHeader(Loc("Detail")))
	{
		if (ImGui::InputText(Loc("Detail name"), m_DetailName, sizeof(m_DetailName)))
		{
			m_Params.detail_name = m_DetailName;
			m_Dirty = true;
		}
		if (ImGui::DragFloat(Loc("Detail scale"), &m_Params.detail_scale, 0.01f, 0.0f, 100.0f, "%.3f"))
			m_Dirty = true;
	}
}

void UIThmEditorForm::DrawMaterialBump()
{
	if (ImGui::CollapsingHeader(Loc("Material and Bump"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (TokenCombo(Loc("Material"), *(u32*)&m_Params.material, ae_tmtl_token))
			m_Dirty = true;
		if (ImGui::DragFloat(Loc("Material weight"), &m_Params.material_weight, 0.01f, 0.0f, 1.0f, "%.3f"))
			m_Dirty = true;
		ImGui::Separator();
		if (TokenCombo(Loc("Bump mode"), *(u32*)&m_Params.bump_mode, ae_tbmode_token))
			m_Dirty = true;
		if (ImGui::DragFloat(Loc("Bump virtual height"), &m_Params.bump_virtual_height, 0.001f, 0.0f, 1.0f, "%.4f"))
			m_Dirty = true;
		if (ImGui::InputText(Loc("Bump name"), m_BumpName, sizeof(m_BumpName)))
		{
			m_Params.bump_name = m_BumpName;
			m_Dirty = true;
		}
		if (ImGui::InputText(Loc("Ext. normal map"), m_ExtNormalMap, sizeof(m_ExtNormalMap)))
		{
			m_Params.ext_normal_map_name = m_ExtNormalMap;
			m_Dirty = true;
		}
	}
}

void UIThmEditorForm::Draw()
{
	if (ImGui::Button(Loc("Load...")))
		DoLoad();
	ImGui::SameLine();
	if (ImGui::Button(Loc("Save")))
		DoSave(false);
	ImGui::SameLine();
	if (ImGui::Button(Loc("Save as ...")))
		DoSave(true);
	ImGui::SameLine();
	if (ImGui::Button(Loc("Close")))
		bOpen = false;
	ImGui::SameLine();
	if (ImGui::Button(Loc("Batch THM -> LTX")))
		ConvertThmToLtx();

	ImGui::SameLine();
	if (!m_FilePath.empty())
	{
		xr_string title = m_Dirty ? "* " : "";
		title += m_FilePath;
		ImGui::TextUnformatted(title.c_str());
	}
	else
		ImGui::TextDisabled(Loc("No file loaded"));

	if (!m_FilePath.empty())
	{
		ImGui::Separator();
		ImGui::BeginChild("thm_params", ImVec2(0, 0), ImGuiChildFlags_None);
		DrawBasic();
		DrawFlags();
		DrawColors();
		DrawDetail();
		DrawMaterialBump();
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
