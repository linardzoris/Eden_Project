#include "stdafx.h"
#include "UIDbArchiveForm.h"
#include "imgui.h"
#include "../xrEUI/EditorLocalization.h"
#include "lzo/lzo1x.h"
#include <shlobj.h>
#include <filesystem>

namespace fs = std::filesystem;

UIDbArchiveForm* UIDbArchiveForm::Form = nullptr;

UIDbArchiveForm::UIDbArchiveForm()
{
}

UIDbArchiveForm::~UIDbArchiveForm()
{
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

struct DbRecord
{
	u32 size_real = 0;
	u32 size_compr = 0;
	u32 crc = 0;
	u32 ptr = 0;
	xr_string name;
};

// folder picker (no EFS equivalent for directories)
static bool BrowseFolder(char* buf, size_t sz)
{
	// BIF_NEWDIALOGSTYLE requires COM (STA) on the calling thread; without it
	// SHBrowseForFolderA hangs forever. Initialize here and balance on exit;
	// if the thread already uses another COM model, fall back to the
	// old-style dialog which does not need COM.
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool com_owned = (hr == S_OK || hr == S_FALSE);
	bool new_style = com_owned;

	BROWSEINFOA bi = {};
	char display[MAX_PATH] = "";
	bi.hwndOwner = GetForegroundWindow();
	bi.pszDisplayName = display;
	bi.lpszTitle = "Select folder";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | (new_style ? BIF_NEWDIALOGSTYLE : 0);
	LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
	bool ok = false;
	if (pidl)
	{
		char path[MAX_PATH] = "";
		if (SHGetPathFromIDListA(pidl, path))
		{
			strncpy_s(buf, sz, path, _TRUNCATE);
			ok = true;
		}
		CoTaskMemFree(pidl);
	}
	if (com_owned)
		CoUninitialize();
	return ok;
}

// replicate xrCompressor::write_file_header (xrCompress.cpp L138-164):
// u16 buffer_size | u32 size_real | u32 size_compr | u32 crc | name | u32 ptr
static void WriteDescRecord(CMemoryWriter& desc, LPCSTR name, u32 crc, u32 ptr, u32 size_real, u32 size_compr)
{
	u32 name_len = (u32)xr_strlen(name);
	u32 buffer_size = name_len + 4 * sizeof(u32);
	VERIFY(buffer_size <= 65535);
	desc.w_u16((u16)buffer_size);
	desc.w_u32(size_real);
	desc.w_u32(size_compr);
	desc.w_u32(crc);
	desc.w(name, name_len);
	desc.w_u32(ptr);
}

// parse top-level chunks of an archive file; fills records from the desc chunk.
// Uses IReader::open_chunk_iterator so LZHUF-compressed chunks (desc) are
// decompressed transparently by the engine-side chunk reader.
static bool ParseArchive(LPCSTR path, xr_vector<DbRecord>& recs, xr_string& entry_point, xr_string& err)
{
	FILE* f = fopen(path, "rb");
	if (!f)
	{
		err = "can't open archive file";
		return false;
	}
	_fseeki64(f, 0, SEEK_END);
	long long fsize = _ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
	xr_vector<char> buf((size_t)fsize);
	if (fsize > 0 && fread(buf.data(), 1, (size_t)fsize, f) != (size_t)fsize)
	{
		fclose(f);
		err = "archive read failed";
		return false;
	}
	fclose(f);

	IReader R(buf.data(), (u32)buf.size());

	bool desc_found = false;
	IReader* prev = nullptr;
	for (;;)
	{
		u32 id = 0;
		IReader* chunk = R.open_chunk_iterator(id, prev);
		prev = chunk;
		if (!chunk)
			break;

		if ((id & (~CFS_CompressMark)) == CFS_HeaderChunkID)
		{
			// ini header chunk: "[header]\r\nentry_point = xxx\r\n"
			xr_string text;
			text.assign((const char*)chunk->pointer(), (size_t)chunk->length());
			const char* p = strstr(text.c_str(), "entry_point");
			if (p)
			{
				p += 11;
				while (*p == ' ' || *p == '=')
					++p;
				entry_point = p;
				size_t nl = entry_point.find_first_of("\r\n");
				if (nl != xr_string::npos)
					entry_point.resize(nl);
			}
		}
		else if ((id & (~CFS_CompressMark)) == 1)
		{
			// file table chunk (decompressed automatically if marked)
			while (!chunk->eof())
			{
				u16 buffer_size = chunk->r_u16();
				if (buffer_size < 4 * sizeof(u32) + 1)
					break; // corrupted record
				DbRecord rec;
				rec.size_real = chunk->r_u32();
				rec.size_compr = chunk->r_u32();
				rec.crc = chunk->r_u32();
				u32 name_len = buffer_size - 4 * sizeof(u32);
				char name[1024];
				VERIFY(name_len < sizeof(name));
				chunk->r(name, name_len);
				name[name_len] = 0;
				rec.name = name;
				rec.ptr = chunk->r_u32();
				recs.push_back(rec);
			}
			desc_found = true;
		}
		// any other chunk (0 = data area): payload is addressed via record ptr
	}
	if (prev)
		prev->close();

	if (!desc_found)
	{
		err = "no desc chunk found (encrypted SoC archive or wrong file)";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// unpack
// ---------------------------------------------------------------------------

struct UnpackStats
{
	int extracted = 0;
	int dirs = 0;
	int raw = 0;
	int lzo = 0;
	int crc_mismatch = 0;
};

// parse + extract one archive into out_root\<entry_point>\ (parse errors are
// logged here). Record names are relative to the archive entry point (usually
// "gamedata"), so multiple archives merge into a single tree.
static bool UnpackArchive(LPCSTR archive_path, const fs::path& out_root, UnpackStats& st, xr_string& entry_point)
{
	xr_vector<DbRecord> recs;
	xr_string err;
	if (!ParseArchive(archive_path, recs, entry_point, err))
	{
		ELog.Msg(mtError, "DB Tool: %s (%s).", err.c_str(), archive_path);
		return false;
	}

	FILE* f = fopen(archive_path, "rb");
	if (!f)
	{
		ELog.Msg(mtError, "DB Tool: can't reopen archive '%s'.", archive_path);
		return false;
	}

	// merge target: out_root\<entry_point>\ ("gamedata" when the header is absent)
	xr_string ep = entry_point.empty() ? "gamedata" : entry_point;
	fs::path base = out_root / fs::path(ep.c_str());

	xr_vector<char> src_buf, dst_buf;
	for (const auto& rec : recs)
	{
		xr_string name = rec.name;
		std::transform(name.begin(), name.end(), name.begin(), [](char c) { return (char)tolower((u8)c); });
		if (name.find("..") != xr_string::npos)
			continue; // refuse path traversal entries

		fs::path target = base / fs::path(name.c_str());
		if (!name.empty() && name.back() == '\\')
		{
			std::error_code ec;
			fs::create_directories(target, ec);
			++st.dirs;
			continue;
		}

		if (rec.size_compr == 0 && rec.size_real == 0)
		{
			// empty file
			std::error_code ec;
			fs::create_directories(target.parent_path(), ec);
			FILE* out = fopen(target.string().c_str(), "wb");
			if (out)
				fclose(out);
			++st.extracted;
			continue;
		}

		if (_fseeki64(f, (long long)rec.ptr, SEEK_SET) != 0)
		{
			ELog.Msg(mtError, "DB Tool: bad offset for '%s'.", rec.name.c_str());
			continue;
		}
		src_buf.resize(rec.size_compr);
		if (fread(src_buf.data(), 1, rec.size_compr, f) != rec.size_compr)
		{
			ELog.Msg(mtError, "DB Tool: read failed for '%s'.", rec.name.c_str());
			continue;
		}

		const char* data = src_buf.data();
		if (rec.size_real != rec.size_compr)
		{
			dst_buf.resize(rec.size_real);
			rtc_decompress(dst_buf.data(), rec.size_real, src_buf.data(), rec.size_compr);
			data = dst_buf.data();
			++st.lzo;
		}
		else
		{
			++st.raw;
		}

		if (rec.size_real && crc32(data, rec.size_real) != rec.crc)
			++st.crc_mismatch;

		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		FILE* out = fopen(target.string().c_str(), "wb");
		if (!out)
		{
			ELog.Msg(mtError, "DB Tool: can't write '%s'.", target.string().c_str());
			continue;
		}
		if (rec.size_real)
			fwrite(data, 1, rec.size_real, out);
		fclose(out);
		++st.extracted;
	}
	fclose(f);
	return true;
}

// a file is treated as an archive when its extension starts with ".db" or is .xdb/.pack
static bool IsArchiveFile(const fs::path& p)
{
	std::wstring ext = p.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
	return ext.size() >= 3 && (ext.compare(0, 3, L".db") == 0 || ext == L".pack" || ext == L".xdb");
}

void UIDbArchiveForm::DoUnpack()
{
	if (m_BatchMode)
	{
		if (!m_BatchDir[0] || !m_UnpackDir[0])
		{
			ELog.Msg(mtError, "DB Tool: batch folder and output dir are required.");
			return;
		}
		fs::path root = fs::path(m_BatchDir);
		if (!fs::exists(root))
		{
			ELog.Msg(mtError, "DB Tool: batch folder doesn't exist '%s'.", m_BatchDir);
			return;
		}

		// collect all archive files in the folder
		xr_vector<fs::path> archives;
		std::error_code ec;
		if (m_Recursive)
		{
			for (auto& entry : fs::recursive_directory_iterator(root, ec))
				if (entry.is_regular_file() && IsArchiveFile(entry.path()))
					archives.push_back(entry.path());
		}
		else
		{
			for (auto& entry : fs::directory_iterator(root, ec))
				if (entry.is_regular_file() && IsArchiveFile(entry.path()))
					archives.push_back(entry.path());
		}
		if (archives.empty())
		{
			ELog.Msg(mtError, "DB Tool: no archive files found in '%s'.", m_BatchDir);
			return;
		}
		std::sort(archives.begin(), archives.end());

		fs::path out_root = fs::path(m_UnpackDir);
		UnpackStats total;
		int ok_archives = 0;
		xr_string entry_point;
		SPBItem* PB = UI->ProgressStart((int)archives.size(), "Unpacking archives");
		for (const auto& arch : archives)
		{
			// contents merge into out_root\<entry_point>\ across all archives
			UnpackStats st;
			if (UnpackArchive(arch.string().c_str(), out_root, st, entry_point))
			{
				++ok_archives;
				total.extracted += st.extracted;
				total.dirs += st.dirs;
				total.raw += st.raw;
				total.lzo += st.lzo;
				total.crc_mismatch += st.crc_mismatch;
			}
			PB->Inc();
		}
		UI->ProgressEnd(PB);

		string1024 stat;
		xr_sprintf(stat, "Batch unpack done: %d/%u archives, %d files (%d raw, %d lzo), %d dirs, %d crc mismatches.",
			ok_archives, (u32)archives.size(), total.extracted, total.raw, total.lzo, total.dirs, total.crc_mismatch);
		m_LastResult = stat;
		ELog.Msg(mtInformation, "DB Tool: %s", m_LastResult.c_str());
		return;
	}

	if (!m_ArchivePath[0] || !m_UnpackDir[0])
	{
		ELog.Msg(mtError, "DB Tool: archive path and output dir are required.");
		return;
	}

	UnpackStats st;
	xr_string entry_point;
	if (!UnpackArchive(m_ArchivePath, fs::path(m_UnpackDir), st, entry_point))
		return;

	string1024 stat;
	xr_sprintf(stat, "Unpack done: %d files (%d raw, %d lzo), %d dirs, %d crc mismatches, entry point '%s'.",
		st.extracted, st.raw, st.lzo, st.dirs, st.crc_mismatch, entry_point.c_str());
	m_LastResult = stat;
	ELog.Msg(mtInformation, "DB Tool: %s", m_LastResult.c_str());
}

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

void UIDbArchiveForm::DoPack()
{
	if (!m_PackSrcDir[0] || !m_PackOutPath[0])
	{
		ELog.Msg(mtError, "DB Tool: source dir and output file are required.");
		return;
	}
	fs::path src_root = fs::path(m_PackSrcDir);
	if (!fs::exists(src_root))
	{
		ELog.Msg(mtError, "DB Tool: source dir doesn't exist '%s'.", m_PackSrcDir);
		return;
	}

	// collect relative names (lowercase, backslashes), folders first
	xr_vector<xr_string> dirs, files;
	for (auto& entry : fs::recursive_directory_iterator(src_root))
	{
		std::string rel = fs::relative(entry.path(), src_root).string();
		std::transform(rel.begin(), rel.end(), rel.begin(), [](char c) { return (char)tolower((u8)c); });
		std::replace(rel.begin(), rel.end(), '/', '\\');
		if (entry.is_directory())
			dirs.push_back(xr_string((rel + "\\").c_str()));
		else
			files.push_back(xr_string(rel.c_str()));
	}
	std::sort(dirs.begin(), dirs.end());
	std::sort(files.begin(), files.end());

	static bool lzo_ready = false;
	if (!lzo_ready)
	{
		VERIFY(lzo_init() == LZO_E_OK);
		lzo_ready = true;
	}
	u8* lzo_heap = m_Compress ? (u8*)xr_alloc<u8>(LZO1X_999_MEM_COMPRESS) : nullptr;

	IWriter* pack = FS.w_open(m_PackOutPath);
	if (!pack)
	{
		ELog.Msg(mtError, "DB Tool: can't create '%s'.", m_PackOutPath);
		xr_free(lzo_heap);
		return;
	}

	// ini header chunk (same layout as xrCompress with a [header] config section)
	{
		CMemoryWriter W;
		string512 buff;
		xr_sprintf(buff, "[%s]", "header");
		W.w_string(buff);
		xr_sprintf(buff, "%s = %s", "entry_point", m_EntryPoint[0] ? m_EntryPoint : "gamedata");
		W.w_string(buff);
		pack->open_chunk(CFS_HeaderChunkID);
		pack->w(W.pointer(), W.size());
		pack->close_chunk();
	}

	CMemoryWriter fs_desc;
	xr_vector<char> file_buf;
	u32 raw_files = 0, lzo_files = 0;
	u64 bytes_src = 0, bytes_dst = 0;

	SPBItem* PB = UI->ProgressStart((int)(dirs.size() + files.size()), "Packing archive");

	// folder records carry no data (same as xrCompress PerformWork)
	for (const auto& d : dirs)
	{
		PB->Inc();
		WriteDescRecord(fs_desc, d.c_str(), 0, 0, 0, 0);
	}

	pack->open_chunk(0);
	for (const auto& rel : files)
	{
		PB->Inc();
		fs::path full = src_root / fs::path(rel.c_str());
		std::error_code ec;
		u64 fsize = fs::file_size(full, ec);
		if (ec)
		{
			ELog.Msg(mtError, "DB Tool: can't stat '%s'.", full.string().c_str());
			continue;
		}

		u32 crc = 0, size_real = 0, size_compr = 0;
		u32 ptr = (u32)pack->tell();
		if (fsize > 0)
		{
			FILE* in = fopen(full.string().c_str(), "rb");
			if (!in)
			{
				ELog.Msg(mtError, "DB Tool: can't read '%s'.", full.string().c_str());
				continue;
			}
			file_buf.resize((size_t)fsize);
			size_t rd = fread(file_buf.data(), 1, (size_t)fsize, in);
			fclose(in);
			if (rd != fsize)
			{
				ELog.Msg(mtError, "DB Tool: read failed '%s'.", full.string().c_str());
				continue;
			}
			size_real = (u32)fsize;
			crc = crc32(file_buf.data(), size_real);
			bytes_src += size_real;

			if (!m_Compress)
			{
				size_compr = size_real;
				pack->w(file_buf.data(), size_real);
				++raw_files;
			}
			else
			{
				u32 c_max = rtc_csize(size_real);
				u8* c_data = (u8*)xr_alloc<u8>(c_max);
				lzo_uint c_size = c_max;
				VERIFY(LZO_E_OK == lzo1x_999_compress((u8*)file_buf.data(), size_real, c_data, &c_size, lzo_heap));
				if ((u32)c_size + 16 >= size_real)
				{
					// compression was not effective - store raw (engine does the same)
					size_compr = size_real;
					pack->w(file_buf.data(), size_real);
					++raw_files;
				}
				else
				{
					size_compr = (u32)c_size;
					pack->w(c_data, size_compr);
					++lzo_files;
				}
				xr_free(c_data);
			}
			bytes_dst += size_compr;
		}
		WriteDescRecord(fs_desc, rel.c_str(), crc, ptr, size_real, size_compr);
	}
	pack->close_chunk();

	// desc chunk is LZHUF compressed by IWriter::w_chunk (CompressMark)
	pack->w_chunk(1 | CFS_CompressMark, fs_desc.pointer(), fs_desc.size());
	FS.w_close(pack);
	xr_free(lzo_heap);
	UI->ProgressEnd(PB);

	u64 out_size = 0;
	std::error_code sz_ec;
	out_size = fs::file_size(fs::path(m_PackOutPath), sz_ec);
	string1024 stat;
	xr_sprintf(stat, "Pack done: %d files (%d raw, %d lzo) + %d dirs, %uK -> %uK.",
		(int)files.size(), raw_files, lzo_files, (int)dirs.size(),
		(u32)(bytes_src / 1024), (u32)(out_size / 1024));
	m_LastResult = stat;
	ELog.Msg(mtInformation, "DB Tool: %s", m_LastResult.c_str());
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void UIDbArchiveForm::DrawUnpackTab()
{
	ImGui::RadioButton(Loc("Single archive"), &m_BatchMode, 0);
	ImGui::SameLine();
	ImGui::RadioButton(Loc("Folder (batch)"), &m_BatchMode, 1);
	ImGui::Separator();

	if (!m_BatchMode)
	{
		ImGui::InputText("##arch", m_ArchivePath, sizeof(m_ArchivePath));
		ImGui::SameLine();
		if (ImGui::Button((xr_string(Loc("Browse...")) + "##arch").c_str()))
		{
			xr_string fn = m_ArchivePath;
			if (EFS.GetOpenName("$fs_root$", fn, false, 0, -1, "*.db*;*.xdb;*.pack"))
				xr_strcpy(m_ArchivePath, sizeof(m_ArchivePath), fn.c_str());
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(Loc("Archive"));
	}
	else
	{
		ImGui::InputText("##batchdir", m_BatchDir, sizeof(m_BatchDir));
		ImGui::SameLine();
		if (ImGui::Button((xr_string(Loc("Browse...")) + "##batchdir").c_str()))
			BrowseFolder(m_BatchDir, sizeof(m_BatchDir));
		ImGui::SameLine();
		ImGui::TextUnformatted(Loc("Source folder"));

		ImGui::Checkbox(Loc("Recursive"), &m_Recursive);
	}

	ImGui::InputText("##outdir", m_UnpackDir, sizeof(m_UnpackDir));
	ImGui::SameLine();
	if (ImGui::Button((xr_string(Loc("Browse...")) + "##outdir").c_str()))
		BrowseFolder(m_UnpackDir, sizeof(m_UnpackDir));
	ImGui::SameLine();
	ImGui::TextUnformatted(Loc("Output folder"));

	if (ImGui::Button(m_BatchMode ? Loc("Unpack all") : Loc("Unpack")))
		DoUnpack();
	if (!m_LastResult.empty())
		ImGui::TextUnformatted(m_LastResult.c_str());
}

void UIDbArchiveForm::DrawPackTab()
{
	ImGui::InputText("##srcdir", m_PackSrcDir, sizeof(m_PackSrcDir));
	ImGui::SameLine();
	if (ImGui::Button((xr_string(Loc("Browse...")) + "##srcdir").c_str()))
		BrowseFolder(m_PackSrcDir, sizeof(m_PackSrcDir));
	ImGui::SameLine();
	ImGui::TextUnformatted(Loc("Source folder"));

	ImGui::InputText("##outpack", m_PackOutPath, sizeof(m_PackOutPath));
	ImGui::SameLine();
	if (ImGui::Button((xr_string(Loc("Browse...")) + "##outpack").c_str()))
	{
		xr_string fn = m_PackOutPath;
		if (EFS.GetSaveName("$fs_root$", fn, 0, -1, "*.db;*.xdb"))
			xr_strcpy(m_PackOutPath, sizeof(m_PackOutPath), fn.c_str());
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(Loc("Output archive"));

	ImGui::InputText(Loc("Entry point"), m_EntryPoint, sizeof(m_EntryPoint));
	ImGui::Checkbox(Loc("Compress (lzo)"), &m_Compress);

	if (ImGui::Button(Loc("Pack")))
		DoPack();
	if (!m_LastResult.empty())
		ImGui::TextUnformatted(m_LastResult.c_str());
}

void UIDbArchiveForm::Draw()
{
	if (ImGui::BeginTabBar("db_tool_tabs"))
	{
		if (ImGui::BeginTabItem(Loc("Unpack")))
		{
			m_UnpackTab = true;
			DrawUnpackTab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(Loc("Pack")))
		{
			m_UnpackTab = false;
			DrawPackTab();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void UIDbArchiveForm::Update()
{
	if (!Form)
		return;

	if (Form->bOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(680, 300), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(560, 220));
		if (ImGui::Begin(Loc("DB Archive Tool"), &Form->bOpen))
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

void UIDbArchiveForm::Show()
{
	if (Form)
	{
		Form->bOpen = true;
		return;
	}
	Form = new UIDbArchiveForm();
}
