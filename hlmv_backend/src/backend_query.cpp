#include "backend_query.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "filesystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "datacache/imdlcache.h"
#include "studio.h"
#include "StudioModel.h"
#include "ViewerSettings.h"

// Response-file plumbing and the dump builders of the IPC protocol v2 data
// queries (docs/IPC_PROTOCOL.md). Everything here runs on the main thread:
// either when the main window handles VMTIPC_QUERY_JOB_MESSAGE or inline when
// the message could not be posted (the WM_COPYDATA handler runs there too).

extern void HLMV_Log(const char* fmt, ...);

// models dump limits; the walk stops with a "note\ttruncated" row.
#define QUERY_MAX_MODEL_ROWS   30000
#define QUERY_MAX_MODEL_SCAN_ENTRIES 400000

// assets dump limits. The row cap is what the UI index can absorb; the scan cap
// and the clock budget keep a VPK-heavy game from monopolizing the backend's
// main thread. The budget is per root so a pathological root cannot starve the
// rest, with a global cap that leaves room for the source resolution and the
// UI's 60 s query timeout (docs/IPC_PROTOCOL.md).
#define QUERY_MAX_ASSET_ROWS 150000
#define QUERY_MAX_SCAN_ENTRIES 1200000
#define QUERY_ASSET_ROOT_BUDGET_MS 20000
#define QUERY_MAX_ASSET_SCAN_MS 45000

// RAW refuses files bigger than this (response files are polled, not streamed).
#define QUERY_MAX_RAW_BYTES (256u * 1024u * 1024u)

// Bodyparts tracked for the model_scene bodypart rows.
#define QUERY_MAX_BODYPARTS 64

static std::string g_responseDir;
static HWND g_hQueryWnd = NULL;
static int g_submodels[QUERY_MAX_BODYPARTS] = { 0 };

struct BackendQueryJob
{
	std::string kind;   // dump kind, ignored for RAW
	std::string token;
	std::string args;   // dump prefix / RAW game path
	bool raw;
};

//-----------------------------------------------------------------------------
// Small helpers
//-----------------------------------------------------------------------------

static bool IsValidToken(const std::string& token)
{
	if (token.empty() || token.size() > 64)
	{
		return false;
	}
	for (size_t i = 0; i < token.size(); ++i)
	{
		char c = token[i];
		bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		             (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!valid)
		{
			return false;
		}
	}
	return true;
}

static std::string Trimmed(const std::string& text)
{
	size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return std::string();
	}
	size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

static std::string FixSlashes(const std::string& path)
{
	std::string out = path;
	for (size_t i = 0; i < out.size(); ++i)
	{
		if (out[i] == '\\')
		{
			out[i] = '/';
		}
	}
	return out;
}

// Field values coming from the engine must not break the TSV layout.
static std::string Sanitize(const char* text)
{
	if (!text)
	{
		return std::string();
	}
	std::string out = text;
	for (size_t i = 0; i < out.size(); ++i)
	{
		char c = out[i];
		if (c == '\t' || c == '\r' || c == '\n')
		{
			out[i] = ' ';
		}
	}
	return out;
}

static std::string IntField(int value)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%d", value);
	return buffer;
}

static std::string FloatField(float value)
{
	if (!std::isfinite(value))
	{
		value = 0.0f;
	}
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.3f", value);
	return buffer;
}

static void AppendRow(std::string& text, std::initializer_list<std::string> fields)
{
	bool first = true;
	for (std::initializer_list<std::string>::const_iterator it = fields.begin(); it != fields.end(); ++it)
	{
		if (!first)
		{
			text += '\t';
		}
		text += *it;
		first = false;
	}
	text += '\n';
}

//-----------------------------------------------------------------------------
// Response files
//-----------------------------------------------------------------------------

static std::string DefaultResponseDir()
{
	char tempPath[MAX_PATH];
	DWORD length = GetTempPathA(MAX_PATH, tempPath);
	std::string dir;
	if (length > 0 && length < MAX_PATH)
	{
		dir = tempPath;
	}
	dir += "VMTStudio\\ipc";
	return dir;
}

static std::string ResponsePath(const std::string& token, const char* extension)
{
	return g_responseDir + "\\" + token + extension;
}

static bool WriteBytes(const std::string& path, const void* data, size_t size)
{
	FILE* file = fopen(path.c_str(), "wb");
	if (!file)
	{
		return false;
	}
	bool ok = true;
	if (size > 0 && fwrite(data, 1, size, file) != size)
	{
		ok = false;
	}
	if (fclose(file) != 0)
	{
		ok = false;
	}
	return ok;
}

static void DeleteFileIfExists(const std::string& path)
{
	if (!path.empty())
	{
		DeleteFileA(path.c_str());
	}
}

static void DeleteStaleFiles(const std::string& token)
{
	DeleteFileIfExists(ResponsePath(token, ".dat"));
	DeleteFileIfExists(ResponsePath(token, ".bin"));
	DeleteFileIfExists(ResponsePath(token, ".done"));
	DeleteFileIfExists(ResponsePath(token, ".err"));
	DeleteFileIfExists(ResponsePath(token, ".part"));

	// Anything else an older job may have left for this token.
	std::string pattern = g_responseDir + "\\" + token + ".*";
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(pattern.c_str(), &data);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				DeleteFileIfExists(g_responseDir + "\\" + data.cFileName);
			}
		} while (FindNextFileA(find, &data));
		FindClose(find);
	}
}

// Writes <token>.<extension> atomically and marks the result complete with the
// empty <token>.done file.
static bool PublishPayload(const std::string& token, const char* extension, const void* data, size_t size)
{
	const std::string part = ResponsePath(token, ".part");
	const std::string destination = ResponsePath(token, extension);

	if (!WriteBytes(part, data, size))
	{
		DeleteFileIfExists(part);
		return false;
	}

	if (!MoveFileExA(part.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileIfExists(destination);
		if (!MoveFileA(part.c_str(), destination.c_str()))
		{
			DeleteFileIfExists(part);
			return false;
		}
	}

	if (!WriteBytes(ResponsePath(token, ".done"), "", 0))
	{
		DeleteFileIfExists(ResponsePath(token, ".done"));
		DeleteFileIfExists(destination);
		return false;
	}
	return true;
}

static void WriteErrorResult(const std::string& token, const std::string& message)
{
	// A failed job must never look complete.
	DeleteFileIfExists(ResponsePath(token, ".done"));
	DeleteFileIfExists(ResponsePath(token, ".dat"));
	DeleteFileIfExists(ResponsePath(token, ".bin"));
	DeleteFileIfExists(ResponsePath(token, ".part"));

	std::string text = Trimmed(message);
	if (text.empty())
	{
		text = "query failed";
	}
	WriteBytes(ResponsePath(token, ".err"), text.data(), text.size());
}

//-----------------------------------------------------------------------------
// file scans (models and assets dumps)
//-----------------------------------------------------------------------------

// The engine indexes entries with their original casing; the dumps dedup by a
// folded path but report the first spelling seen, which is what the game uses.
static std::string LowerAscii(const std::string& text)
{
	std::string out = text;
	for (size_t i = 0; i < out.size(); ++i)
	{
		if (out[i] >= 'A' && out[i] <= 'Z')
		{
			out[i] = (char)(out[i] - 'A' + 'a');
		}
	}
	return out;
}

static bool HasExtension(const std::string& path, const std::vector<std::string>& extensions)
{
	for (size_t i = 0; i < extensions.size(); ++i)
	{
		const std::string& extension = extensions[i];
		if (path.size() >= extension.size() &&
			_stricmp(path.c_str() + path.size() - extension.size(), extension.c_str()) == 0)
		{
			return true;
		}
	}
	return false;
}

// Find* entries are either leaf names of the scanned directory or already
// game-relative paths (the engine's wildcard matcher differs between the
// loose folders and the VPK index).
static std::string NormalizeEntry(const std::string& dir, const std::string& entry)
{
	std::string fixed = FixSlashes(entry);
	if (fixed.find('/') == std::string::npos)
	{
		return dir + "/" + fixed;
	}
	return fixed;
}

// The engine resolves entries inside a VPK as "<game>/<name>.vpk/<inner path>"
// but does not always set PATH_IS_PACKFILE in the returned path type, so the
// resolved path has to be inspected as well.
static bool ResolvedPathIsInVpk(const char* resolved)
{
	const std::string path = FixSlashes(resolved ? resolved : "");
	const std::string needle = ".vpk/";
	if (path.size() >= needle.size() &&
		path.compare(path.size() - needle.size(), needle.size(), needle) == 0)
	{
		return true;
	}
	return path.find(needle) != std::string::npos;
}

// Loose files and VPK entries both resolve through the engine; the resolved
// path is the only reliable hint because PATH_IS_PACKFILE is not always set.
static const char* QueryFileSource(const std::string& path)
{
	if (!g_pFileSystem)
	{
		return "vpk";
	}
	char resolved[1024];
	PathTypeQuery_t pathType = 0;
	if (!g_pFileSystem->RelativePathToFullPath(path.c_str(), NULL, resolved, (int)sizeof(resolved), FILTER_NONE, &pathType))
	{
		return "vpk";
	}
	return (IS_PACKFILE(pathType) || ResolvedPathIsInVpk(resolved)) ? "vpk" : "loose";
}

// One scan shared by the models and assets dumps. The engine returns the same
// file from the flat VPK wildcard and from the loose walk, so the lower-case
// path is the dedup key while the first spelling seen is the one reported. A
// map keeps the output deterministic (sorted by that key).
struct FileScan
{
	std::map<std::string, std::string> paths;
	size_t maxRows;
	size_t maxScanEntries;
	ULONGLONG scanDeadline;   // GetTickCount64() value; 0 disables the clock budget
	size_t scanEntries;
	bool truncated;

	FileScan(size_t rows, size_t entries)
		: maxRows(rows), maxScanEntries(entries), scanDeadline(0), scanEntries(0), truncated(false) {}

	bool BudgetExhausted() const
	{
		if (scanEntries >= maxScanEntries)
		{
			return true;
		}
		return scanDeadline != 0 && GetTickCount64() >= scanDeadline;
	}
};

static void AddScanCandidate(FileScan& scan, const std::string& path, const std::string& prefix,
                             const std::vector<std::string>& extensions)
{
	if (!HasExtension(path, extensions))
	{
		return;
	}
	// The same prefix test the UI uses when it scans the game folder itself.
	if (!prefix.empty() && path.compare(0, prefix.size(), prefix) != 0)
	{
		return;
	}
	const std::string key = LowerAscii(path);
	if (scan.paths.find(key) != scan.paths.end())
	{
		return;
	}
	if (scan.paths.size() >= scan.maxRows)
	{
		scan.truncated = true;
		return;
	}
	scan.paths.insert(std::make_pair(key, path));
}

// Flat wildcard per extension: this is what reaches the VPK index entries
// whose wildcard matcher spans the whole stored path.
static void ScanFilesFlat(const std::string& root, const std::string& prefix,
                          const std::vector<std::string>& extensions, FileScan& scan)
{
	for (size_t i = 0; i < extensions.size() && !scan.truncated; ++i)
	{
		FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
		const char* entry = g_pFileSystem->FindFirst((root + "/*" + extensions[i]).c_str(), &handle);
		while (entry != NULL && !scan.truncated)
		{
			if (scan.BudgetExhausted())
			{
				scan.truncated = true;
				break;
			}
			++scan.scanEntries;
			if (!g_pFileSystem->FindIsDirectory(handle))
			{
				AddScanCandidate(scan, NormalizeEntry(root, entry), prefix, extensions);
			}
			entry = g_pFileSystem->FindNext(handle);
		}
		if (handle != FILESYSTEM_INVALID_FIND_HANDLE)
		{
			g_pFileSystem->FindClose(handle);
		}
	}
}

// Directory walk: covers loose files and addon folders that the VPK-style
// wildcard misses.
static void ScanFilesRecursive(const std::string& root, const std::string& prefix,
                               const std::vector<std::string>& extensions, FileScan& scan)
{
	std::vector<std::string> pending;
	std::set<std::string> visited;
	pending.push_back(root);

	while (!pending.empty() && !scan.truncated)
	{
		std::string dir = pending.back();
		pending.pop_back();
		if (!visited.insert(dir).second)
		{
			continue;
		}

		FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
		const char* entry = g_pFileSystem->FindFirst((dir + "/*").c_str(), &handle);
		while (entry != NULL && !scan.truncated)
		{
			if (scan.BudgetExhausted())
			{
				scan.truncated = true;
				break;
			}
			++scan.scanEntries;
			std::string name = entry;
			if (name != "." && name != "..")
			{
				if (g_pFileSystem->FindIsDirectory(handle))
				{
					pending.push_back(NormalizeEntry(dir, name));
				}
				else
				{
					AddScanCandidate(scan, NormalizeEntry(dir, name), prefix, extensions);
				}
			}
			entry = g_pFileSystem->FindNext(handle);
		}
		if (handle != FILESYSTEM_INVALID_FIND_HANDLE)
		{
			g_pFileSystem->FindClose(handle);
		}
	}
}

//-----------------------------------------------------------------------------
// models dump
//-----------------------------------------------------------------------------

static void BuildModelsDump(const std::string& args, std::string& text, std::string& error)
{
	if (!g_pFileSystem)
	{
		error = "file system is not available";
		return;
	}

	std::string prefix = FixSlashes(Trimmed(args));

	// Walk the directory the prefix points into; an empty or partial prefix
	// falls back to the models/ root.
	std::string root = "models";
	if (!prefix.empty())
	{
		size_t slash = prefix.find_last_of('/');
		if (slash != std::string::npos)
		{
			root = prefix.substr(0, slash);
			if (root.empty())
			{
				root = "models";
			}
		}
	}

	static const std::vector<std::string> kModelExtensions = { ".mdl" };
	FileScan scan(QUERY_MAX_MODEL_ROWS, QUERY_MAX_MODEL_SCAN_ENTRIES);
	ScanFilesFlat(root, prefix, kModelExtensions, scan);
	ScanFilesRecursive(root, prefix, kModelExtensions, scan);

	text = "VMTStudio-IPC\t2\n";
	AppendRow(text, { "kind", "models" });
	for (std::map<std::string, std::string>::const_iterator it = scan.paths.begin(); it != scan.paths.end(); ++it)
	{
		AppendRow(text, { "model", it->second, QueryFileSource(it->second) });
	}
	if (scan.truncated)
	{
		// Unknown rows are ignored by the parser; this documents the cut.
		AppendRow(text, { "note", "truncated" });
	}

	HLMV_Log("[IPC] models dump: %d row(s)%s\n", (int)scan.paths.size(), scan.truncated ? " (truncated)" : "");
}

//-----------------------------------------------------------------------------
// assets dump
//-----------------------------------------------------------------------------

// "a;b;c" -> { "a", "b", "c" } with trimmed, non-empty elements.
static std::vector<std::string> SplitList(const std::string& text, char separator)
{
	std::vector<std::string> values;
	size_t start = 0;
	for (;;)
	{
		size_t end = text.find(separator, start);
		if (end == std::string::npos)
		{
			end = text.size();
		}
		std::string value = Trimmed(text.substr(start, end - start));
		if (!value.empty())
		{
			values.push_back(value);
		}
		if (end == text.size())
		{
			break;
		}
		start = end + 1;
	}
	return values;
}

static std::string NormalizeAssetExtension(const std::string& extension)
{
	std::string out = LowerAscii(Trimmed(extension));
	if (!out.empty() && out[0] != '.')
	{
		out = "." + out;
	}
	return out;
}

static void BuildAssetsDump(const std::string& args, std::string& text, std::string& error)
{
	if (!g_pFileSystem)
	{
		error = "file system is not available";
		return;
	}

	// "<roots> <extensions>": the first space separates the two lists and ';'
	// separates the elements inside each one.
	const size_t separator = args.find(' ');
	const std::string rootsText = separator == std::string::npos ? args : args.substr(0, separator);
	const std::string extensionsText = separator == std::string::npos ? std::string() : args.substr(separator + 1);

	std::vector<std::string> roots;
	const std::vector<std::string> rawRoots = SplitList(rootsText, ';');
	for (size_t i = 0; i < rawRoots.size(); ++i)
	{
		std::string root = FixSlashes(rawRoots[i]);
		while (!root.empty() && root.back() == '/')
		{
			root.pop_back();
		}
		if (!root.empty())
		{
			roots.push_back(root);
		}
	}

	std::vector<std::string> extensions;
	const std::vector<std::string> rawExtensions = SplitList(extensionsText, ';');
	for (size_t i = 0; i < rawExtensions.size(); ++i)
	{
		const std::string extension = NormalizeAssetExtension(rawExtensions[i]);
		if (!extension.empty() && std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
		{
			extensions.push_back(extension);
		}
	}

	if (roots.empty())
	{
		error = "missing asset roots";
		return;
	}
	if (extensions.empty())
	{
		error = "missing asset extensions";
		return;
	}

	const ULONGLONG startTick = GetTickCount64();
	const ULONGLONG scanDeadline = startTick + QUERY_MAX_ASSET_SCAN_MS;

	FileScan scan(QUERY_MAX_ASSET_ROWS, QUERY_MAX_SCAN_ENTRIES);
	for (size_t i = 0; i < roots.size() && !scan.truncated; ++i)
	{
		scan.scanDeadline = GetTickCount64() + QUERY_ASSET_ROOT_BUDGET_MS;
		if (scan.scanDeadline > scanDeadline)
		{
			scan.scanDeadline = scanDeadline;
		}

		ScanFilesFlat(roots[i], std::string(), extensions, scan);
		ScanFilesRecursive(roots[i], std::string(), extensions, scan);
	}

	text = "VMTStudio-IPC\t2\n";
	AppendRow(text, { "kind", "assets" });
	for (std::map<std::string, std::string>::const_iterator it = scan.paths.begin(); it != scan.paths.end(); ++it)
	{
		AppendRow(text, { "asset", it->second, QueryFileSource(it->second) });
	}
	if (scan.truncated)
	{
		// Unknown rows are ignored by the parser; this documents the cut.
		AppendRow(text, { "note", "truncated" });
	}

	const ULONGLONG elapsed = GetTickCount64() - startTick;
	HLMV_Log("[IPC] assets dump: %d row(s), %d scan entr%s in %llu ms%s\n",
		(int)scan.paths.size(), (int)scan.scanEntries, scan.scanEntries == 1 ? "y" : "ies",
		(unsigned long long)elapsed, scan.truncated ? " (truncated)" : "");
}

//-----------------------------------------------------------------------------
// model_scene dump
//-----------------------------------------------------------------------------

static bool ModelIsLoaded()
{
	return g_pStudioModel != NULL && g_pStudioModel->HasModel();
}

static std::string GameRelativeModelPath(const std::string& fileName)
{
	std::string fixed = FixSlashes(fileName);
	bool looksAbsolute = (fixed.size() > 1 && fixed[1] == ':') || (!fixed.empty() && fixed[0] == '/');
	if (looksAbsolute && g_pFileSystem)
	{
		char relative[1024];
		if (g_pFileSystem->FullPathToRelativePath(fileName.c_str(), relative, (int)sizeof(relative)))
		{
			return FixSlashes(relative);
		}
	}
	return fixed;
}

static std::string MaterialNameFromVmtPath(const std::string& vmtPath)
{
	std::string name = vmtPath;
	if (name.size() > 10 && _strnicmp(name.c_str(), "materials/", 10) == 0)
	{
		name = name.substr(10);
	}
	if (name.size() >= 4 && _stricmp(name.c_str() + name.size() - 4, ".vmt") == 0)
	{
		name.resize(name.size() - 4);
	}
	return name;
}

// The game resolves a model texture by trying "materials/" + each $cdmaterials
// directory + the texture name + ".vmt", in order (this is what vrad does for
// texture shadow models).
static std::string ResolveTextureMaterialName(studiohdr_t* hdr, int index)
{
	if (!hdr || index < 0 || index >= hdr->numtextures)
	{
		return std::string();
	}
	std::string textureName;
	mstudiotexture_t* texture = hdr->pTexture(index);
	if (texture && texture->sznameindex != 0)
	{
		textureName = Sanitize(texture->pszName());
	}

	std::string firstCandidate;
	for (int j = 0; j < hdr->numcdtextures; ++j)
	{
		const char* cd = hdr->pCdtexture(j);
		if (!cd)
		{
			continue;
		}
		std::string candidate = FixSlashes("materials/" + Sanitize(cd) + textureName + ".vmt");
		if (firstCandidate.empty())
		{
			firstCandidate = candidate;
		}
		if (g_pFileSystem && g_pFileSystem->FileExists(candidate.c_str(), "GAME"))
		{
			return MaterialNameFromVmtPath(candidate);
		}
	}
	if (!firstCandidate.empty())
	{
		return MaterialNameFromVmtPath(firstCandidate);
	}
	return FixSlashes(textureName);
}

static std::string ResolveMaterialShader(const std::string& materialName)
{
	if (materialName.empty() || !g_pMaterialSystem)
	{
		return std::string();
	}
	IMaterial* material = g_pMaterialSystem->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
	if (!material || material->IsErrorMaterial())
	{
		return std::string();
	}
	const char* shaderName = material->GetShaderName();
	return shaderName ? Sanitize(shaderName) : std::string();
}

static void BuildModelSceneDump(std::string& text, std::string& error)
{
	if (!ModelIsLoaded())
	{
		error = "no model is loaded";
		return;
	}

	CStudioHdr* studioHdr = g_pStudioModel->GetStudioHdr();
	studiohdr_t* hdr = g_pStudioModel->GetStudioRenderHdr();
	if (!studioHdr || !hdr)
	{
		error = "no model is loaded";
		return;
	}

	text = "VMTStudio-IPC\t2\n";
	AppendRow(text, { "kind", "model_scene" });

	const char* fileName = g_pStudioModel->GetFileName();
	AppendRow(text, { "info", "name", GameRelativeModelPath(fileName ? fileName : "") });

	const auto& metrics = g_pStudioModel->GetDrawMetrics();
	int lods = 0;
	studiohwdata_t* hardware = g_pStudioModel->GetHardwareData();
	if (hardware)
	{
		lods = hardware->m_NumLODs;
	}
	Vector minimum, maximum;
	g_pStudioModel->ExtractBbox(minimum, maximum);
	AppendRow(text, { "stats", IntField(metrics.PolyCount), IntField(metrics.NumBatches),
		IntField(studioHdr->numbones()), IntField(lods), IntField(hdr->numtextures),
		FloatField(minimum.x), FloatField(minimum.y), FloatField(minimum.z),
		FloatField(maximum.x), FloatField(maximum.y), FloatField(maximum.z) });

	for (int i = 0; i < hdr->numtextures; ++i)
	{
		std::string materialName = ResolveTextureMaterialName(hdr, i);
		AppendRow(text, { "material", IntField(i), materialName, ResolveMaterialShader(materialName) });
	}

	// Which texture (after the current skin's remap) is used by which
	// bodypart/submodel; the UI needs this to pre-select material slots.
	int skin = g_viewerSettings.skin;
	if (skin < 0)
	{
		skin = 0;
	}
	if (hdr->numskinfamilies > 0 && skin >= hdr->numskinfamilies)
	{
		skin = hdr->numskinfamilies - 1;
	}

	std::set<std::string> useKeys;
	for (int partIndex = 0; partIndex < studioHdr->numbodyparts(); ++partIndex)
	{
		mstudiobodyparts_t* part = studioHdr->pBodypart(partIndex);
		if (!part)
		{
			continue;
		}
		std::string partName = part->sznameindex ? Sanitize(part->pszName()) : std::string();
		for (int submodel = 0; submodel < part->nummodels; ++submodel)
		{
			mstudiomodel_t* sub = part->pModel(submodel);
			if (!sub)
			{
				continue;
			}
			for (int meshIndex = 0; meshIndex < sub->nummeshes; ++meshIndex)
			{
				mstudiomesh_t* mesh = sub->pMesh(meshIndex);
				if (!mesh)
				{
					continue;
				}
				int textureIndex = mesh->material;
				if (hdr->skinindex != 0 && hdr->numskinfamilies > 0 && hdr->numskinref > 0 &&
					textureIndex >= 0 && textureIndex < hdr->numskinref)
				{
					int skinRef = skin * hdr->numskinref + textureIndex;
					if (skinRef >= 0 && skinRef < hdr->numskinfamilies * hdr->numskinref)
					{
						textureIndex = (int)hdr->pSkinref(skinRef)[0];
					}
				}
				if (textureIndex < 0 || textureIndex >= hdr->numtextures)
				{
					textureIndex = 0;
				}

				char key[64];
				snprintf(key, sizeof(key), "%d|%d|%d", textureIndex, partIndex, submodel);
				if (!useKeys.insert(key).second)
				{
					continue;
				}
				AppendRow(text, { "use", IntField(textureIndex), IntField(partIndex),
					IntField(submodel), partName });
			}
		}
	}

	for (int i = 0; i < studioHdr->GetNumSeq(); ++i)
	{
		mstudioseqdesc_t& sequence = studioHdr->pSeqdesc(i);
		// The engine string accessors return the struct itself when the index
		// is 0, so empty labels/activities must be checked first.
		std::string label = sequence.szlabelindex ? Sanitize(sequence.pszLabel()) : std::string();
		std::string activity = sequence.szactivitynameindex ? Sanitize(sequence.pszActivityName()) : std::string();
		AppendRow(text, { "sequence", IntField(i), label, activity,
			FloatField(g_pStudioModel->GetFPS(i)),
			IntField(g_pStudioModel->GetNumFrames(i)),
			FloatField(g_pStudioModel->GetDuration(i)),
			g_pStudioModel->GetSequenceLoops(i) ? "1" : "0",
			g_pStudioModel->IsHidden(i) ? "1" : "0" });
	}

	for (int i = 0; i < studioHdr->numbodyparts(); ++i)
	{
		mstudiobodyparts_t* part = studioHdr->pBodypart(i);
		if (!part)
		{
			continue;
		}
		AppendRow(text, { "bodypart", IntField(i),
			part->sznameindex ? Sanitize(part->pszName()) : std::string(),
			IntField(part->nummodels), IntField(BackendQuery_GetSubmodel(i)) });
	}

	// The MDL format has no skin names; the panel displays "Skin %d".
	for (int i = 0; i < hdr->numskinfamilies; ++i)
	{
		char name[32];
		snprintf(name, sizeof(name), "Skin %d", i + 1);
		AppendRow(text, { "skin", IntField(i), name });
	}

	for (int i = 0; i < studioHdr->numbones(); ++i)
	{
		mstudiobone_t* bone = studioHdr->pBone(i);
		if (!bone)
		{
			continue;
		}
		AppendRow(text, { "bone", IntField(i),
			bone->sznameindex ? Sanitize(bone->pszName()) : std::string(),
			IntField(bone->parent) });
	}

	for (int i = 0; i < studioHdr->GetNumAttachments(); ++i)
	{
		const mstudioattachment_t& attachment = studioHdr->pAttachment(i);
		AppendRow(text, { "attachment", IntField(i),
			attachment.sznameindex ? Sanitize(attachment.pszName()) : std::string() });
	}

	for (int i = 0; i < studioHdr->GetNumPoseParameters(); ++i)
	{
		const mstudioposeparamdesc_t& parameter = studioHdr->pPoseParameter(i);
		AppendRow(text, { "pose", IntField(i),
			parameter.sznameindex ? Sanitize(parameter.pszName()) : std::string(),
			FloatField(parameter.start), FloatField(parameter.end) });
	}
}

//-----------------------------------------------------------------------------
// anim_state dump
//-----------------------------------------------------------------------------

static void BuildAnimStateDump(std::string& text, std::string& error)
{
	if (!ModelIsLoaded())
	{
		error = "no model is loaded";
		return;
	}

	text = "VMTStudio-IPC\t2\n";
	AppendRow(text, { "kind", "anim_state" });
	AppendRow(text, { "state",
		IntField(g_pStudioModel->GetSequence()),
		FloatField(g_pStudioModel->GetFrame()),
		FloatField(g_pStudioModel->GetCycle()),
		FloatField(g_viewerSettings.speedScale),
		FloatField(g_pStudioModel->GetGroundSpeed()),
		g_viewerSettings.pause ? "1" : "0" });
}

//-----------------------------------------------------------------------------
// Job execution
//-----------------------------------------------------------------------------

static void RunDumpJob(const BackendQueryJob& job)
{
	std::string text;
	std::string error;

	if (job.kind == "models")
	{
		BuildModelsDump(job.args, text, error);
	}
	else if (job.kind == "assets")
	{
		BuildAssetsDump(job.args, text, error);
	}
	else if (job.kind == "model_scene")
	{
		BuildModelSceneDump(text, error);
	}
	else if (job.kind == "anim_state")
	{
		BuildAnimStateDump(text, error);
	}
	else
	{
		error = "unknown dump kind: " + job.kind;
	}

	if (!error.empty())
	{
		WriteErrorResult(job.token, error);
		return;
	}
	if (!PublishPayload(job.token, ".dat", text.data(), text.size()))
	{
		WriteErrorResult(job.token, "failed to write " + job.token + ".dat");
	}
}

static void RunRawJob(const BackendQueryJob& job)
{
	if (!g_pFileSystem)
	{
		WriteErrorResult(job.token, "file system is not available");
		return;
	}

	const std::string path = FixSlashes(Trimmed(job.args));
	if (path.empty())
	{
		WriteErrorResult(job.token, "missing file path");
		return;
	}

	FileHandle_t file = g_pFileSystem->Open(path.c_str(), "rb", "GAME");
	if (file == FILESYSTEM_INVALID_HANDLE)
	{
		WriteErrorResult(job.token, "failed to open " + path);
		return;
	}

	unsigned int size = g_pFileSystem->Size(file);
	if ((int)size < 0 || size > QUERY_MAX_RAW_BYTES)
	{
		g_pFileSystem->Close(file);
		WriteErrorResult(job.token, "file is too large to query or has invalid size");
		return;
	}

	std::string bytes;
	try
	{
		bytes.resize(size);
	}
	catch (...)
	{
		g_pFileSystem->Close(file);
		WriteErrorResult(job.token, "insufficient memory to read file");
		return;
	}
	unsigned int total = 0;
	while (total < size)
	{
		int read = g_pFileSystem->Read(&bytes[total], (int)(size - total), file);
		if (read <= 0)
		{
			break;
		}
		total += (unsigned int)read;
	}
	g_pFileSystem->Close(file);

	if (total != size)
	{
		WriteErrorResult(job.token, "failed to read " + path);
		return;
	}

	if (!PublishPayload(job.token, ".bin", bytes.data(), bytes.size()))
	{
		WriteErrorResult(job.token, "failed to write " + job.token + ".bin");
	}
}

static void ScheduleJob(BackendQueryJob* job)
{
	if (job == NULL)
	{
		return;
	}
	if (g_hQueryWnd && IsWindow(g_hQueryWnd) &&
		PostMessageA(g_hQueryWnd, VMTIPC_QUERY_JOB_MESSAGE, 0, (LPARAM)job))
	{
		return;
	}
	// Fall back to running the job inline; still on the main thread (the
	// WM_COPYDATA handler runs there), just without the deferred queue.
	BackendQuery_RunJob(0, (LPARAM)job);
}

//-----------------------------------------------------------------------------
// Public interface
//-----------------------------------------------------------------------------

// Re-creates the response directory; it may have been removed since init.
static void EnsureResponseDir()
{
	if (g_responseDir.empty())
	{
		BackendQuery_SetResponseDir(std::string());
		return;
	}
	std::error_code createError;
	std::filesystem::create_directories(g_responseDir, createError);
}

void BackendQuery_SetResponseDir(const std::string& dir)
{
	g_responseDir = Trimmed(dir);
	if (g_responseDir.empty())
	{
		g_responseDir = DefaultResponseDir();
	}
	while (!g_responseDir.empty() && (g_responseDir.back() == '\\' || g_responseDir.back() == '/'))
	{
		g_responseDir.pop_back();
	}

	std::error_code createError;
	std::filesystem::create_directories(g_responseDir, createError);
	HLMV_Log("[IPC] Query response directory: %s\n", g_responseDir.c_str());
}

void BackendQuery_SetMainWindow(HWND hWnd)
{
	g_hQueryWnd = hWnd;
}

bool BackendQuery_HandleDump(const std::string& kind, const std::string& token, const std::string& args)
{
	if (kind.empty() || !IsValidToken(token))
	{
		return false;
	}
	EnsureResponseDir();

	// The protocol deletes stale <token>.* files before starting a job.
	DeleteStaleFiles(token);

	BackendQueryJob* job = new BackendQueryJob();
	job->kind = kind;
	job->token = token;
	job->args = Trimmed(args);
	job->raw = false;
	ScheduleJob(job);
	return true;
}

bool BackendQuery_HandleRaw(const std::string& token, const std::string& path)
{
	if (!IsValidToken(token) || Trimmed(path).empty())
	{
		return false;
	}
	EnsureResponseDir();

	DeleteStaleFiles(token);

	BackendQueryJob* job = new BackendQueryJob();
	job->token = token;
	job->args = Trimmed(path);
	job->raw = true;
	ScheduleJob(job);
	return true;
}

void BackendQuery_RunJob(WPARAM wParam, LPARAM lParam)
{
	(void)wParam;

	BackendQueryJob* job = (BackendQueryJob*)lParam;
	if (job == NULL)
	{
		return;
	}

	if (job->raw)
	{
		RunRawJob(*job);
	}
	else if (g_pMDLCache)
	{
		MDLCACHE_CRITICAL_SECTION_(g_pMDLCache);
		RunDumpJob(*job);
	}
	else
	{
		RunDumpJob(*job);
	}

	delete job;
}

void BackendQuery_ResetSubmodels()
{
	for (int i = 0; i < QUERY_MAX_BODYPARTS; ++i)
	{
		g_submodels[i] = 0;
	}
}

void BackendQuery_NoteSubmodel(int group, int value)
{
	if (group >= 0 && group < QUERY_MAX_BODYPARTS)
	{
		g_submodels[group] = value;
	}
}

int BackendQuery_GetSubmodel(int group)
{
	if (group >= 0 && group < QUERY_MAX_BODYPARTS)
	{
		return g_submodels[group];
	}
	return 0;
}
