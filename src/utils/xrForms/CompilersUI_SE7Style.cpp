#include <imgui.h>

#include "../../xrCore/xrCore.h"
#include "CompilersUI.h"
#include "cl_log.h"
#include <timeapi.h>

//Ex: 25, 200, 50, 255 -> 0.0980392, 0.784314, 0.196078, 1
#define RGBAColor(r,g,b,a) r/(float)255, g/(float)255, b/(float)255, a/(float)255
extern size_t GetHeapMemory();

bool ShowMainUI = true;

extern CompilersMode gCompilerMode;

// Shows a descriptive help tooltip while the previously submitted item is hovered.
static void HoverHelp(const char* desc)
{
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
		ImGui::SetTooltip("%s", desc);
}

void InitializeUIData()
{
	string_path LevelsDir = {};
	FS.update_path(LevelsDir, "$game_levels$", "");

	for (const xr_path& Dir : std::filesystem::directory_iterator{ LevelsDir })
	{
		if (!std::filesystem::is_directory(Dir))
			continue;

		auto& LevelInfo = gCompilerMode.Files.emplace_back();
		LevelInfo.Name = Dir.xfilename();

	}
}

static bool autoScroll = true;
static bool hideLogSection = true;
static bool ResizeMaximal = false;


void DrawCompilerConfig();
void DrawAIConfig();
void DrawDOConfig();
void DrawLCConfig();


void RenderMainUI()
{
	int Size[2] = {};
 	SDL_GetWindowSize(g_AppInfo.Window, &Size[0], &Size[1]);


	ImGui::SetNextWindowPos({ 0, 0 });
	ImGui::SetNextWindowSize({ (float)Size[0], (float)Size[1] });
	

	if (!ShowMainUI) 
	{
		RenderCompilerUI(Size[0], Size[1]);
		return;
	}

	if ( Size[0] != 1000 || Size[1] != 650)
	{
		SDL_SetWindowSize(g_AppInfo.Window, 1000, 650);
	}

	if (ImGui::Begin("MainForm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus))
	{
		ImVec2 ListBoxSize = { float(Size[0] - 20), float ( Size[1] - 115) };
		if (ImGui::BeginTable("##Levels", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ListBoxSize))
		{
			// 
			ImGui::TableSetupColumn("Levels");
			ImGui::TableSetupColumn("Settings");
			ImGui::TableSetupColumn("xrLC");
			ImGui::TableSetupColumn("xrAI");
			ImGui::TableSetupColumn("xrDO");

			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			
			ImVec2 ListBoxSize2 = { 200, float(Size[1] - 155) };
			if (  ImGui::BeginTable("##Levels", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ListBoxSize2)  )
			{
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Prop");
				ImGui::TableHeadersRow();

				size_t Iter = 1;
				for (auto& [File, Selected] : gCompilerMode.Files)
				{
					ImGui::TableNextColumn();
				xr_string U8Str = Platform::ANSI_TO_UTF8(File);
				ImGui::Selectable(U8Str.c_str());
				HoverHelp("Level folder inside gamedata\\levels that the enabled compiler\n"
					"modules will process.\nThe 'Prop' checkbox marks this level for the next Run Compiler.");

				ImGui::TableNextColumn();
				ImGui::Checkbox(("##check" + File).c_str(), &Selected);
				HoverHelp("Mark this level to be compiled when 'Run Compiler' is pressed.");
				Iter++;

					if (Iter < gCompilerMode.Files.size())
					{
						ImGui::TableNextRow();
					}
				}
				ImGui::EndTable();
			}

			ImGui::TableNextColumn();
			
			DrawCompilerConfig();
			
			ImGui::TableNextColumn();
			
			DrawLCConfig();

			ImGui::TableNextColumn();

			DrawAIConfig();

			ImGui::TableNextColumn();

			DrawDOConfig();

			ImGui::EndTable();  
		}
	}
	 
	auto BSize = ImGui::GetContentRegionAvail();

	if (ImGui::Button("Run Compiler", { BSize.x, 50 }))
	{
		GetIterationData().clear();

		bool isReady = false;
		if (gCompilerMode.LC || gCompilerMode.DO)
		{
			isReady = true;
		}

		if (gCompilerMode.AI)
		{

			if (gCompilerMode.AI_BuildLevel)
			{
				isReady = true;
			}

			if (gCompilerMode.AI_BuildSpawn)
			{
				isReady = true;
			}
		}

		if (isReady)
		{
			for (auto& FILE : gCompilerMode.Files)
			{
				if (FILE.Select)
				{
					Msg("Level For Building : %s", FILE.Name);
					break;
				}
			}

			ShowMainUI = false;
			extern void StartCompile();
			StartCompile();
		}

	}
	 
	if (true)
	{
		ImGui::Separator();

		if (ImGui::Button(autoScroll ? "Disable Auto-Scroll" : "Enable Auto-Scroll"))
		{
			autoScroll = !autoScroll;
		}

		ImGui::SameLine();

		if (ImGui::Button(!ResizeMaximal ? "Maximal resize" : "Minimal resize"))
		{
			ResizeMaximal = !ResizeMaximal;
		}

		ImGui::SameLine();
		 
		ImGui::TextColored(ImVec4{ 0, 0.9, 0, 1 }, "Memory: %u mb", GetHeapMemory() / 1024 / 1024);

		ImGui::SameLine();

		ImGui::Checkbox("ShowMain", &ShowMainUI);
	}
 
	ImGui::End();


	/*
		Якорный переход просчитывается по порядку.
			Первый yButton будет связан с первым yRow

		yButton - может иметь любое название.
	*/

	// для включения
	//    \/
	if (false && ImGui::Begin("DemoWindow##dw02", 0, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
	{
///////////////////////////////////////////////
#define yButton(text) ++yStep; if (ImGui::Button(text)) colClick = yStep;
#define yRow()   ++yStep; ImGui::TableNextRow(); ImGui::TableNextColumn(); if (yStep == colClick) { ImGui::SetScrollHereY(0.0f); colClick = 0; }
///////////////////////////////////////////////

		ImVec2 winSize = ImGui::GetWindowSize();
		//
		int yStep = 0;
		int colClick = 0;

		ImGui::BeginGroup();
			yButton("Compiler");
			yButton("xrLC");
			yButton("xrAI");
			yButton("xrDO");
		ImGui::EndGroup();

		ImGui::SameLine();
		
		yStep = 0;
		ImVec2 ListBoxSize = { -1, -1 };
		if (ImGui::BeginTable("##DemoContent", 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders| ImGuiTableFlags_ScrollY, ListBoxSize))
		{
			ImGui::TableSetupColumn("c1");
			
			yRow();

			ImGui::Text("Compiler");
			ImGui::Separator();
			DrawCompilerConfig();

			yRow();

			ImGui::Text("xrLC");
			ImGui::Separator();
			DrawLCConfig();

			yRow();

			ImGui::Text("xrAI");
			ImGui::Separator();
			DrawAIConfig();

			yRow();

			ImGui::Text("xrDO");
			ImGui::Separator();
			DrawDOConfig();

			//Заглушка
			ImGui::Dummy({ 0,winSize.y -123});

			ImGui::EndTable();
		}
		ImGui::End();
	}
}

int item_current_selected = 2;
int item_current_jitter = 2;
int item_current_jitter_mu = 6;

const char* items[] = { "1024", "2048", "4096", "8192" };
const char* itemsJitter[] = { "1", "4", "9" };
const char* itemsJitterMU[] = { "0", "1", "2", "3", "4", "5", "6"};

void DrawLCConfig()
{
	//if (ImGui::BeginChild("LC", { 200, 415 }, ImGuiChildFlags_Border, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::PushID("xrLC");
		ImGui::Checkbox("Levels Compiler", &gCompilerMode.LC);
		HoverHelp("Master switch for the Levels Compiler (xrLC).\n"
			"Runs the full build pipeline for every selected level: geometry conversion, UV mapping,\n"
			"subdivide, lightmap packing, lightmap/vertex/MU light baking and lightmap save.");
		ImGui::Separator();

		ImGui::BeginDisabled(!gCompilerMode.LC);

		// ImGui::Checkbox("GI (Radiosity)", &gCompilerMode.LC_GI);
		ImGui::Checkbox("No Sun", &gCompilerMode.LC_NoSun);
		HoverHelp("Skip the directional sun light while baking lightmaps and vertex colors.\n"
			"All sun channels stay black - use for levels lit only by interior or point lights.");
		ImGui::Checkbox("No Hemi", &gCompilerMode.LC_NoHemi);
		HoverHelp("Skip the hemisphere ('$hemi') light group while baking lightmaps.\n"
			"The lmap hemi channel stays black and the whole hemi raycast pass is skipped.\n"
			"Static geometry vertex lighting is baked without hemi anyway;\n"
			"MU objects keep their own baked vertex hemi.");
		ImGui::Checkbox("No Smooth Group", &gCompilerMode.LC_NoSMG);
		HoverHelp("Ignore the smoothing groups stored in level faces when generating smooth vertex\n"
			"Use when the geometry has sharp edges for smoothing.\n"
			"This should be the default methods since no one uses maya/3dsmax plugins anymore.");
		ImGui::Checkbox("Skip Progressive", &gCompilerMode.LC_Noise);
		HoverHelp("Do not build progressive meshes (VIPM) for x-meshes.\n"
			"Skips mesh simplification and the vertex-cache-ordered LOD records.\n"
			"Faster builds and no VIPM crashes on huge meshes,\n"
			"but the engine keeps full-detail geometry with no vertex-LOD memory savings.");
		ImGui::Checkbox("Tesselation", &gCompilerMode.LC_Tess);
		HoverHelp("Allow the Adaptive HT phase to tessellate (split) faces whose vertex hemi\n"
			"lighting differs strongly, giving smoother baked hemi/vertex lighting.\n"
			"Disable for faster builds on geometries with large tris, mostly terrain & buildings.");
		ImGui::Checkbox("Skip invalid faces", &gCompilerMode.LC_SkipInvalidFaces);
		HoverHelp("Do not hard-crash when degenerate or invalid faces are found during conversion.\n"
			"The bad faces are logged and skipped silently instead of aborting the whole build.");
		ImGui::Checkbox("Texture RGBA", &gCompilerMode.LC_tex_rgba);
		HoverHelp("Store lightmap textures as uncompressed 32-bit RGBA instead of DXT-compressed DDS.\n"
			"Static shadows (DX8) and gradients stay clean (no compression artifacts), but files are ~4x larger.");
		ImGui::Checkbox("Skip Subdivide", &gCompilerMode.LC_NoSubdivide);
		HoverHelp("Skip the subdivide phase that splits oversized faces into smaller ones before\n"
			"lightmap packing. Faster, but lightmaps are more coarse on very large faces.");
		ImGui::Checkbox("Skip Welding", &gCompilerMode.LC_skipWeld);
		HoverHelp("Skip the geometry welding phase - vertices and edges closer than 'Dist Weld'\n"
			"are no longer merged. Use when welding corrupts imported or very dense geometry.");

		ImGui::Separator();

		ImGui::SetNextItemWidth(100);
		if (ImGui::Combo("lmaps", &item_current_selected, items, 4))
 			gCompilerMode.LC_sizeLmaps = atoi(items[item_current_selected]);
		HoverHelp("Lightmap atlas resolution in pixels (per side).\n"
			"Larger atlases = fewer lightmap files and denser packing,\n"
			"but each texture costs more VRAM in the engine.");
		ImGui::Checkbox("Simple LMAP pack", &gCompilerMode.LC_LmapsAlternative);
		HoverHelp("Lightmap atlas placement strategy.\n"
			"OFF (default): per-pixel placement - a pixel-occupancy grid built from each\n"
			"deflector's marker channel packs deflectors tightly around lit texels\n"
			"(se7kills layout, densest atlases).\n"
			"ON: simple scanline row packing with fixed 2px borders -\n"
			"simpler and faster, but leaves noticeably more empty atlas space.");

		ImGui::Separator();
		ImGui::Checkbox("Overload Prebuild", &gCompilerMode.IsOverloadedSettings);
		HoverHelp("Ignore the quality settings stored in build.prj and use the\n"
			"customized values below for this run instead.");

		ImGui::BeginDisabled(!gCompilerMode.IsOverloadedSettings);

			ImGui::SetNextItemWidth(100);
			ImGui::Combo("JitterMU", &item_current_jitter_mu, itemsJitterMU, 7);
			HoverHelp("Raycast jitter sample count used while baking MU (multi-use) object vertex colors.\n"
				"0 = single centered ray, higher = averaged multi-ray sampling (smoother, slower).");
			ImGui::SetNextItemWidth(100);
			ImGui::Combo("Jitter", &item_current_jitter, itemsJitter, 3);
			HoverHelp("Jitter samples per lightmap texel: 1 = one centered ray,\n"
				"4 / 9 = stratified grid samples averaged per texel.\n"
				"Smoother shadows at a proportional speed cost.");
 			ImGui::SetNextItemWidth(100);
			ImGui::InputFloat("Lightmap Density", &gCompilerMode.LC_Pixels);
			HoverHelp("Lightmap texels per world meter.\n"
				"Higher = sharper lightmaps, proportionally longer bake times and more memory.");
 			ImGui::SetNextItemWidth(100);
			ImGui::InputFloat("Dist Weld", &gCompilerMode.WeldDistance);
			HoverHelp("World-space distance threshold for the welding phase:\n"
				"vertices closer than this value are merged into one.");

			gCompilerMode.LC_JSample   = atoi(itemsJitter[item_current_jitter]);
			gCompilerMode.LC_JSampleMU = atoi(itemsJitterMU[item_current_jitter_mu]);

			ImGui::EndDisabled();
		ImGui::EndDisabled();

		ImGui::PopID();
		//ImGui::EndChild();
	}


}

void DrawDOConfig()
{
	ImGui::PushID("xrDO");
	ImGui::Checkbox("Details Compiler", &gCompilerMode.DO);
	HoverHelp("Run the Details Compiler (xrDO) for each selected level:\n"
		"bakes detail objects (grass/rocks etc. from detail slots) into the level details file.");
	ImGui::Separator();

	ImGui::BeginDisabled(!gCompilerMode.DO);
	ImGui::Checkbox("No Sun", &gCompilerMode.LC_NoSun);
	HoverHelp("Do not bake the sun light into detail object vertex colors.\n"
		"Shares the same flag as the xrLC 'No Sun' option.");
	ImGui::EndDisabled();

	ImGui::PopID();
}

void DrawAIConfig()
{
	//if (ImGui::BeginChild("AI", { 200, 370 }, ImGuiChildFlags_Border, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
	{
			ImGui::Checkbox("AI Compiler", &gCompilerMode.AI);
			HoverHelp("Master switch for the AI Compiler (xrAI).");

			ImGui::BeginDisabled(!gCompilerMode.AI);

			ImGui::Separator();

			ImGui::Checkbox("AI Compiler ai.level", &gCompilerMode.AI_BuildLevel);
			HoverHelp("Build the level AI map (ai.level): navigation nodes, links and graph data\n"
				"used by pathfinding, cover selection and NPC logic.");

				ImGui::BeginDisabled(!gCompilerMode.AI_BuildLevel);

				ImGui::Checkbox("Draft AI-Map", &gCompilerMode.AI_Draft);
				HoverHelp("Draft mode: fewer refinement and merge passes, no final validation.\n"
					"For quick iteration only - rebuild without this flag before release.");
				ImGui::Checkbox("Pure Covers", &gCompilerMode.AI_PureCovers);
				HoverHelp("Compute cover tables directly from raw level geometry,\n"
					"without the cover optimization and merge post-pass.");
				ImGui::Checkbox("Verify", &gCompilerMode.AI_Verify);
				HoverHelp("Run verify_level_graph on the built ai.level:\n"
					"checks node connectivity and reports broken or invalid links.");
				ImGui::Checkbox("Verbose", &gCompilerMode.AI_Verbose);
				HoverHelp("Print detailed per-node diagnostics while Verify runs.");

				ImGui::EndDisabled();

			ImGui::Separator();

			ImGui::Checkbox("AI Compiler all.spawn", &gCompilerMode.AI_BuildSpawn);
			HoverHelp("Build the global spawn file (all.spawn): game objects, waypoints,\n"
				"paths and level transitions for the whole game graph.");
				ImGui::BeginDisabled(!gCompilerMode.AI_BuildSpawn);

				ImGui::Checkbox("No Separator Check", &gCompilerMode.AI_NoSeparatorCheck);
				HoverHelp("Skip separator (level-transition link) validation while building all.spawn.\n"
					"Allows building spawn with partially connected levels.");

				ImGui::Text("Name all.spawn :");
				ImGui::InputText("#1", gCompilerMode.AI_spawn_name, sizeof(gCompilerMode.AI_spawn_name));
				HoverHelp("Output file name of the built all.spawn.");
				ImGui::Text("Name level start:");
				ImGui::InputText("#2", gCompilerMode.AI_StartActor, sizeof(gCompilerMode.AI_StartActor));
				HoverHelp("Name of the level that provides the actor start point\n"
					"for spawn graph construction.");

				ImGui::EndDisabled();

			ImGui::EndDisabled();
			//ImGui::EndChild();
	}
}

extern bool SaveCForm;

void DrawCompilerConfig()
{
	//if (ImGui::BeginChild("Settings", { 170, 370 }, ImGuiChildFlags_Border, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::Checkbox("Silent mode", &gCompilerMode.Silent);
		HoverHelp("Run without dialog windows or message boxes - all output goes to the log only.\n"
			"A failing build will not stop for user input.");
		ImGui::Checkbox("Use Intel Embree", &gCompilerMode.Embree);
		HoverHelp("Trace occlusion rays with the Intel Embre kernel instead of the engine CDB collider.\n"
			"A BVH is built once per phase and traced with SIMD rays - several times faster\n"
			"on large levels.");
		ImGui::Separator();

		ImGui::TextColored(ImVec4(RGBAColor(204, 102, 102, 255)), "(Warning Disable MU in raycast)");

		ImGui::Checkbox("Embree Optimized", &gCompilerMode.Embree_SplitBVH);

		ImGui::TextColored(ImVec4(RGBAColor(0, 255, 0, 255)), "(This Only For Build BVH)");
		ImGui::Checkbox("Embree Compacted", &gCompilerMode.EmbreeBVHCompact);
		HoverHelp("Adds RTC_SCENE_FLAG_COMPACT to the Embre scene - compact BVH node layout.\n"
			"Less memory, slightly slower traversal.");
		ImGui::Checkbox("Embree Robust", &gCompilerMode.EmbreeBVHRobust);
		HoverHelp("Adds RTC_SCENE_FLAG_ROBUST to the Embre scene - exact ray/triangle intersection\n"
			"tests near shared edges. Fixes rare missed hits, slightly slower.");

		ImGui::Separator();

		ImGui::Checkbox("Clear temp files", &gCompilerMode.ClearTemp);
		HoverHelp("Delete the level's temp folder (intermediate build data) after the compiler finishes.");
		ImGui::Checkbox("Skip THM", &gCompilerMode.SkipTHM);
		HoverHelp("Do not write .thm (thumbnail/meta) files next to the exported textures.");
		ImGui::Checkbox("Save cform to obj", &SaveCForm);
		HoverHelp("Keep CFORM simplifier temp files and dump the collision form as a\n"
			"Wavefront .obj mesh for inspection in external 3D tools.");

		ImGui::Separator();
		ImGui::Text("Threads Max");

		if (ImGui::InputInt("##Threads Max", &gCompilerMode.ThreadsPerWork))
		{
			gCompilerMode.ThreadsPerWork = std::min((u32)gCompilerMode.ThreadsPerWork, CPU::ID.n_threads);
		}
		HoverHelp("Cap for the worker thread pool used by all parallel phases.\n"
			"Clamped to the CPU logical core count.");

		//ImGui::EndChild();
	}
	
}

void getStatusInfo(IterationStatus status, xr_string& text, ImVec4& textCol, char& icon)
{
	switch (status)
	{
	case Complete:
		text = "Complete";
		textCol = { 0, 0.9, 0, 1 };

		icon = 'C';
		break;
	case InProgress:
		text = "In Progress";
		textCol = { 0.9, 0.9, 0, 1 };

		icon = 'B';
		break;
	case Pending:
		text = "Pending";
		textCol = { 0.8, 0.8, 0.8, 0.8 };

		icon = 'A';
		break;
	case Skip:
		text = "Skip";
		textCol = { 0.9, 0.9, 0.9, 0.6 };

		icon = 'D';
		break;
	default:
		text = "";
		textCol = { 1,1,1,1 };

		icon = 'A';
		break;
	}
}

const ImVec4 getLogColor(char* text)
{
	if (text == nullptr || xr_strlen(text) == 0)
		return ImVec4(RGBAColor(230, 230, 230, 255));

	xr_string TextEx = text;
	TextEx = TextEx.RemoveWhitespaces();
	size_t Pos = TextEx.find('|');

	while (Pos != xr_string::npos)
	{
		TextEx.erase(Pos, 1);
		Pos = TextEx.find('|');
	}

	char Word = TextEx[0];

	switch (Word)
	{
	case '~': return ImVec4(RGBAColor(248, 248, 49, 255));
	case '!': return ImVec4(RGBAColor(204, 102, 102, 255));
	case '@': return ImVec4(RGBAColor(125, 125, 241, 255));
	case '#': return ImVec4(RGBAColor(0, 222, 205, 155));
	case '%': return ImVec4(RGBAColor(202, 85, 219, 155));
	case '$': return ImVec4(RGBAColor(172, 172, 255, 255));
	case '*': return ImVec4(RGBAColor(248, 248, 49, 255));
	case '^': return ImVec4(RGBAColor(100, 246, 121, 255));
	case '&': return ImVec4(RGBAColor(255, 255, 0, 255));
	case '-': return ImVec4(RGBAColor(0, 255, 0, 255));
	case '+': return ImVec4(RGBAColor(84, 255, 255, 255));
	case '=': return ImVec4(RGBAColor(205, 205, 105, 255));
	case '/': return ImVec4(RGBAColor(146, 146, 252, 255));
	}

	return ImVec4(RGBAColor(230, 230, 230, 255));
}

void RenderCompilerUI(int X, int Y)
{
	//static const char* levelName = "LevelTextName";

	// Set up the window
	ImGui::Begin("Compile Split Screen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus);

	// Calculate sizes for the top and bottom parts
	ImVec2 windowSize = ImGui::GetContentRegionAvail();
	float topHeight = hideLogSection ? windowSize.y - 58.f : windowSize.y * 0.5f;

	// Top section
	ImGui::BeginChild("TopSection", ImVec2(windowSize.x, topHeight), true);

	// Level name
	xr_string Levels;

	for (auto& [Name, Selected] : gCompilerMode.Files)
	{
		if (Selected)
			Levels += (!Levels.empty() ? ", " : "") + Name;
	}
	ImGui::Text("%s", Levels.c_str());
	ImGui::Separator();

	ImVec4 phaseTextCol = { 78, 178, 98, 0.78 };


	if (ResizeMaximal)
	{
		if (X != 1400 || Y != 925)
		{
			SDL_SetWindowSize(g_AppInfo.Window, 1400, 925);
		}
	}
	else
	{
		if (X != 1000 || Y != 560)
		{
			SDL_SetWindowSize(g_AppInfo.Window, 1000, 560);
		}
	}
	
	int MAX_TRABS = 9;
	if (ResizeMaximal)
		MAX_TRABS = 10;

	// Table
	if (ImGui::BeginTable("IterationsTable", MAX_TRABS, ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedSame))
	{
 		if (ResizeMaximal)
		{
			ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 15.0f);
			ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_WidthFixed, 15.f);
			ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 250.0f);
			ImGui::TableSetupColumn("Phase %", ImGuiTableColumnFlags_WidthFixed, 40.f);
			ImGui::TableSetupColumn("Elapsed Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Remain Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Warnings", ImGuiTableColumnFlags_WidthFixed, 25.0f);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 50.f);
			ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 50.f);
 
			ImGui::TableSetupColumn("Status Description", ImGuiTableColumnFlags_WidthStretch);
		}
		else
		{
			ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 15.0f);
			ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_WidthFixed, 50.f);
			ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Phase %", ImGuiTableColumnFlags_WidthFixed, 50.f);
			ImGui::TableSetupColumn("Elapsed Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Remain Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Warnings", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.f);
			ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 100.f);
		}

		ImGui::TableHeadersRow();

		for (auto& row : GetIterationData())
		{

			xr_string rowStatus;
			ImVec4 rowStatusColor;

			char rowIcon;

			getStatusInfo(row.status, rowStatus, rowStatusColor, rowIcon);

			ImGui::TableNextRow();

			// Status icon

			ImGui::TableSetColumnIndex(0);
			ImGui::PushFont(gCompilerMode.CompilerIconsFont);
			ImGui::TextColored(rowStatusColor, "%c", rowIcon);
			ImGui::PopFont();

			// TASK
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", row.iterationName.c_str());

			// 
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%0.f", row.Persent * 100);

			ImGui::TableSetColumnIndex(6);
			ImGui::Text("%d", row.warnings);
			// Status text
			ImGui::TableSetColumnIndex(7);


			ImGui::TextColored(rowStatusColor, rowStatus.c_str());

			for (auto& phase : row.phases)
			{
				xr_string status;
				ImVec4 statusColor;
				char phaseIcon;

				getStatusInfo(phase.status, status, statusColor, phaseIcon);

				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(1);
				ImGui::PushFont(gCompilerMode.CompilerIconsFont);

				float column_width = ImGui::GetColumnWidth();
				float text_size = ImGui::CalcTextSize("A").x;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column_width - text_size);

				ImGui::TextColored(statusColor, "%c", phaseIcon);

				ImGui::PopFont();

				ImGui::TableSetColumnIndex(2);
				ImGui::TextColored(phaseTextCol, phase.PhaseName.c_str());
				//PHASE %
				auto pers = phase.PhasePersent;

				if (phase.status != Complete)
				{
					u32 dwCurrentTime = timeGetTime();
					u32 dwTimeDiff = dwCurrentTime - GetPhaseStartTime();
					u32 secElapsed = dwTimeDiff / 1000;
					u32 secRemain = u32(float(secElapsed) / pers) - secElapsed;

					phase.elapsed_time = secElapsed;
					if (pers > 0.005f)
						phase.remain_time = secRemain;
				}

				//
				if (phase.status == Complete) pers = 1;
				else if (pers > 1.f)	pers = 1;
				else if (pers < 0.f)	pers = 0;

				ImGui::TableSetColumnIndex(3);
				ImGui::TextColored(phaseTextCol, "%0.f", pers * 100);

				ImGui::TableSetColumnIndex(4);
				ImGui::TextColored(phaseTextCol, "%s", make_time(phase.elapsed_time).c_str());

				ImGui::TableSetColumnIndex(5);
				if (phase.status != Complete)
					ImGui::TextColored(phaseTextCol, "%s", (phase.remain_time == 0 ? "Calculating..." : make_time(phase.remain_time).c_str()));

				ImGui::TableSetColumnIndex(7);

				ImGui::TextColored(statusColor, status.c_str());

				ImGui::TableSetColumnIndex(8);
				ImGui::Text("%u MB", u32 ( size_t( phase.used_memory/ 1024/ 1024) ) );

				if (ResizeMaximal)
				{
					ImGui::TableSetColumnIndex(9);
					ImGui::Text("%s", phase.AdditionalData.c_str());
				}
			}
		}

		if (autoScroll)
			ImGui::SetScrollY(ImGui::GetScrollMaxY());
		ImGui::EndTable();
	}

	ImGui::EndChild();
	 

	ImGui::Separator();
	ImGui::Text("Log");

	ImGui::SameLine();

	const char* buttonText = (hideLogSection) ? "+" : "-";
	ImVec2 textSize = ImGui::CalcTextSize(buttonText);

	ImVec2 buttonSize = ImVec2(textSize.x + ImGui::GetStyle().FramePadding.x * 2,
		textSize.y + ImGui::GetStyle().FramePadding.y * 2);

	auto ZSize = ImGui::GetContentRegionAvail();

	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ZSize.x - buttonSize.x);

	if (ImGui::Button(buttonText))
		hideLogSection = !hideLogSection;

	if (!hideLogSection && ImGui::BeginChild("LogSection", ImVec2(windowSize.x, windowSize.y - topHeight - (buttonSize.y * 2)-30), true))
	{
		ImGuiListClipper clipper;

		xrCriticalSectionGuard LogGuard(&csLog);

		clipper.Begin(GetLogVector().size());

		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				auto& line = GetLogVector()[i];
				ImGui::TextColored(getLogColor((char*)line.c_str()), "%s", line.c_str());
			}
		}


		if (autoScroll)
			ImGui::SetScrollY(ImGui::GetScrollMaxY());

		ImGui::EndChild();
	}

	ImGui::Separator();

	if (ImGui::Button(autoScroll ? "Disable Auto-Scroll" : "Enable Auto-Scroll"))
	{
		autoScroll = !autoScroll;
	}

	ImGui::SameLine();

	if (ImGui::Button(!ResizeMaximal ? "Maximal resize" : "Minimal resize"))
	{
		ResizeMaximal = !ResizeMaximal;
	}

	ImGui::SameLine();

	ImGui::TextColored( ImVec4{ 0, 0.9, 0, 1 }, "Memory usage: %u mb", GetHeapMemory() / 1024 / 1024);

	ImGui::SameLine();
	ImGui::Checkbox("Show Main", &ShowMainUI);

	ImGui::End();
}