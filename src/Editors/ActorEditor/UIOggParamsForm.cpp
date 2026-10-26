#include "stdafx.h"
#include "UIOggParamsForm.h"
#include "imgui.h"
#include "../xrEUI/EditorLocalization.h"
#include "../xrECore/Importer/AudioWav.h"
#include "../xrECore/Engine/ai_sounds.h"
#include <vorbis/vorbisfile.h>
#include <windows.h>

// the game type token table lives in xrECore/Engine/ai_sounds.cpp but is not
// exported from the DLL; duplicate it here for the UI only (sg_Undefined = 0)
static xr_token ae_game_type_token[] = {
	{ "undefined",            sg_Undefined },
	{ "Item picking up",      SOUND_TYPE_ITEM_PICKING_UP },
	{ "Item dropping",        SOUND_TYPE_ITEM_DROPPING },
	{ "Item taking",          SOUND_TYPE_ITEM_TAKING },
	{ "Item hiding",          SOUND_TYPE_ITEM_HIDING },
	{ "Item using",           SOUND_TYPE_ITEM_USING },
	{ "Weapon shooting",      SOUND_TYPE_WEAPON_SHOOTING },
	{ "Weapon empty clicking",SOUND_TYPE_WEAPON_EMPTY_CLICKING },
	{ "Weapon bullet hit",    SOUND_TYPE_WEAPON_BULLET_HIT },
	{ "Weapon recharging",    SOUND_TYPE_WEAPON_RECHARGING },
	{ "NPC dying",            SOUND_TYPE_MONSTER_DYING },
	{ "NPC injuring",         SOUND_TYPE_MONSTER_INJURING },
	{ "NPC step",             SOUND_TYPE_MONSTER_STEP },
	{ "NPC talking",          SOUND_TYPE_MONSTER_TALKING },
	{ "NPC attacking",        SOUND_TYPE_MONSTER_ATTACKING },
	{ "NPC eating",           SOUND_TYPE_MONSTER_EATING },
	{ "Anomaly idle",         SOUND_TYPE_ANOMALY_IDLE },
	{ "Object breaking",      SOUND_TYPE_WORLD_OBJECT_BREAKING },
	{ "Object colliding",     SOUND_TYPE_WORLD_OBJECT_COLLIDING },
	{ "Object exploding",     SOUND_TYPE_WORLD_OBJECT_EXPLODING },
	{ "World ambient",        SOUND_TYPE_WORLD_AMBIENT },
	{ 0, 0 }
};

// ---------------------------------------------------------------------------
// Memory-backed ov_callbacks over a raw byte buffer
// ---------------------------------------------------------------------------
struct OgMemFile
{
	const char* data = nullptr;
	size_t      size = 0;
	size_t      pos = 0;
};

static size_t OvReadFunc(void* ptr, size_t size, size_t nmemb, void* datasource)
{
	OgMemFile* f = (OgMemFile*)datasource;
	size_t want = size * nmemb;
	size_t avail = f->size - f->pos;
	if (want > avail)
		want = avail;
	memcpy(ptr, f->data + f->pos, want);
	f->pos += want;
	return size ? want / size : 0;
}

static int OvSeekFunc(void* datasource, ogg_int64_t offset, int whence)
{
	OgMemFile* f = (OgMemFile*)datasource;
	size_t target = f->pos;
	switch (whence)
	{
	case SEEK_SET: target = (size_t)offset; break;
	case SEEK_CUR: target = f->pos + (size_t)offset; break;
	case SEEK_END: target = f->size + (size_t)offset; break;
	}
	if (target > f->size)
		target = f->size;
	f->pos = target;
	return 0;
}

static long OvTellFunc(void* datasource)
{
	OgMemFile* f = (OgMemFile*)datasource;
	return (long)f->pos;
}

static int OvCloseFunc(void*)
{
	// memory is owned by the caller, nothing to release here
	return 0;
}

UIOggParamsForm* UIOggParamsForm::Form = nullptr;

UIOggParamsForm::UIOggParamsForm()
{
}

UIOggParamsForm::~UIOggParamsForm()
{
}

// open the ogg from memory and parse the X-Ray comment (v1/v2/v3 layouts)
bool UIOggParamsForm::Load(LPCSTR path)
{
	FILE* file = fopen(path, "rb");
	if (!file)
	{
		ELog.Msg(mtError, "OGG Params: can't open file '%s'.", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	long fsize = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (fsize <= 0)
	{
		ELog.Msg(mtError, "OGG Params: empty file '%s'.", path);
		fclose(file);
		return false;
	}

	m_FileData.resize((size_t)fsize);
	if (fread(m_FileData.data(), 1, (size_t)fsize, file) != (size_t)fsize)
	{
		fclose(file);
		ELog.Msg(mtError, "OGG Params: read failed '%s'.", path);
		return false;
	}
	fclose(file);

	OgMemFile mem;
	mem.data = m_FileData.data();
	mem.size = m_FileData.size();

	OggVorbis_File ovf;
	ov_callbacks ovc;
	ovc.read_func = OvReadFunc;
	ovc.seek_func = OvSeekFunc;
	ovc.close_func = OvCloseFunc;
	ovc.tell_func = OvTellFunc;

	if (ov_open_callbacks(&mem, &ovf, nullptr, 0, ovc) != 0)
	{
		ELog.Msg(mtError, "OGG Params: not a valid ogg file '%s'.", path);
		m_FileData.clear();
		return false;
	}

	vorbis_info* info = ov_info(&ovf, -1);
	if (!info)
	{
		ELog.Msg(mtError, "OGG Params: no stream info '%s'.", path);
		ov_clear(&ovf);
		m_FileData.clear();
		return false;
	}
	if (info->channels > 2)
	{
		ELog.Msg(mtError, "OGG Params: more than 2 channels not supported ('%s').", path);
		ov_clear(&ovf);
		m_FileData.clear();
		return false;
	}

	m_Rate = (float)info->rate;
	m_Channels = info->channels;
	s64 pcm_total = ov_pcm_total(&ovf, -1);
	m_Duration = (m_Rate > 0.0f) ? (float)((double)pcm_total / m_Rate) : 0.0f;

	// defaults, patched below when a valid comment exists
	m_MinDist = 1.0f;
	m_MaxDist = 100.0f;
	m_BaseVolume = 1.0f;
	m_MaxAIDist = 100.0f;
	m_GameType = 0;
	m_HadComment = false;
	m_CommentVersion = 0;

	vorbis_comment* ovm = ov_comment(&ovf, -1);
	if (ovm && ovm->comments > 0 && ovm->comment_lengths[0] >= 4)
	{
		IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);
		u32 vers = F.r_u32();
		if (vers == 0x0001)
		{
			m_MinDist = F.r_float();
			m_MaxDist = F.r_float();
			m_BaseVolume = 1.0f;
			m_GameType = F.r_u32();
			m_MaxAIDist = m_MaxDist;
			m_HadComment = true;
			m_CommentVersion = vers;
		}
		else if (vers == 0x0002)
		{
			m_MinDist = F.r_float();
			m_MaxDist = F.r_float();
			m_BaseVolume = F.r_float();
			m_GameType = F.r_u32();
			m_MaxAIDist = m_MaxDist;
			m_HadComment = true;
			m_CommentVersion = vers;
		}
		else if (vers == OGG_COMMENT_VERSION)
		{
			m_MinDist = F.r_float();
			m_MaxDist = F.r_float();
			m_BaseVolume = F.r_float();
			m_GameType = F.r_u32();
			m_MaxAIDist = F.r_float();
			m_HadComment = true;
			m_CommentVersion = vers;
		}
		else
		{
			ELog.Msg(mtWarning, "OGG Params: invalid comment version 0x%04X, defaults used.", vers);
		}
	}
	else
	{
		ELog.Msg(mtWarning, "OGG Params: missing comment, defaults used.");
	}

	ov_clear(&ovf);

	m_FilePath = path;
	m_Dirty = false;
	ELog.Msg(mtInformation, "OGG Params: loaded '%s'.", path);
	return true;
}

// decode the whole stream to s16 PCM and wrap it into a 16-bit WAV buffer
static bool DecodeToWav(const xr_vector<char>& ogg_data, xr_vector<char>& wav)
{
	OgMemFile mem;
	mem.data = ogg_data.data();
	mem.size = ogg_data.size();

	OggVorbis_File ovf;
	ov_callbacks ovc;
	ovc.read_func = OvReadFunc;
	ovc.seek_func = OvSeekFunc;
	ovc.close_func = OvCloseFunc;
	ovc.tell_func = OvTellFunc;

	if (ov_open_callbacks(&mem, &ovf, nullptr, 0, ovc) != 0)
		return false;

	vorbis_info* info = ov_info(&ovf, -1);
	if (!info || info->channels > 2)
	{
		ov_clear(&ovf);
		return false;
	}

	const int channels = info->channels;
	const long rate = info->rate;
	const int block_align = channels * 2;

	// decode into a raw PCM buffer
	xr_vector<char> pcm;
	char decode_buf[8192];
	int bitstream = 0;
	for (;;)
	{
		// 16-bit signed little-endian samples (host endian on x86)
		long bytes = ov_read(&ovf, decode_buf, (int)sizeof(decode_buf), 0, 1, 1, &bitstream);
		if (bytes == 0)
			break; // eof
		if (bytes < 0)
			continue; // hole in the stream, skip
		pcm.insert(pcm.end(), decode_buf, decode_buf + bytes);
	}
	ov_clear(&ovf);

	if (pcm.empty())
		return false;

	// assemble a canonical 16-bit PCM WAV file in memory
	u32 data_size = (u32)pcm.size();
	u32 riff_size = 36 + data_size;
	u32 byte_rate = rate * block_align;
	wav.clear();
	wav.reserve(44 + pcm.size());
	auto append = [&wav](const void* p, size_t n)
	{
		const char* c = (const char*)p;
		wav.insert(wav.end(), c, c + n);
	};
	append("RIFF", 4);
	append(&riff_size, 4);
	append("WAVE", 4);
	append("fmt ", 4);
	u32 fmt_size = 16;
	append(&fmt_size, 4);
	u16 format = 1;
	append(&format, 2);
	u16 ch = (u16)channels;
	append(&ch, 2);
	append(&rate, 4);
	append(&byte_rate, 4);
	u16 ba = (u16)block_align;
	append(&ba, 2);
	u16 bits = 16;
	append(&bits, 2);
	append("data", 4);
	append(&data_size, 4);
	append(pcm.data(), pcm.size());
	return true;
}

bool UIOggParamsForm::Save(LPCSTR path)
{
	if (m_FileData.empty())
		return false;

	// decode the kept stream to a temporary 16-bit wav
	xr_vector<char> wav;
	if (!DecodeToWav(m_FileData, wav))
	{
		ELog.Msg(mtError, "OGG Params: decode failed, file not saved.");
		return false;
	}

	// temp file next to the output (same volume, no cross-drive rename issues)
	xr_string tmp_wav = xr_string(path) + ".tmp.wav";
	FILE* file = fopen(tmp_wav.c_str(), "wb");
	if (!file)
	{
		ELog.Msg(mtError, "OGG Params: can't create temp file '%s'.", tmp_wav.c_str());
		return false;
	}
	size_t written = fwrite(wav.data(), 1, wav.size(), file);
	fclose(file);
	if (written != wav.size())
	{
		ELog.Msg(mtError, "OGG Params: temp write failed '%s'.", tmp_wav.c_str());
		remove(tmp_wav.c_str());
		return false;
	}

	// build the X-Ray comment blob (version 3, same layout as MakeGameSound)
	CMemoryWriter C;
	C.w_u32(OGG_COMMENT_VERSION);
	C.w_float(m_MinDist);
	C.w_float(m_MaxDist);
	C.w_float(m_BaseVolume);
	C.w_u32(m_GameType);
	C.w_float(m_MaxAIDist);

	// re-encode into a temp file first, then swap: the original must survive
	// a failed encode; ImportWav has no reliable return value, success =
	// output file exists and is not empty
	xr_string tmp_ogg = xr_string(path) + ".tmp.ogg";
	bool ok = XRay::Importer::Audio::ImportWav(shared_str(tmp_wav.c_str()), shared_str(tmp_ogg.c_str()), m_Quality, C.pointer(), C.size());
	if (ok)
	{
		HANDLE h = CreateFileA(tmp_ogg.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			ok = false;
		else
		{
			LARGE_INTEGER sz;
			ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0;
			CloseHandle(h);
		}
	}
	remove(tmp_wav.c_str());

	if (ok)
	{
		remove(path);
		if (rename(tmp_ogg.c_str(), path) != 0)
			ok = false;
	}
	else
	{
		remove(tmp_ogg.c_str());
	}

	if (!ok)
	{
		ELog.Msg(mtError, "OGG Params: re-encode failed '%s'.", path);
		return false;
	}

	m_FilePath = path;
	m_Dirty = false;
	ELog.Msg(mtInformation, "OGG Params: saved '%s'.", path);
	return true;
}

void UIOggParamsForm::DoLoad()
{
	xr_string fn = m_FilePath;
	if (EFS.GetOpenName("$game_sounds$", fn, false, 0, -1, "*.ogg"))
		Load(fn.c_str());
}

void UIOggParamsForm::DoSave(bool save_as)
{
	if (m_FileData.empty())
		return;

	xr_string fn = m_FilePath;
	if (save_as || fn.empty())
	{
		if (!EFS.GetSaveName("$game_sounds$", fn, 0, -1, "*.ogg"))
			return;
	}
	Save(fn.c_str());
}

void UIOggParamsForm::Draw()
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
	if (!m_FilePath.empty())
	{
		xr_string title = m_Dirty ? "* " : "";
		title += m_FilePath;
		ImGui::TextUnformatted(title.c_str());
	}
	else
		ImGui::TextDisabled(Loc("No file loaded"));

	if (m_FilePath.empty())
		return;

	ImGui::Separator();
	ImGui::Text("%s: %d Hz, %d ch, %.2f s | %s v%u",
		Loc("Stream info"),
		(int)m_Rate, m_Channels, m_Duration,
		m_HadComment ? Loc("Comment") : Loc("No comment"),
		m_CommentVersion);
	ImGui::Separator();

	if (ImGui::DragFloat(Loc("Min distance"), &m_MinDist, 0.1f, 0.1f, 10000.0f, "%.2f"))
		m_Dirty = true;
	if (ImGui::DragFloat(Loc("Max distance"), &m_MaxDist, 0.1f, 0.1f, 10000.0f, "%.2f"))
		m_Dirty = true;
	if (ImGui::DragFloat(Loc("Base volume"), &m_BaseVolume, 0.01f, 0.0f, 10.0f, "%.3f"))
		m_Dirty = true;
	if (ImGui::DragFloat(Loc("Max AI distance"), &m_MaxAIDist, 0.1f, 0.1f, 10000.0f, "%.2f"))
		m_Dirty = true;
	// game type shown as human-readable text (same mapping as the Sound Editor);
	// unknown values fall back to a hex display but stay unchanged until picked
	{
		int count = 0, current = -1;
		char unknown_label[32];
		for (; ae_game_type_token[count].name; ++count)
			if ((u32)ae_game_type_token[count].id == m_GameType)
				current = count;
		if (current < 0)
		{
			sprintf_s(unknown_label, sizeof(unknown_label), "Unknown (0x%08X)", m_GameType);
			if (ImGui::BeginCombo(Loc("Game type"), unknown_label))
				ImGui::EndCombo();
		}
		else if (ImGui::BeginCombo(Loc("Game type"), ae_game_type_token[current].name))
		{
			for (int i = 0; i < count; ++i)
			{
				bool selected = (i == current);
				if (ImGui::Selectable(ae_game_type_token[i].name, selected))
				{
					m_GameType = (u32)ae_game_type_token[i].id;
					m_Dirty = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (ImGui::SliderFloat(Loc("Encode quality"), &m_Quality, 0.5f, 1.0f, "%.2f"))
		m_Dirty = true;
}

void UIOggParamsForm::Update()
{
	if (!Form)
		return;

	if (Form->bOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(520, 380), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(420, 280));
		if (ImGui::Begin(Loc("OGG Sound Params"), &Form->bOpen))
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

void UIOggParamsForm::Show()
{
	if (Form)
	{
		Form->bOpen = true;
		return;
	}
	Form = new UIOggParamsForm();
}
