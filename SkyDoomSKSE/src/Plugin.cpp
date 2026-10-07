#include "RE/Skyrim.h"
#include "RE/S/SourceActionMap.h"
#include "RE/B/BSVisit.h"
#include "RE/B/bhkNiCollisionObject.h"
#include "RE/B/bhkRigidBody.h"
#include "RE/H/hkpRigidBody.h"
#include "PCH.h"
#include "skydoom_protocol.h"

#include <Windows.h>

#pragma comment(lib, "Advapi32.lib")

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef min
#	undef min
#endif

#ifdef max
#	undef max
#endif

namespace
{
	using Microsoft::WRL::ComPtr;

	// ========================================================
	// SKYDOOM CORE
	// ========================================================

	HANDLE g_mapping = nullptr;

	SkyDoomSharedState* g_state =
		nullptr;

	HANDLE g_doomProcess =
		nullptr;

	HANDLE g_doomJob =
		nullptr;

	std::atomic_bool g_running =
		false;

	std::atomic_bool g_taskPending =
		false;

	std::jthread g_updateThread;

	std::uint32_t g_inputSequence =
		0;

	// SKYDOOM_PHYSICAL_LMB_V4
	std::mutex g_inputRingMutex;

	bool g_physicalFireDown =
	    false;

    // ========================================================
    // SKYDOOM PISTOL COMBAT BRIDGE
    // ========================================================

    // SKYDOOM_PISTOL_COMBAT_BRIDGE_V4

    constexpr std::int32_t
        SKYDOOM_DOOM_WEAPON_PISTOL =
            1;


/*
        Maximum Skyrim-world distance for the DOOM pistol
        hitscan.

        8192 Skyrim units is a little over 100 metres.
    */

    constexpr float
        SKYDOOM_PISTOL_HITSCAN_RANGE =
            8192.0f;



    bool
        g_haveDoomCombatSnapshot =
            false;


    std::int32_t
        g_lastDoomBullets =
            0;


    std::int32_t
        g_lastDoomWeapon =
            -1;


    std::uint64_t
        g_lastDoomCombatTick =
            0;


	bool g_inputSinkRegistered =
		false;

	bool g_reportedDoomConnected =
		false;

	// SKYDOOM_PUBLIC_BETA_PORTABLE_PATHS_V16_0
	//
	// Public beta path policy:
	//   * The modified Chocolate Doom runtime ships beside the SKSE plugin
	//     under: SKSE\Plugins\SkyDoom\Runtime\
	//   * DOOM.WAD remains in the user's legitimate Steam installation.
	//   * No user-specific C:\SkyDoom or Steam path is compiled into the DLL.
	//
	// Advanced/testing overrides:
	//   SKYDOOM_DOOM_EXE  = full path to chocolate-doom.exe
	//   SKYDOOM_WAD_PATH  = full path to DOOM.WAD

	std::wstring g_doomExePath;
	std::wstring g_doomWorkingDirectory;
	std::wstring g_doomWadPath;

	bool g_skyDoomPortablePathsInitialised =
		false;


	bool SkyDoomFileExists(
		const std::wstring& a_path)
	{
		if (a_path.empty()) {
			return false;
		}

		const DWORD attributes =
			GetFileAttributesW(
				a_path.c_str());

		return
			attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}


	std::wstring SkyDoomParentDirectory(
		const std::wstring& a_path)
	{
		const auto slash =
			a_path.find_last_of(
				L"\\/");

		if (
			slash ==
			std::wstring::npos) {
			return {};
		}

		return
			a_path.substr(
				0,
				slash);
	}


	std::wstring SkyDoomJoinPath(
		const std::wstring& a_left,
		const std::wstring& a_right)
	{
		if (a_left.empty()) {
			return a_right;
		}

		if (a_right.empty()) {
			return a_left;
		}

		if (
			a_left.back() == L'\\' ||
			a_left.back() == L'/') {
			return
				a_left +
				a_right;
		}

		return
			a_left +
			L"\\" +
			a_right;
	}


	void SkyDoomNormaliseDirectory(
		std::wstring& a_path)
	{
		for (auto& ch : a_path) {
			if (ch == L'/') {
				ch = L'\\';
			}
		}

		while (
			a_path.size() > 3 &&
			!a_path.empty() &&
			(a_path.back() == L'\\' ||
			 a_path.back() == L'/')) {
			a_path.pop_back();
		}
	}


	void SkyDoomAppendUniquePath(
		std::vector<std::wstring>& a_paths,
		std::wstring a_path)
	{
		SkyDoomNormaliseDirectory(
			a_path);

		if (a_path.empty()) {
			return;
		}

		for (
			const auto& existing :
			a_paths) {
			if (
				_wcsicmp(
					existing.c_str(),
					a_path.c_str()) ==
				0) {
				return;
			}
		}

		a_paths.push_back(
			a_path);
	}


	bool SkyDoomGetEnvironmentPath(
		const wchar_t* a_name,
		std::wstring& a_result)
	{
		a_result.clear();

		std::vector<wchar_t>
			buffer(
				32768,
				L'\0');

		const DWORD length =
			GetEnvironmentVariableW(
				a_name,
				buffer.data(),
				static_cast<DWORD>(
					buffer.size()));

		if (
			length == 0 ||
			length >= buffer.size()) {
			return false;
		}

		a_result.assign(
			buffer.data(),
			length);

		return true;
	}


	bool SkyDoomReadRegistryString(
		HKEY a_root,
		const wchar_t* a_subKey,
		const wchar_t* a_valueName,
		REGSAM a_viewFlags,
		std::wstring& a_result)
	{
		a_result.clear();

		HKEY key =
			nullptr;

		const LSTATUS openResult =
			RegOpenKeyExW(
				a_root,
				a_subKey,
				0,
				KEY_READ |
					a_viewFlags,
				&key);

		if (
			openResult !=
			ERROR_SUCCESS) {
			return false;
		}

		wchar_t buffer[4096]{};
		DWORD type =
			0;
		DWORD byteCount =
			sizeof(
				buffer);

		const LSTATUS queryResult =
			RegQueryValueExW(
				key,
				a_valueName,
				nullptr,
				&type,
				reinterpret_cast<LPBYTE>(
					buffer),
				&byteCount);

		RegCloseKey(
			key);

		if (
			queryResult != ERROR_SUCCESS ||
			(type != REG_SZ &&
			 type != REG_EXPAND_SZ)) {
			return false;
		}

		buffer[
			(sizeof(buffer) /
			 sizeof(buffer[0])) - 1] =
			L'\0';

		a_result =
			buffer;

		if (
			type ==
			REG_EXPAND_SZ) {
			wchar_t expanded[4096]{};

			const DWORD expandedLength =
				ExpandEnvironmentStringsW(
					a_result.c_str(),
					expanded,
					static_cast<DWORD>(
						sizeof(expanded) /
						sizeof(expanded[0])));

			if (
				expandedLength > 0 &&
				expandedLength <
					(sizeof(expanded) /
					 sizeof(expanded[0]))) {
				a_result =
					expanded;
			}
		}

		return
			!a_result.empty();
	}


	bool SkyDoomReadWholeFile(
		const std::wstring& a_path,
		std::string& a_result)
	{
		a_result.clear();

		HANDLE file =
			CreateFileW(
				a_path.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ |
					FILE_SHARE_WRITE |
					FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

		if (
			file ==
			INVALID_HANDLE_VALUE) {
			return false;
		}

		LARGE_INTEGER size{};

		if (
			!GetFileSizeEx(
				file,
				&size) ||
			size.QuadPart < 0 ||
			size.QuadPart >
				(16ll * 1024ll * 1024ll)) {
			CloseHandle(
				file);

			return false;
		}

		a_result.resize(
			static_cast<std::size_t>(
				size.QuadPart));

		DWORD totalRead =
			0;

		while (
			totalRead <
			a_result.size()) {
			const std::size_t bytesRemaining =
				a_result.size() -
				totalRead;

			const DWORD remaining =
				static_cast<DWORD>(
					bytesRemaining >
							(1024u * 1024u) ?
						(1024u * 1024u) :
						bytesRemaining);

			DWORD justRead =
				0;

			if (
				!ReadFile(
					file,
					a_result.data() +
						totalRead,
					remaining,
					&justRead,
					nullptr)) {
				CloseHandle(
					file);

				a_result.clear();

				return false;
			}

			if (
				justRead ==
				0) {
				break;
			}

			totalRead +=
				justRead;
		}

		CloseHandle(
			file);

		a_result.resize(
			totalRead);

		return true;
	}


	std::wstring SkyDoomUtf8ToWide(
		const std::string& a_text)
	{
		if (a_text.empty()) {
			return {};
		}

		int length =
			MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				a_text.data(),
				static_cast<int>(
					a_text.size()),
				nullptr,
				0);

		UINT codePage =
			CP_UTF8;
		DWORD flags =
			MB_ERR_INVALID_CHARS;

		if (length <= 0) {
			codePage =
				CP_ACP;
			flags =
				0;

			length =
				MultiByteToWideChar(
					codePage,
					flags,
					a_text.data(),
					static_cast<int>(
						a_text.size()),
					nullptr,
					0);
		}

		if (length <= 0) {
			return {};
		}

		std::wstring result(
			static_cast<std::size_t>(
				length),
			L'\0');

		if (
			MultiByteToWideChar(
				codePage,
				flags,
				a_text.data(),
				static_cast<int>(
					a_text.size()),
				result.data(),
				length) <= 0) {
			return {};
		}

		return result;
	}


	// SKYDOOM_V16_0_UNICODE_PATH_LOGGING_R3
	//
	// Convert Windows UTF-16 paths to UTF-8 for spdlog/logger output.
	// Do not narrow wchar_t characters with std::string(begin, end):
	// that loses non-ASCII characters and caused MSVC warning C4244.
	std::string SkyDoomWideToUtf8(
		const std::wstring& a_text)
	{
		if (a_text.empty()) {
			return {};
		}

		const int length =
			WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				a_text.data(),
				static_cast<int>(
					a_text.size()),
				nullptr,
				0,
				nullptr,
				nullptr);

		if (length <= 0) {
			return {};
		}

		std::string result(
			static_cast<std::size_t>(
				length),
			'\0');

		if (
			WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				a_text.data(),
				static_cast<int>(
					a_text.size()),
				result.data(),
				length,
				nullptr,
				nullptr) <= 0) {
			return {};
		}

		return result;
	}

	void SkyDoomAddSteamLibrariesFromVdf(
		const std::wstring& a_steamRoot,
		std::vector<std::wstring>& a_roots)
	{
		const std::wstring vdfPath =
			SkyDoomJoinPath(
				a_steamRoot,
				L"steamapps\\libraryfolders.vdf");

		std::string text;

		if (
			!SkyDoomReadWholeFile(
				vdfPath,
				text)) {
			return;
		}

		std::size_t position =
			0;

		while (true) {
			position =
				text.find(
					"\"path\"",
					position);

			if (
				position ==
				std::string::npos) {
				break;
			}

			const std::size_t openQuote =
				text.find(
					'"',
					position + 6);

			if (
				openQuote ==
				std::string::npos) {
				break;
			}

			const std::size_t closeQuote =
				text.find(
					'"',
					openQuote + 1);

			if (
				closeQuote ==
				std::string::npos) {
				break;
			}

			const std::string raw =
				text.substr(
					openQuote + 1,
					closeQuote -
						openQuote -
						1);

			std::string unescaped;
			unescaped.reserve(
				raw.size());

			for (
				std::size_t i = 0;
				i < raw.size();
				++i) {
				if (
					raw[i] == '\\' &&
					i + 1 < raw.size() &&
					raw[i + 1] == '\\') {
					unescaped.push_back(
						'\\');

					++i;
				} else {
					unescaped.push_back(
						raw[i]);
				}
			}

			SkyDoomAppendUniquePath(
				a_roots,
				SkyDoomUtf8ToWide(
					unescaped));

			position =
				closeQuote + 1;
		}
	}


	std::vector<std::wstring>
	SkyDoomCollectSteamRoots()
	{
		std::vector<std::wstring>
			roots;

		std::wstring value;

		if (
			SkyDoomReadRegistryString(
				HKEY_CURRENT_USER,
				L"Software\\Valve\\Steam",
				L"SteamPath",
				0,
				value)) {
			SkyDoomAppendUniquePath(
				roots,
				value);
		}

		if (
			SkyDoomReadRegistryString(
				HKEY_LOCAL_MACHINE,
				L"SOFTWARE\\Valve\\Steam",
				L"InstallPath",
				KEY_WOW64_32KEY,
				value)) {
			SkyDoomAppendUniquePath(
				roots,
				value);
		}

		if (
			SkyDoomGetEnvironmentPath(
				L"ProgramFiles(x86)",
				value)) {
			SkyDoomAppendUniquePath(
				roots,
				SkyDoomJoinPath(
					value,
					L"Steam"));
		}

		if (
			SkyDoomGetEnvironmentPath(
				L"ProgramFiles",
				value)) {
			SkyDoomAppendUniquePath(
				roots,
				SkyDoomJoinPath(
					value,
					L"Steam"));
		}

		// libraryfolders.vdf normally lives under the primary Steam root
		// and lists all additional libraries (D:, other disks, etc.).
		const auto primaryRoots =
			roots;

		for (
			const auto& root :
			primaryRoots) {
			SkyDoomAddSteamLibrariesFromVdf(
				root,
				roots);
		}

		return roots;
	}


	bool SkyDoomGetPluginDirectory(
		std::wstring& a_directory)
	{
		a_directory.clear();

		HMODULE module =
			nullptr;

		if (
			!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(
					&g_mapping),
				&module)) {
			return false;
		}

		std::vector<wchar_t>
			buffer(
				32768,
				L'\0');

		const DWORD length =
			GetModuleFileNameW(
				module,
				buffer.data(),
				static_cast<DWORD>(
					buffer.size()));

		if (
			length == 0 ||
			length >= buffer.size()) {
			return false;
		}

		const std::wstring modulePath(
			buffer.data(),
			length);

		a_directory =
			SkyDoomParentDirectory(
				modulePath);

		return
			!a_directory.empty();
	}


	bool SkyDoomDiscoverRuntimeExe(
		std::wstring& a_exePath)
	{
		a_exePath.clear();

		std::wstring overridePath;

		if (
			SkyDoomGetEnvironmentPath(
				L"SKYDOOM_DOOM_EXE",
				overridePath) &&
			SkyDoomFileExists(
				overridePath)) {
			a_exePath =
				overridePath;

			return true;
		}

		std::wstring pluginDirectory;

		if (
			!SkyDoomGetPluginDirectory(
				pluginDirectory)) {
			return false;
		}

		const std::array<std::wstring, 3>
			candidates{
				SkyDoomJoinPath(
					pluginDirectory,
					L"SkyDoom\\Runtime\\chocolate-doom.exe"),
				SkyDoomJoinPath(
					pluginDirectory,
					L"SkyDoom\\chocolate-doom.exe"),
				SkyDoomJoinPath(
					pluginDirectory,
					L"chocolate-doom.exe")
			};

		for (
			const auto& candidate :
			candidates) {
			if (
				SkyDoomFileExists(
					candidate)) {
				a_exePath =
					candidate;

				return true;
			}
		}

		return false;
	}


	bool SkyDoomDiscoverDoomWad(
		std::wstring& a_wadPath)
	{
		a_wadPath.clear();

		std::wstring overridePath;

		if (
			SkyDoomGetEnvironmentPath(
				L"SKYDOOM_WAD_PATH",
				overridePath) &&
			SkyDoomFileExists(
				overridePath)) {
			a_wadPath =
				overridePath;

			return true;
		}

		const auto steamRoots =
			SkyDoomCollectSteamRoots();

		for (
			const auto& root :
			steamRoots) {
			const std::array<std::wstring, 3>
				candidates{
					SkyDoomJoinPath(
						root,
						L"steamapps\\common\\Ultimate Doom\\base\\DOOM.WAD"),
					SkyDoomJoinPath(
						root,
						L"steamapps\\common\\DOOM + DOOM II\\base\\DOOM.WAD"),
					SkyDoomJoinPath(
						root,
						L"steamapps\\common\\DOOM + DOOM II\\DOOM.WAD")
				};

			for (
				const auto& candidate :
				candidates) {
				if (
					SkyDoomFileExists(
						candidate)) {
					a_wadPath =
						candidate;

					return true;
				}
			}
		}

		return false;
	}


	bool InitialiseSkyDoomPortablePaths()
	{
		if (
			g_skyDoomPortablePathsInitialised) {
			return
				!g_doomExePath.empty() &&
				!g_doomWadPath.empty();
		}

		g_skyDoomPortablePathsInitialised =
			true;

		if (
			!SkyDoomDiscoverRuntimeExe(
				g_doomExePath)) {
			logger::error(
				"SkyDoom public beta: chocolate-doom.exe was not found. "
				"Expected SKSE\\Plugins\\SkyDoom\\Runtime\\chocolate-doom.exe");

			return false;
		}

		g_doomWorkingDirectory =
			SkyDoomParentDirectory(
				g_doomExePath);

		if (
			g_doomWorkingDirectory.empty()) {
			logger::error(
				"SkyDoom public beta: could not determine Chocolate Doom working directory");

			return false;
		}

		if (
			!SkyDoomDiscoverDoomWad(
				g_doomWadPath)) {
			logger::error(
				"SkyDoom public beta: DOOM.WAD was not found in the user's Steam libraries");

			return false;
		}

		logger::info(
			"SkyDoom public beta runtime: {}",
			SkyDoomWideToUtf8(
				g_doomExePath));

		logger::info(
			"SkyDoom public beta DOOM.WAD: {}",
			SkyDoomWideToUtf8(
				g_doomWadPath));

		return true;
	}

	constexpr std::uint64_t DOOM_FRESH_MS =
		1500;

	// ========================================================
	// D3D11 OVERLAY
	// ========================================================

	using PresentFn =
		HRESULT(__stdcall*)(
			IDXGISwapChain*,
			UINT,
			UINT);

	using ResizeBuffersFn =
		HRESULT(__stdcall*)(
			IDXGISwapChain*,
			UINT,
			UINT,
			UINT,
			DXGI_FORMAT,
			UINT);

	PresentFn g_originalPresent =
		nullptr;

	ResizeBuffersFn g_originalResizeBuffers =
		nullptr;

	ComPtr<ID3D11Device>
		g_device;

	ComPtr<ID3D11DeviceContext>
		g_context;

	ComPtr<ID3D11RenderTargetView>
		g_backBufferRTV;

	ComPtr<ID3D11Texture2D>
		g_overlayTexture;

	ComPtr<ID3D11ShaderResourceView>
		g_overlaySRV;

	ComPtr<ID3D11VertexShader>
		g_vertexShader;

	ComPtr<ID3D11PixelShader>
		g_pixelShader;

	ComPtr<ID3D11InputLayout>
		g_inputLayout;

	ComPtr<ID3D11Buffer>
		g_vertexBuffer;

	ComPtr<ID3D11SamplerState>
		g_pointSampler;

	ComPtr<ID3D11BlendState>
		g_alphaBlend;

	ComPtr<ID3D11DepthStencilState>
		g_depthDisabled;

	ComPtr<ID3D11RasterizerState>
		g_rasterizer;

	std::array<
		std::uint8_t,
		SKYDOOM_OVERLAY_RGBA_BYTES>
		g_overlayPixels{};

	std::uint64_t g_lastOverlayFrame =
		0;

	std::uint32_t g_backBufferWidth =
		0;

	std::uint32_t g_backBufferHeight =
		0;

	bool g_renderHookInstalled =
		false;

	// SKYDOOM_PRESENTATION_STATE_V4
	bool g_skyDoomDisabledFightingControls = false;
	bool g_skyDoomCulledFirstPerson = false;

	struct OverlayVertex
	{
		float x;
		float y;
		float z;

		float u;
		float v;
	};

	// ========================================================
	// SHADERS
	// ========================================================

	    // SKYDOOM_VISIBLE_ROCKET_SPRITE_V11_1

    // ========================================================

    // SKYDOOM_WAD_ENDIAN_FORWARD_DECLS_V11_2_FIX
    std::uint16_t SkyDoomReadLE16(
        const std::uint8_t* data
    );

    std::uint32_t SkyDoomReadLE32(
        const std::uint8_t* data
    );


    // SKYDOOM_ROCKET_EXPLOSION_V11_2

    //

    // Genuine Chocolate Doom rocket death animation:

    //

    //   S_EXPLODE1 = SPR_MISL frame B, 8 tics

    //   S_EXPLODE2 = SPR_MISL frame C, 6 tics

    //   S_EXPLODE3 = SPR_MISL frame D, 4 tics

    //

    // At 35 Hz:

    //

    //   B = 229 ms

    //   C = 171 ms

    //   D = 114 ms

    //

    // Total = approximately 514 ms.

    //

    // This is VISUAL ONLY.

    //

    // Existing v11 projectile collision, direct damage,

    // splash damage and self-damage remain authoritative.

    // ========================================================





    constexpr std::uint32_t

        SKYDOOM_MAX_ROCKET_EXPLOSIONS =

            16u;





    constexpr std::uint64_t

        SKYDOOM_ROCKET_EXPLODE_B_END_MS =

            229u;





    constexpr std::uint64_t

        SKYDOOM_ROCKET_EXPLODE_C_END_MS =

            400u;





    constexpr std::uint64_t

        SKYDOOM_ROCKET_EXPLODE_END_MS =

            515u;





    struct SkyDoomRocketExplosionVisual

    {

        bool active =

            false;





        RE::NiPoint3 position{};





        float doomToSkyrimScale =

            1.0f;





        std::uint64_t startMs =

            0;

    };





    std::mutex

        g_skyDoomRocketExplosionMutex;





    std::array<

        SkyDoomRocketExplosionVisual,

        SKYDOOM_MAX_ROCKET_EXPLOSIONS>

        g_skyDoomRocketExplosions{};





    std::array<ComPtr<ID3D11ShaderResourceView>, 51> g_skyDoomRocketExplosionSRVs{};





    std::array<std::uint32_t, 51> g_skyDoomRocketExplosionWidths{
            0u,
            0u,
            0u,
            0u,
            0u
        };





    std::array<std::uint32_t, 51> g_skyDoomRocketExplosionHeights{
            0u,
            0u,
            0u,
            0u,
            0u
        };





    bool

        g_skyDoomRocketExplosionLoadAttempted =

            false;





    void StartSkyDoomRocketExplosionVisual(

        const RE::NiPoint3& position,

        float doomToSkyrimScale

    )

    {

        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        std::lock_guard<std::mutex>

            lock(

                g_skyDoomRocketExplosionMutex

            );





        SkyDoomRocketExplosionVisual*

            selected =

                nullptr;





        for (

            auto& explosion :

                g_skyDoomRocketExplosions

        )

        {

            if (

                !explosion.active ||

                now -

                    explosion.startMs >=

                    SKYDOOM_ROCKET_EXPLODE_END_MS

            )

            {

                selected =

                    &explosion;



                break;

            }

        }





        if (!selected)

        {

            /*

                Extremely unlikely with only one launcher, but

                overwrite the oldest slot rather than dropping

                the impact visual completely.

            */



            selected =

                &g_skyDoomRocketExplosions[0];





            for (

                auto& explosion :

                    g_skyDoomRocketExplosions

            )

            {

                if (

                    explosion.startMs <

                    selected->startMs

                )

                {

                    selected =

                        &explosion;

                }

            }

        }





        selected->active =

            true;





        selected->position =

            position;





        selected->doomToSkyrimScale =

            doomToSkyrimScale;





        selected->startMs =

            now;





        SKSE::log::info(

            "[skydoomskse] REAL DOOM rocket explosion visual started"

        );

    }





    bool EnsureSkyDoomRocketExplosionTextures()

    {

        if (

            g_skyDoomRocketExplosionSRVs[0] &&
            g_skyDoomRocketExplosionSRVs[1] &&
            g_skyDoomRocketExplosionSRVs[2] &&
            g_skyDoomRocketExplosionSRVs[3] &&
            g_skyDoomRocketExplosionSRVs[4] &&
            g_skyDoomRocketExplosionSRVs[5] &&
            g_skyDoomRocketExplosionSRVs[6] &&
            g_skyDoomRocketExplosionSRVs[7] &&
            g_skyDoomRocketExplosionSRVs[8] &&
            g_skyDoomRocketExplosionSRVs[9] &&
            g_skyDoomRocketExplosionSRVs[10] &&
            g_skyDoomRocketExplosionSRVs[11] &&
            g_skyDoomRocketExplosionSRVs[12] &&
            g_skyDoomRocketExplosionSRVs[13] &&
            g_skyDoomRocketExplosionSRVs[14] &&
            g_skyDoomRocketExplosionSRVs[15] &&
            g_skyDoomRocketExplosionSRVs[16] &&
            g_skyDoomRocketExplosionSRVs[17] &&
            g_skyDoomRocketExplosionSRVs[18] &&
            g_skyDoomRocketExplosionSRVs[19] &&
            g_skyDoomRocketExplosionSRVs[20] &&
            g_skyDoomRocketExplosionSRVs[21] &&
            g_skyDoomRocketExplosionSRVs[22] &&
            g_skyDoomRocketExplosionSRVs[23] &&
            g_skyDoomRocketExplosionSRVs[24] &&
            g_skyDoomRocketExplosionSRVs[25] &&
            g_skyDoomRocketExplosionSRVs[26] &&
            g_skyDoomRocketExplosionSRVs[27] &&
            g_skyDoomRocketExplosionSRVs[28] &&
            g_skyDoomRocketExplosionSRVs[29] &&
            g_skyDoomRocketExplosionSRVs[30] &&
            g_skyDoomRocketExplosionSRVs[31] &&
            g_skyDoomRocketExplosionSRVs[32] &&
            g_skyDoomRocketExplosionSRVs[33] &&
            g_skyDoomRocketExplosionSRVs[34] &&
            g_skyDoomRocketExplosionSRVs[35] &&
            g_skyDoomRocketExplosionSRVs[36] &&
            g_skyDoomRocketExplosionSRVs[37]

        )

        {

            return true;

        }





        if (!g_device)

        {

            return false;

        }





        if (

            g_skyDoomRocketExplosionLoadAttempted

        )

        {

            return false;

        }





        g_skyDoomRocketExplosionLoadAttempted =

            true;





        HANDLE file =

            CreateFileW(

                g_doomWadPath.c_str(),

                GENERIC_READ,

                FILE_SHARE_READ,

                nullptr,

                OPEN_EXISTING,

                FILE_ATTRIBUTE_NORMAL,

                nullptr

            );





        if (

            file ==

            INVALID_HANDLE_VALUE

        )

        {

            SKSE::log::error(

                "[skydoomskse] Rocket explosion: could not open DOOM.WAD"

            );



            return false;

        }





        LARGE_INTEGER

            fileSize{};





        if (

            !GetFileSizeEx(

                file,

                &fileSize

            ) ||

            fileSize.QuadPart <

                12 ||

            fileSize.QuadPart >

                64ll *

                1024ll *

                1024ll

        )

        {

            CloseHandle(

                file

            );





            SKSE::log::error(

                "[skydoomskse] Rocket explosion: invalid DOOM.WAD size"

            );



            return false;

        }





        std::vector<std::uint8_t>

            wad(

                static_cast<std::size_t>(

                    fileSize.QuadPart

                )

            );





        std::size_t totalRead =

            0;





        while (

            totalRead <

            wad.size()

        )

        {

            const std::size_t remaining =

                wad.size() -

                totalRead;





            const DWORD request =

                static_cast<DWORD>(

                    remaining >

                        1024u *

                        1024u ?

                        1024u *

                        1024u :

                        remaining

                );





            DWORD bytesRead =

                0;





            if (

                !ReadFile(

                    file,

                    wad.data() +

                        totalRead,

                    request,

                    &bytesRead,

                    nullptr

                ) ||

                bytesRead ==

                    0

            )

            {

                CloseHandle(

                    file

                );





                SKSE::log::error(

                    "[skydoomskse] Rocket explosion: failed reading DOOM.WAD"

                );



                return false;

            }





            totalRead +=

                bytesRead;

        }





        CloseHandle(

            file

        );





        const std::uint32_t lumpCount =

            SkyDoomReadLE32(

                wad.data() +

                4

            );





        const std::uint32_t directoryOffset =

            SkyDoomReadLE32(

                wad.data() +

                8

            );





        if (

            lumpCount ==

                0 ||

            directoryOffset >=

                wad.size() ||

            static_cast<std::uint64_t>(

                directoryOffset

            ) +

                static_cast<std::uint64_t>(

                    lumpCount

                ) *

                16ull >

                wad.size()

        )

        {

            SKSE::log::error(

                "[skydoomskse] Rocket explosion: invalid WAD directory"

            );



            return false;

        }





        struct ExplosionLumpLocation

        {

            bool found =

                false;





            std::uint32_t offset =

                0;





            std::uint32_t size =

                0;





            char name[

                9

            ]{};

        };





        auto readDirectoryName =

            [

                &wad

            ](

                std::size_t entryOffset,

                char output[9]

            )

            {

                for (

                    std::uint32_t i = 0;

                    i <

                        8u;

                    ++i

                )

                {

                    output[i] =

                        static_cast<char>(

                            wad[

                                entryOffset +

                                8u +

                                i

                            ]

                        );

                }





                output[8] =

                    '\0';

            };





        auto findExact =

            [

                &wad,

                lumpCount,

                directoryOffset,

                &readDirectoryName

            ](

                const char* wanted

            ) -> ExplosionLumpLocation

            {

                for (

                    std::uint32_t i = 0;

                    i <

                        lumpCount;

                    ++i

                )

                {

                    const std::size_t entryOffset =

                        static_cast<std::size_t>(

                            directoryOffset

                        ) +

                        static_cast<std::size_t>(

                            i

                        ) *

                        16u;





                    char name[

                        9

                    ]{};





                    readDirectoryName(

                        entryOffset,

                        name

                    );





                    if (

                        std::strncmp(

                            name,

                            wanted,

                            8u

                        ) !=

                        0

                    )

                    {

                        continue;

                    }





                    ExplosionLumpLocation result{};





                    result.offset =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset

                        );





                    result.size =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset +

                            4u

                        );





                    if (

                        static_cast<std::uint64_t>(

                            result.offset

                        ) +

                            result.size >

                        wad.size()

                    )

                    {

                        return

                            ExplosionLumpLocation{};

                    }





                    std::memcpy(

                        result.name,

                        name,

                        9u

                    );





                    result.found =

                        true;





                    return result;

                }





                return

                    ExplosionLumpLocation{};

            };





        auto findFramePrefix =

            [

                &wad,

                lumpCount,

                directoryOffset,

                &readDirectoryName

            ](

                const char* prefix

            ) -> ExplosionLumpLocation

            {

                for (

                    std::uint32_t i = 0;

                    i <

                        lumpCount;

                    ++i

                )

                {

                    const std::size_t entryOffset =

                        static_cast<std::size_t>(

                            directoryOffset

                        ) +

                        static_cast<std::size_t>(

                            i

                        ) *

                        16u;





                    char name[

                        9

                    ]{};





                    readDirectoryName(

                        entryOffset,

                        name

                    );





                    if (

                        std::strncmp(

                            name,

                            prefix,

                            5u

                        ) !=

                        0

                    )

                    {

                        continue;

                    }





                    ExplosionLumpLocation result{};





                    result.offset =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset

                        );





                    result.size =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset +

                            4u

                        );





                    if (

                        static_cast<std::uint64_t>(

                            result.offset

                        ) +

                            result.size >

                        wad.size()

                    )

                    {

                        return

                            ExplosionLumpLocation{};

                    }





                    std::memcpy(

                        result.name,

                        name,

                        9u

                    );





                    result.found =

                        true;





                    return result;

                }





                return

                    ExplosionLumpLocation{};

            };





        const ExplosionLumpLocation paletteLump =

            findExact(

                "PLAYPAL"

            );





        if (

            !paletteLump.found ||

            paletteLump.size <

                768u

        )

        {

            SKSE::log::error(

                "[skydoomskse] Rocket explosion: PLAYPAL not found"

            );



            return false;

        }





        std::array<ExplosionLumpLocation, 51> frameLumps{};





        frameLumps[0] =

            findExact(

                "MISLB0"

            );





        frameLumps[1] =

            findExact(

                "MISLC0"

            );





        frameLumps[2] =

            findExact(

                "MISLD0"

            );
        frameLumps[3] =
            findExact(
                "PLSSA0"
            );

        frameLumps[4] =
            findExact(
                "PLSSB0"
            );

        frameLumps[5] =

            findExact("PLSEA0");



        frameLumps[6] =

            findExact("PLSEB0");



        frameLumps[7] =

            findExact("PLSEC0");



        frameLumps[8] =

            findExact("PLSED0");



        frameLumps[9] =

            findExact("PLSEE0");

        // SKYDOOM_VISIBLE_BFG_WAD_V14_2A

        frameLumps[10] =

            findExact("BFS1A0");



        frameLumps[11] =

            findExact("BFS1B0");
        // SKYDOOM_BFG_COLLISION_IMPACT_V14_2B
        frameLumps[12] =
            findExact("BFE1A0");

        frameLumps[13] =
            findExact("BFE1B0");

        frameLumps[14] =
            findExact("BFE1C0");

        frameLumps[15] =
            findExact("BFE1D0");

        frameLumps[16] =
            findExact("BFE1E0");

        frameLumps[17] =
            findExact("BFE1F0");
        // SKYDOOM_BFG_DAMAGE_SPRAY_V14_3
        frameLumps[18] =
            findExact("BFE2A0");

        frameLumps[19] =
            findExact("BFE2B0");

        frameLumps[20] =
            findExact("BFE2C0");

        frameLumps[21] =
            findExact("BFE2D0");
        // SKYDOOM_PHYSICAL_PICKUPS_V15
        frameLumps[22] =
            findExact("MEDIA0");
        // SKYDOOM_RESOURCE_DROPS_V15_3
        // State frame 0 => A0 for these genuine Doom pickup sprites.
        frameLumps[23] =
            findExact("ARM1A0");

        frameLumps[24] =
            findExact("CLIPA0");

        frameLumps[25] =
            findExact("SHELA0");

        frameLumps[26] =
            findExact("ROCKA0");

        frameLumps[27] =
            findExact("CELLA0");
        // SKYDOOM_ARMOR_BONUS_V15_5
        // Native BON2 animation states use frames A/B/C/D/C/B.
        frameLumps[28] =
            findExact("BON2A0");

        frameLumps[29] =
            findExact("BON2B0");

        frameLumps[30] =
            findExact("BON2C0");

        frameLumps[31] =
            findExact("BON2D0");
        // SKYDOOM_COMPLETE_ITEMS_V15_9A1
        frameLumps[32] =
            findExact("STIMA0");

        frameLumps[33] =
            findExact("ARM2A0");

        frameLumps[34] =
            findExact("AMMOA0");

        frameLumps[35] =
            findExact("SBOXA0");

        frameLumps[36] =
            findExact("BROKA0");

        frameLumps[37] =
            findExact("CELPA0");
        // SKYDOOM_COMPLETE_ITEMS_V15_9B1
        frameLumps[38] =
            findExact("BON1A0");

        frameLumps[39] =
            findExact("BON1B0");

        frameLumps[40] =
            findExact("BON1C0");

        frameLumps[41] =
            findExact("BON1D0");

        frameLumps[42] =
            findExact("SOULA0");

        frameLumps[43] =
            findExact("SOULB0");

        frameLumps[44] =
            findExact("SOULC0");

        frameLumps[45] =
            findExact("SOULD0");

        frameLumps[46] =
            findExact("BPAKA0");

        frameLumps[47] =
            findExact("MEGAA0");

        frameLumps[48] =
            findExact("MEGAB0");

        frameLumps[49] =
            findExact("MEGAC0");

        frameLumps[50] =
            findExact("MEGAD0");





        if (!frameLumps[0].found)

        {

            frameLumps[0] =

                findFramePrefix(

                    "MISLB"

                );

        }





        if (!frameLumps[1].found)

        {

            frameLumps[1] =

                findFramePrefix(

                    "MISLC"

                );

        }





        if (!frameLumps[2].found)

        {

            frameLumps[2] =

                findFramePrefix(

                    "MISLD"

                );

        }





        for (

            std::uint32_t frameIndex = 0;

            frameIndex < 51u;

            ++frameIndex

        )

        {

            if (

                !frameLumps[

                    frameIndex

                ].found

            )

            {
                /* SKYDOOM_OPTIONAL_PICKUP_SPRITES_V15_9B1_R6_MISSING */
                if (frameIndex >= 38u)
                {
                    SKSE::log::warn(
                        "[skydoomskse] Optional DOOM pickup WAD frame {} is absent; skipping it",
                        frameIndex
                    );

                    continue;
                }

                SKSE::log::error(

                    "[skydoomskse] Rocket explosion: required MISL explosion frame {} not found",

                    frameIndex

                );



                return false;

            }

        }





        const auto* palette =

            wad.data() +

            paletteLump.offset;





        auto createTextureFromPatch =

            [

                &wad,

                palette

            ](

                const ExplosionLumpLocation& lump,

                ComPtr<ID3D11ShaderResourceView>& outputSRV,

                std::uint32_t& outputWidth,

                std::uint32_t& outputHeight

            ) -> bool

            {

                if (

                    lump.size <

                    12u

                )

                {

                    return false;

                }





                const auto* patch =

                    wad.data() +

                    lump.offset;





                const std::uint32_t width =

                    SkyDoomReadLE16(

                        patch +

                        0

                    );





                const std::uint32_t height =

                    SkyDoomReadLE16(

                        patch +

                        2

                    );





                if (

                    width ==

                        0 ||

                    height ==

                        0 ||

                    width >

                        512u ||

                    height >

                        512u ||

                    8u +

                        width *

                        4u >

                        lump.size

                )

                {

                    return false;

                }





                std::vector<std::uint8_t>

                    rgba(

                        static_cast<std::size_t>(

                            width

                        ) *

                        static_cast<std::size_t>(

                            height

                        ) *

                        4u,

                        0u

                    );





                const std::size_t lumpStart =

                    lump.offset;





                const std::size_t lumpEnd =

                    lumpStart +

                    lump.size;





                for (

                    std::uint32_t x = 0;

                    x <

                        width;

                    ++x

                )

                {

                    const std::uint32_t columnOffset =

                        SkyDoomReadLE32(

                            patch +

                            8u +

                            x *

                            4u

                        );





                    std::size_t pos =

                        lumpStart +

                        columnOffset;





                    if (

                        pos >=

                        lumpEnd

                    )

                    {

                        continue;

                    }





                    while (

                        pos <

                        lumpEnd

                    )

                    {

                        const std::uint8_t topDelta =

                            wad[

                                pos++

                            ];





                        if (

                            topDelta ==

                            0xFFu

                        )

                        {

                            break;

                        }





                        if (

                            pos +

                            2u >

                            lumpEnd

                        )

                        {

                            break;

                        }





                        const std::uint8_t length =

                            wad[

                                pos++

                            ];





                        ++pos;





                        if (

                            pos +

                            length +

                            1u >

                            lumpEnd

                        )

                        {

                            break;

                        }





                        for (

                            std::uint32_t y = 0;

                            y <

                                length;

                            ++y

                        )

                        {

                            const std::uint32_t destinationY =

                                static_cast<std::uint32_t>(

                                    topDelta

                                ) +

                                y;





                            if (

                                destinationY >=

                                height

                            )

                            {

                                continue;

                            }





                            const std::uint8_t paletteIndex =

                                wad[

                                    pos +

                                    y

                                ];





                            const std::size_t destination =

                                (

                                    static_cast<std::size_t>(

                                        destinationY

                                    ) *

                                    width +

                                    x

                                ) *

                                4u;





                            rgba[

                                destination +

                                0u

                            ] =

                                palette[

                                    paletteIndex *

                                    3u +

                                    0u

                                ];





                            rgba[

                                destination +

                                1u

                            ] =

                                palette[

                                    paletteIndex *

                                    3u +

                                    1u

                                ];





                            rgba[

                                destination +

                                2u

                            ] =

                                palette[

                                    paletteIndex *

                                    3u +

                                    2u

                                ];





                            rgba[

                                destination +

                                3u

                            ] =

                                255u;

                        }





                        pos +=

                            length;





                        ++pos;

                    }

                }





                D3D11_TEXTURE2D_DESC textureDesc{};





                textureDesc.Width =

                    width;





                textureDesc.Height =

                    height;





                textureDesc.MipLevels =

                    1;





                textureDesc.ArraySize =

                    1;





                textureDesc.Format =

                    DXGI_FORMAT_R8G8B8A8_UNORM;





                textureDesc.SampleDesc.Count =

                    1;





                textureDesc.Usage =

                    D3D11_USAGE_IMMUTABLE;





                textureDesc.BindFlags =

                    D3D11_BIND_SHADER_RESOURCE;





                D3D11_SUBRESOURCE_DATA initialData{};





                initialData.pSysMem =

                    rgba.data();





                initialData.SysMemPitch =

                    width *

                    4u;





                ComPtr<ID3D11Texture2D>

                    texture;





                if (

                    FAILED(

                        g_device->

                            CreateTexture2D(

                                &textureDesc,

                                &initialData,

                                &texture

                            )

                    )

                )

                {

                    return false;

                }





                ComPtr<ID3D11ShaderResourceView>

                    srv;





                if (

                    FAILED(

                        g_device->

                            CreateShaderResourceView(

                                texture.Get(),

                                nullptr,

                                &srv

                            )

                    )

                )

                {

                    return false;

                }





                outputSRV =

                    srv;





                outputWidth =

                    width;





                outputHeight =

                    height;





                return true;

            };





        for (

            std::uint32_t frameIndex = 0;

            frameIndex < 51u;

            ++frameIndex

        )

        {

            if (

                !createTextureFromPatch(

                    frameLumps[

                        frameIndex

                    ],

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ],

                    g_skyDoomRocketExplosionWidths[

                        frameIndex

                    ],

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                )

            )

            {
                /* SKYDOOM_OPTIONAL_PICKUP_SPRITES_V15_9B1_R6_DECODE */
                if (frameIndex >= 38u)
                {
                    g_skyDoomRocketExplosionSRVs[
                        frameIndex
                    ].Reset();

                    g_skyDoomRocketExplosionWidths[
                        frameIndex
                    ] = 0u;

                    g_skyDoomRocketExplosionHeights[
                        frameIndex
                    ] = 0u;

                    SKSE::log::warn(
                        "[skydoomskse] Optional DOOM pickup WAD frame {} failed texture decoding; skipping it",
                        frameIndex
                    );

                    continue;
                }

                SKSE::log::error(

                    "[skydoomskse] Rocket explosion: failed decoding {}",

                    frameLumps[

                        frameIndex

                    ].name

                );



                return false;

            }





            SKSE::log::info(

                "[skydoomskse] REAL DOOM rocket explosion frame loaded: {} {}x{}",

                frameLumps[

                    frameIndex

                ].name,

                g_skyDoomRocketExplosionWidths[

                    frameIndex

                ],

                g_skyDoomRocketExplosionHeights[

                    frameIndex

                ]

            );

        }





        return true;

    }





    void DrawSkyDoomRocketExplosionsV11_2()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomRocketExplosionVisual,

            SKYDOOM_MAX_ROCKET_EXPLOSIONS>

            explosions{};





        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomRocketExplosionMutex

                );





            explosions =

                g_skyDoomRocketExplosions;

        }





        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        bool haveExplosion =

            false;





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                explosion.active &&

                now >=

                    explosion.startMs &&

                now -

                    explosion.startMs <

                    SKYDOOM_ROCKET_EXPLODE_END_MS

            )

            {

                haveExplosion =

                    true;



                break;

            }

        }





        if (!haveExplosion)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                !explosion.active ||

                now <

                    explosion.startMs

            )

            {

                continue;

            }





            const std::uint64_t elapsed =

                now -

                explosion.startMs;





            if (

                elapsed >=

                SKYDOOM_ROCKET_EXPLODE_END_MS

            )

            {

                continue;

            }





            std::uint32_t frameIndex =

                0u;





            if (

                elapsed >=

                SKYDOOM_ROCKET_EXPLODE_C_END_MS

            )

            {

                frameIndex =

                    2u;

            }

            else if (

                elapsed >=

                SKYDOOM_ROCKET_EXPLODE_B_END_MS

            )

            {

                frameIndex =

                    1u;

            }





            float screenX =

                0.0f;





            float screenY =

                0.0f;





            float screenZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        explosion.position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }





            const float halfWorldHeight =

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                ) *

                explosion.

                    doomToSkyrimScale *

                0.5f;





            const RE::NiPoint3 topWorld =

                explosion.position +

                cameraUp *

                    halfWorldHeight;





            float topX =

                0.0f;





            float topY =

                0.0f;





            float topZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );





            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }





            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }





            const float aspect =

                static_cast<float>(

                    g_skyDoomRocketExplosionWidths[

                        frameIndex

                    ]

                ) /

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                );





            const float fullWidthPixels =

                fullHeightPixels *

                aspect;





            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;





            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;





            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;





            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;





            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };





            D3D11_MAPPED_SUBRESOURCE mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();





            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }





    // ========================================================

    // SKYDOOM_VISIBLE_PLASMA_V12_2A

    //

    // Visual-only first plasma projectile pass.

    //

    // Chocolate Doom remains authoritative for:

    //   - weapon ownership

    //   - cell ammo consumption

    //   - firing cadence

    //   - genuine A_FirePlasma events

    //

    // This renderer uses the real DOOM PLSSA0 / PLSSB0 sprites.

    //

    // MT_PLASMA native speed:

    //   25 DOOM units/tic * 35 tics/sec = 875 units/sec.

    //

    // Collision and damage are intentionally NOT added in 12.2A.

    // They will be layered on after the visual flight path is

    // confirmed in Skyrim, keeping the working rocket system frozen.

    // ========================================================



    constexpr std::uint32_t

        SKYDOOM_MAX_ACTIVE_PLASMA_VISUALS =

            64u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_VISUAL_LIFETIME_MS =

            3000u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_FRAME_MS =

            171u;



    struct SkyDoomPlasmaVisualV12_2A

    {

        bool active =

            false;



        RE::NiPoint3 origin{};



        RE::NiPoint3 direction{};



        float doomToSkyrimScale =

            1.0f;
        // SKYDOOM_PLASMA_COLLISION_V12_2B
        float stopDistance =
            0.0f;

        std::uint32_t collisionTargetFormID =
            0u;

        bool collisionActorHit =
            false;

        bool collisionWorldHit =
            false;



        std::uint64_t startMs =

            0;

    };



    std::mutex

        g_skyDoomPlasmaVisualMutexV12_2A;



    std::array<

        SkyDoomPlasmaVisualV12_2A,

        SKYDOOM_MAX_ACTIVE_PLASMA_VISUALS>

        g_skyDoomPlasmaVisualsV12_2A{};



    void DrawSkyDoomPlasmaSpritesV12_2()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }



        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }



        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }



        auto* worldCamera =

            RE::Main::

                WorldRootCamera();



        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();



        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }



        std::array<

            SkyDoomPlasmaVisualV12_2A,

            SKYDOOM_MAX_ACTIVE_PLASMA_VISUALS>

            visuals{};



        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomPlasmaVisualMutexV12_2A

                );



            visuals =

                g_skyDoomPlasmaVisualsV12_2A;

        }



        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        bool haveVisiblePlasma =

            false;



        for (

            const auto& plasma :

                visuals

        )

        {

            if (

                plasma.active &&

                now >=

                    plasma.startMs &&

                now -

                    plasma.startMs <

                    SKYDOOM_PLASMA_VISUAL_LIFETIME_MS

            )

            {

                haveVisiblePlasma =

                    true;



                break;

            }

        }



        if (!haveVisiblePlasma)

        {

            return;

        }



        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();



        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );



        D3D11_VIEWPORT

            viewport{};



        viewport.TopLeftX =

            0.0f;



        viewport.TopLeftY =

            0.0f;



        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );



        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );



        viewport.MinDepth =

            0.0f;



        viewport.MaxDepth =

            1.0f;



        g_context->

            RSSetViewports(

                1,

                &viewport

            );



        const UINT stride =

            sizeof(

                OverlayVertex

            );



        const UINT offset =

            0;



        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();



        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );



        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );



        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );



        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );



        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );



        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();



        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );



        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };



        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );



        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );



        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );



        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };



        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }



        for (

            const auto& plasma :

                visuals

        )

        {

            if (

                !plasma.active ||

                now <

                    plasma.startMs

            )

            {

                continue;

            }



            const std::uint64_t elapsed =

                now -

                plasma.startMs;



            if (

                elapsed >=

                SKYDOOM_PLASMA_VISUAL_LIFETIME_MS

            )

            {

                continue;

            }



            const std::uint32_t

                frameIndex =

                    3u +

                    static_cast<std::uint32_t>(

                        (

                            elapsed /

                            SKYDOOM_PLASMA_FRAME_MS

                        ) &

                        1u

                    );



            const float elapsedSeconds =

                static_cast<float>(

                    elapsed

                ) /

                1000.0f;



            const float speed =
                875.0f *
                plasma.
                    doomToSkyrimScale;

            const float travelDistance =
                speed *
                elapsedSeconds;

            if (
                travelDistance >=
                plasma.
                    stopDistance
            )
            {
                continue;
            }

            const RE::NiPoint3 position =
                plasma.origin +
                plasma.direction *
                    travelDistance;



            float screenX =

                0.0f;



            float screenY =

                0.0f;



            float screenZ =

                0.0f;



            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }



            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }



            const auto spriteHeight =

                g_skyDoomRocketExplosionHeights[

                    frameIndex

                ];



            const auto spriteWidth =

                g_skyDoomRocketExplosionWidths[

                    frameIndex

                ];



            if (

                spriteWidth ==

                    0u ||

                spriteHeight ==

                    0u

            )

            {

                continue;

            }



            const float halfWorldHeight =

                static_cast<float>(

                    spriteHeight

                ) *

                plasma.

                    doomToSkyrimScale *

                0.5f;



            const RE::NiPoint3 topWorld =

                position +

                cameraUp *

                    halfWorldHeight;



            float topX =

                0.0f;



            float topY =

                0.0f;



            float topZ =

                0.0f;



            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }



            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );



            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );



            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );



            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }



            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }



            const float aspect =

                static_cast<float>(

                    spriteWidth

                ) /

                static_cast<float>(

                    spriteHeight

                );



            const float fullWidthPixels =

                fullHeightPixels *

                aspect;



            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );



            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );



            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;



            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;



            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;



            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;



            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;



            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;



            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;



            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;



            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };



            D3D11_MAPPED_SUBRESOURCE

                mapped{};



            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }



            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );



            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );



            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();



            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );



            g_context->

                Draw(

                    6,

                    0

                );

        }

    }



    void DrawSkyDoomRocketSpritesV11_1();
    void DrawSkyDoomRocketExplosionsV11_2();
    void DrawSkyDoomPlasmaSpritesV12_2();
    void DrawSkyDoomPlasmaImpactsV13();
    void DrawSkyDoomBFGSpritesV14_2A();
    void DrawSkyDoomBFGImpactsV14_2B();
    void DrawSkyDoomBFGExtrasV14_3();
    void DrawSkyDoomPhysicalPickupsV15();



constexpr const char SKYDOOM_VERTEX_SHADER[] = R"(
        struct VSInput
        {
            float3 position : POSITION;
            float2 texcoord : TEXCOORD0;
        };

        struct VSOutput
        {
            float4 position : SV_POSITION;
            float2 texcoord : TEXCOORD0;
        };

        VSOutput main(VSInput input)
        {
            VSOutput output;

            output.position =
                float4(
                    input.position,
                    1.0
                );

            output.texcoord =
                input.texcoord;

            return output;
        }
    )";

	constexpr const char SKYDOOM_PIXEL_SHADER[] = R"(
        Texture2D overlayTexture : register(t0);

        SamplerState overlaySampler : register(s0);

        struct PSInput
        {
            float4 position : SV_POSITION;
            float2 texcoord : TEXCOORD0;
        };

        float4 main(PSInput input) : SV_TARGET
        {
            return overlayTexture.Sample(
                overlaySampler,
                input.texcoord
            );
        }
    )";

	// ========================================================
	// LOGGING
	// ========================================================

	void SetupLog()
	{
		auto logsFolder =
			logger::log_directory();

		if (!logsFolder) {
			util::report_and_fail(
				"SKSE log_directory not provided, logs can't be written");
		}

		const auto* plugin =
			SKSE::PluginDeclaration::
				GetSingleton();

		const auto logName =
			plugin ?
				std::string{
					plugin->GetName()
				} +
					".log" :
				"Plugin.log";

		auto logPath =
			*logsFolder /
			logName;

		auto fileSink =
			std::make_shared<
				spdlog::sinks::
					basic_file_sink_mt>(
				logPath.string(),
				true);

		std::vector<
			spdlog::sink_ptr>
			sinks{
				fileSink
			};

		if (IsDebuggerPresent()) {
			sinks.push_back(
				std::make_shared<
					spdlog::sinks::
						msvc_sink_mt>());
		}

		auto spdlogger =
			std::make_shared<
				spdlog::logger>(
				"global",
				sinks.begin(),
				sinks.end());

		spdlog::set_default_logger(
			std::move(
				spdlogger));

		spdlog::set_pattern(
			"[%H:%M:%S.%e] [%l] [%s:%#] %v");

#ifdef NDEBUG

		spdlog::set_level(
			spdlog::level::info);

#else

		spdlog::set_level(
			spdlog::level::trace);

#endif

		spdlog::flush_on(
			spdlog::level::info);
	}

	// ========================================================
	// SHARED MEMORY
	// ========================================================

	void InitializeProtocolIfNeeded(
		bool newlyCreated)
	{
		if (!g_state) {
			return;
		}

		if (
			newlyCreated ||
			g_state->magic !=
				SKYDOOM_MAGIC ||
			g_state->version !=
				SKYDOOM_VERSION ||
			g_state->struct_size !=
				sizeof(
					SkyDoomSharedState)) {
			ZeroMemory(
				g_state,
				sizeof(
					SkyDoomSharedState));

			g_state->magic =
				SKYDOOM_MAGIC;

			g_state->version =
				SKYDOOM_VERSION;

			g_state->struct_size =
				static_cast<
					std::uint32_t>(
					sizeof(
						SkyDoomSharedState));

			g_state->protocol_flags =
				SKYDOOM_PROTOCOL_FLAG_GUEST_MODE;

			g_state->overlay.width =
				SKYDOOM_OVERLAY_WIDTH;

			g_state->overlay.height =
				SKYDOOM_OVERLAY_HEIGHT;

			g_state->overlay.flags =
				SKYDOOM_OVERLAY_FLAG_WEAPON;
		}
	}

	bool InitSharedMemory()
	{
		g_mapping =
			CreateFileMappingA(
				INVALID_HANDLE_VALUE,
				nullptr,
				PAGE_READWRITE,
				0,
				sizeof(
					SkyDoomSharedState),
				SKYDOOM_MAPPING_NAME);

		if (!g_mapping) {
			logger::error(
				"CreateFileMappingA failed. Win32 error: {}",
				GetLastError());

			return false;
		}

		const DWORD creationStatus =
			GetLastError();

		const bool newlyCreated =
			creationStatus !=
			ERROR_ALREADY_EXISTS;

		g_state =
			static_cast<
				SkyDoomSharedState*>(
				MapViewOfFile(
					g_mapping,
					FILE_MAP_ALL_ACCESS,
					0,
					0,
					sizeof(
						SkyDoomSharedState)));

		if (!g_state) {
			logger::error(
				"MapViewOfFile failed. Win32 error: {}",
				GetLastError());

			CloseHandle(
				g_mapping);

			g_mapping =
				nullptr;

			return false;
		}

		InitializeProtocolIfNeeded(
			newlyCreated);

		g_state->skyrim.running =
			1;

		g_state->skyrim.pid =
			GetCurrentProcessId();

		g_state->skyrim.heartbeat_ms =
			GetTickCount64();

		logger::info(
			"Connected to SkyDoom Visual Overlay v4 shared memory");

		return true;
	}

	// ========================================================
	// INPUT RING
	// ========================================================

	void PushInputEvent(
		std::uint16_t type,
		std::uint16_t code,
		std::int32_t  value)
	{
        // SKYDOOM_INPUT_RING_LOCK_V4
        std::scoped_lock inputLock(
            g_inputRingMutex
        );
		if (!g_state) {
			return;
		}

		auto& ring =
			g_state->input;

		const std::uint32_t head =
			ring.head;

		const std::uint32_t tail =
			ring.tail;

		if (
			(head - tail) >=
			SKYDOOM_INPUT_RING_ENTRIES) {
			ring.dropped++;

			return;
		}

		auto& event =
			ring.events[head &
						SKYDOOM_INPUT_RING_MASK];

		event.type =
			type;

		event.code =
			code;

		event.value =
			value;

		event.sequence =
			++g_inputSequence;

		event.reserved =
			0;

		MemoryBarrier();

		ring.head =
			head + 1u;
	}

	class SkyDoomInputSink final :
		public RE::BSTEventSink<
			RE::InputEvent*>
	{
	public:
		static SkyDoomInputSink*
			GetSingleton()
		{
			static SkyDoomInputSink
				singleton;

			return &singleton;
		}

		RE::BSEventNotifyControl
			ProcessEvent(
				RE::InputEvent* const*
					a_eventList,
				RE::BSTEventSource<
					RE::InputEvent*>*) override
		{
			if (
				!g_state ||
				!a_eventList ||
				!*a_eventList) {
				return RE::BSEventNotifyControl::
					kContinue;
			}

			auto* ui =
				RE::UI::GetSingleton();

			if (
				ui &&
				ui->GameIsPaused()) {
				return RE::BSEventNotifyControl::
					kContinue;
			}

			auto* userEvents =
				RE::UserEvents::
					GetSingleton();

			if (!userEvents) {
				return RE::BSEventNotifyControl::
					kContinue;
			}

			for (
				auto* event =
					*a_eventList;
				event;
				event =
					event->next) {
				auto* button =
					event->AsButtonEvent();

				if (!button) {
					continue;
				}

				std::int32_t state;

				if (
					button->IsDown()) {
					state =
						1;
				} else if (
					button->IsUp()) {
					state =
						0;
				} else {
					continue;
				}

				const auto& name =
					button->QUserEvent();

				std::uint16_t code =
					0;

				if (
					name ==
					userEvents->forward) {
					code =
						SKYDOOM_INPUT_FORWARD;
				} else if (
					name ==
					userEvents->back) {
					code =
						SKYDOOM_INPUT_BACK;
				} else if (
					name ==
					userEvents->strafeLeft) {
					code =
						SKYDOOM_INPUT_STRAFE_LEFT;
				} else if (
					name ==
					userEvents->strafeRight) {
					code =
						SKYDOOM_INPUT_STRAFE_RIGHT;
				} else if (
					name ==
						userEvents->sprint ||
					name ==
						userEvents->run) {
					code =
						SKYDOOM_INPUT_RUN;
				} else if (
					name ==
					userEvents->rightAttack) {
					code =
						SKYDOOM_INPUT_FIRE;
				} else if (
					name ==
					userEvents->activate) {
					code =
						SKYDOOM_INPUT_USE;
				} else if (
					name ==
					userEvents->jump) {
					code =
						SKYDOOM_INPUT_JUMP;
				}

				if (code != 0) {
					PushInputEvent(
						SKYDOOM_INPUT_EVENT_BUTTON,
						code,
						state);
				}
			}

			return RE::BSEventNotifyControl::
				kContinue;
		}
	};

	bool RegisterInputSink()
	{
		if (
			g_inputSinkRegistered) {
			return true;
		}

		auto* inputManager =
			RE::BSInputDeviceManager::
				GetSingleton();

		if (!inputManager) {
			logger::error(
				"BSInputDeviceManager unavailable");

			return false;
		}

		inputManager->AddEventSink(
			SkyDoomInputSink::
				GetSingleton());

		g_inputSinkRegistered =
			true;

		logger::info(
			"SkyDoom input sink registered");

		return true;
	}

	// ========================================================
	// HIDDEN DOOM GUEST
	// ========================================================

	bool DoomHeartbeatIsFresh()
	{
		if (
			!g_state ||
			!g_state->doom.running) {
			return false;
		}

		const auto heartbeat =
			g_state->doom.heartbeat_ms;

		if (
			heartbeat == 0) {
			return false;
		}

		const auto now =
			static_cast<
				std::uint64_t>(
				GetTickCount64());

		if (
			now < heartbeat) {
			return true;
		}

		return (now - heartbeat) <=
		       DOOM_FRESH_MS;
	}

	bool LaunchDoomGuest()
	{
		if (g_doomProcess) {
			DWORD exitCode =
				0;

			if (
				GetExitCodeProcess(
					g_doomProcess,
					&exitCode) &&
				exitCode ==
					STILL_ACTIVE) {
				return true;
			}

			CloseHandle(
				g_doomProcess);

			g_doomProcess =
				nullptr;
		}

		if (!g_doomJob) {
			g_doomJob =
				CreateJobObjectW(
					nullptr,
					nullptr);

			if (!g_doomJob) {
				logger::error(
					"CreateJobObjectW failed. Win32 error: {}",
					GetLastError());

				return false;
			}

			JOBOBJECT_EXTENDED_LIMIT_INFORMATION
			jobInfo{};

			jobInfo.BasicLimitInformation.LimitFlags =
				JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

			if (
				!SetInformationJobObject(
					g_doomJob,
					JobObjectExtendedLimitInformation,
					&jobInfo,
					sizeof(
						jobInfo))) {
				logger::error(
					"SetInformationJobObject failed. Win32 error: {}",
					GetLastError());

				CloseHandle(
					g_doomJob);

				g_doomJob =
					nullptr;

				return false;
			}
		}

		std::wstring commandLine;

		commandLine +=
			L"\"";

		commandLine +=
			g_doomExePath;

		commandLine +=
			L"\" -iwad \"";

		commandLine +=
			g_doomWadPath;

		commandLine +=
			L"\"";

		commandLine +=
			L" -warp 1 1";

		commandLine +=
			L" -nomonsters";

		/*
    SKYDOOM_MUSIC_TOGGLE_V15_8_R2

    Do NOT launch Chocolate Doom with -nomusic.

    v15.8 now owns music muting dynamically through F10, so the
    guest must initialize its music backend normally at startup.
*/

		commandLine +=
			L" -nomouse";

		commandLine +=
			L" -window";

		commandLine +=
			L" -skydoomguest";

		STARTUPINFOW
		startupInfo{};

		startupInfo.cb =
			sizeof(
				startupInfo);

		startupInfo.dwFlags =
			STARTF_USESHOWWINDOW;

		startupInfo.wShowWindow =
			SW_HIDE;

		PROCESS_INFORMATION
		processInfo{};

		const DWORD creationFlags =
			CREATE_SUSPENDED |
			CREATE_NO_WINDOW;

		if (
			!CreateProcessW(
				g_doomExePath.c_str(),
				commandLine.data(),
				nullptr,
				nullptr,
				FALSE,
				creationFlags,
				nullptr,
				g_doomWorkingDirectory.c_str(),
				&startupInfo,
				&processInfo)) {
			logger::error(
				"CreateProcessW failed. Win32 error: {}",
				GetLastError());

			return false;
		}

		if (
			!AssignProcessToJobObject(
				g_doomJob,
				processInfo.hProcess)) {
			logger::error(
				"AssignProcessToJobObject failed. Win32 error: {}",
				GetLastError());

			TerminateProcess(
				processInfo.hProcess,
				1);

			CloseHandle(
				processInfo.hThread);

			CloseHandle(
				processInfo.hProcess);

			return false;
		}

		if (
			ResumeThread(
				processInfo.hThread) ==
			static_cast<DWORD>(
				-1)) {
			logger::error(
				"ResumeThread failed. Win32 error: {}",
				GetLastError());

			TerminateProcess(
				processInfo.hProcess,
				1);

			CloseHandle(
				processInfo.hThread);

			CloseHandle(
				processInfo.hProcess);

			return false;
		}

		CloseHandle(
			processInfo.hThread);

		g_doomProcess =
			processInfo.hProcess;

		logger::info(
			"Launched hidden DOOM guest. PID={}",
			processInfo.dwProcessId);

		return true;
	}

	// ========================================================
	// COPY WEAPON FRAME FROM DOOM
	// ========================================================

	bool CopyLatestOverlay()
	{
		if (!g_state) {
			return false;
		}

		auto& overlay =
			g_state->overlay;

		for (
			int attempt = 0;
			attempt < 4;
			++attempt) {
			const auto seq1 =
				overlay.seq;

			if (
				seq1 & 1u) {
				continue;
			}

			MemoryBarrier();

			const auto width =
				overlay.width;

			const auto height =
				overlay.height;

			const auto frame =
				overlay.frame_id;

			if (
				width !=
					SKYDOOM_OVERLAY_WIDTH ||
				height !=
					SKYDOOM_OVERLAY_HEIGHT) {
				return false;
			}

			std::memcpy(
				g_overlayPixels.data(),
				overlay.rgba,
				SKYDOOM_OVERLAY_RGBA_BYTES);

			MemoryBarrier();

			const auto seq2 =
				overlay.seq;

			if (
				seq1 == seq2 &&
				!(seq2 & 1u)) {
				if (
					frame !=
					g_lastOverlayFrame) {
					g_lastOverlayFrame =
						frame;

					return true;
				}

				return false;
			}
		}

		return false;
	}

	// ========================================================
	// D3D11 SHADER COMPILATION
	// ========================================================

	bool CompileShader(
		const char* source,
		const char* entryPoint,
		const char* target,
		ID3DBlob**  output)
	{
		if (!output) {
			return false;
		}

		ComPtr<ID3DBlob>
			errors;

		const HRESULT result =
			D3DCompile(
				source,
				std::strlen(
					source),
				nullptr,
				nullptr,
				nullptr,
				entryPoint,
				target,
				D3DCOMPILE_ENABLE_STRICTNESS |
					D3DCOMPILE_OPTIMIZATION_LEVEL3,
				0,
				output,
				&errors);

		if (
			FAILED(
				result)) {
			if (
				errors) {
				logger::error(
					"SkyDoom shader compile error: {}",
					static_cast<
						const char*>(
						errors->GetBufferPointer()));
			}

			return false;
		}

		return true;
	}

	// ========================================================
	// CREATE D3D11 RESOURCES
	// ========================================================

	bool CreateOverlayResources()
	{
		if (
			!g_device ||
			!g_context) {
			return false;
		}

		// ----------------------------------------------------
		// Vertex shader
		// ----------------------------------------------------

		ComPtr<ID3DBlob>
			vertexBlob;

		if (
			!CompileShader(
				SKYDOOM_VERTEX_SHADER,
				"main",
				"vs_5_0",
				&vertexBlob)) {
			return false;
		}

		if (
			FAILED(
				g_device->CreateVertexShader(
					vertexBlob->GetBufferPointer(),
					vertexBlob->GetBufferSize(),
					nullptr,
					&g_vertexShader))) {
			logger::error(
				"CreateVertexShader failed");

			return false;
		}

		// ----------------------------------------------------
		// Input layout
		// ----------------------------------------------------

		D3D11_INPUT_ELEMENT_DESC
		inputElements[] = {
			{ "POSITION",
				0,
				DXGI_FORMAT_R32G32B32_FLOAT,
				0,
				0,
				D3D11_INPUT_PER_VERTEX_DATA,
				0 },

			{ "TEXCOORD",
				0,
				DXGI_FORMAT_R32G32_FLOAT,
				0,
				12,
				D3D11_INPUT_PER_VERTEX_DATA,
				0 }
		};

		if (
			FAILED(
				g_device->CreateInputLayout(
					inputElements,
					2,
					vertexBlob->GetBufferPointer(),
					vertexBlob->GetBufferSize(),
					&g_inputLayout))) {
			logger::error(
				"CreateInputLayout failed");

			return false;
		}

		// ----------------------------------------------------
		// Pixel shader
		// ----------------------------------------------------

		ComPtr<ID3DBlob>
			pixelBlob;

		if (
			!CompileShader(
				SKYDOOM_PIXEL_SHADER,
				"main",
				"ps_5_0",
				&pixelBlob)) {
			return false;
		}

		if (
			FAILED(
				g_device->CreatePixelShader(
					pixelBlob->GetBufferPointer(),
					pixelBlob->GetBufferSize(),
					nullptr,
					&g_pixelShader))) {
			logger::error(
				"CreatePixelShader failed");

			return false;
		}

		// ----------------------------------------------------
		// Dynamic weapon texture
		// ----------------------------------------------------

		D3D11_TEXTURE2D_DESC
		textureDesc{};

		textureDesc.Width =
			SKYDOOM_OVERLAY_WIDTH;

		textureDesc.Height =
			SKYDOOM_OVERLAY_HEIGHT;

		textureDesc.MipLevels =
			1;

		textureDesc.ArraySize =
			1;

		textureDesc.Format =
			DXGI_FORMAT_R8G8B8A8_UNORM;

		textureDesc.SampleDesc.Count =
			1;

		textureDesc.Usage =
			D3D11_USAGE_DYNAMIC;

		textureDesc.BindFlags =
			D3D11_BIND_SHADER_RESOURCE;

		textureDesc.CPUAccessFlags =
			D3D11_CPU_ACCESS_WRITE;

		if (
			FAILED(
				g_device->CreateTexture2D(
					&textureDesc,
					nullptr,
					&g_overlayTexture))) {
			logger::error(
				"CreateTexture2D failed");

			return false;
		}

		if (
			FAILED(
				g_device->CreateShaderResourceView(
					g_overlayTexture.Get(),
					nullptr,
					&g_overlaySRV))) {
			logger::error(
				"CreateShaderResourceView failed");

			return false;
		}

		// ----------------------------------------------------
		// Dynamic vertex buffer
		// ----------------------------------------------------

		D3D11_BUFFER_DESC
		vertexBufferDesc{};

		vertexBufferDesc.ByteWidth =
			static_cast<UINT>(
				sizeof(
					OverlayVertex) *
				6);

		vertexBufferDesc.Usage =
			D3D11_USAGE_DYNAMIC;

		vertexBufferDesc.BindFlags =
			D3D11_BIND_VERTEX_BUFFER;

		vertexBufferDesc.CPUAccessFlags =
			D3D11_CPU_ACCESS_WRITE;

		if (
			FAILED(
				g_device->CreateBuffer(
					&vertexBufferDesc,
					nullptr,
					&g_vertexBuffer))) {
			logger::error(
				"CreateBuffer failed");

			return false;
		}

		// ----------------------------------------------------
		// Point sampler
		// ----------------------------------------------------

		D3D11_SAMPLER_DESC
		samplerDesc{};

		samplerDesc.Filter =
			D3D11_FILTER_MIN_MAG_MIP_POINT;

		samplerDesc.AddressU =
			D3D11_TEXTURE_ADDRESS_CLAMP;

		samplerDesc.AddressV =
			D3D11_TEXTURE_ADDRESS_CLAMP;

		samplerDesc.AddressW =
			D3D11_TEXTURE_ADDRESS_CLAMP;

		samplerDesc.MaxLOD =
			D3D11_FLOAT32_MAX;

		if (
			FAILED(
				g_device->CreateSamplerState(
					&samplerDesc,
					&g_pointSampler))) {
			logger::error(
				"CreateSamplerState failed");

			return false;
		}

		// ----------------------------------------------------
		// Straight-alpha blend
		// ----------------------------------------------------

		D3D11_BLEND_DESC
		blendDesc{};

		blendDesc.RenderTarget[0].BlendEnable =
			TRUE;

		blendDesc.RenderTarget[0].SrcBlend =
			D3D11_BLEND_SRC_ALPHA;

		blendDesc.RenderTarget[0].DestBlend =
			D3D11_BLEND_INV_SRC_ALPHA;

		blendDesc.RenderTarget[0].BlendOp =
			D3D11_BLEND_OP_ADD;

		blendDesc.RenderTarget[0].SrcBlendAlpha =
			D3D11_BLEND_ONE;

		blendDesc.RenderTarget[0].DestBlendAlpha =
			D3D11_BLEND_INV_SRC_ALPHA;

		blendDesc.RenderTarget[0].BlendOpAlpha =
			D3D11_BLEND_OP_ADD;

		blendDesc.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_ALL;

		if (
			FAILED(
				g_device->CreateBlendState(
					&blendDesc,
					&g_alphaBlend))) {
			logger::error(
				"CreateBlendState failed");

			return false;
		}

		// ----------------------------------------------------
		// Disable depth testing
		// ----------------------------------------------------

		D3D11_DEPTH_STENCIL_DESC
		depthDesc{};

		depthDesc.DepthEnable =
			FALSE;

		depthDesc.DepthWriteMask =
			D3D11_DEPTH_WRITE_MASK_ZERO;

		depthDesc.DepthFunc =
			D3D11_COMPARISON_ALWAYS;

		depthDesc.StencilEnable =
			FALSE;

		if (
			FAILED(
				g_device->CreateDepthStencilState(
					&depthDesc,
					&g_depthDisabled))) {
			logger::error(
				"CreateDepthStencilState failed");

			return false;
		}

		// ----------------------------------------------------
		// No culling
		// ----------------------------------------------------

		D3D11_RASTERIZER_DESC
		rasterDesc{};

		rasterDesc.FillMode =
			D3D11_FILL_SOLID;

		rasterDesc.CullMode =
			D3D11_CULL_NONE;

		rasterDesc.DepthClipEnable =
			TRUE;

		if (
			FAILED(
				g_device->CreateRasterizerState(
					&rasterDesc,
					&g_rasterizer))) {
			logger::error(
				"CreateRasterizerState failed");

			return false;
		}

		logger::info(
			"SkyDoom native D3D11 resources created");

		return true;
	}

	bool InitializeOverlayGraphics(
		IDXGISwapChain*
			swapChain)
	{
		if (
			g_device &&
			g_context &&
			g_vertexShader &&
			g_pixelShader) {
			return true;
		}

		if (!swapChain) {
			return false;
		}

		ID3D11Device*
			rawDevice =
				nullptr;

		const HRESULT deviceResult =
			swapChain->GetDevice(
				__uuidof(
					ID3D11Device),
				reinterpret_cast<
					void**>(
					&rawDevice));

		if (
			FAILED(
				deviceResult) ||
			!rawDevice) {
			logger::error(
				"Could not obtain Skyrim D3D11 device");

			return false;
		}

		g_device.Attach(
			rawDevice);

		ID3D11DeviceContext*
			rawContext =
				nullptr;

		g_device->GetImmediateContext(
			&rawContext);

		if (!rawContext) {
			logger::error(
				"Skyrim D3D11 immediate context is null");

			return false;
		}

		g_context.Attach(
			rawContext);

		return CreateOverlayResources();
	}

	bool EnsureBackBufferRTV(
		IDXGISwapChain*
			swapChain)
	{
		if (
			g_backBufferRTV) {
			return true;
		}

		if (
			!swapChain ||
			!g_device) {
			return false;
		}

		ComPtr<
			ID3D11Texture2D>
			backBuffer;

		if (
			FAILED(
				swapChain->GetBuffer(
					0,
					IID_PPV_ARGS(
						&backBuffer)))) {
			logger::error(
				"Could not obtain Skyrim back buffer");

			return false;
		}

		D3D11_TEXTURE2D_DESC
		desc{};

		backBuffer->GetDesc(
			&desc);

		g_backBufferWidth =
			desc.Width;

		g_backBufferHeight =
			desc.Height;

		if (
			FAILED(
				g_device->CreateRenderTargetView(
					backBuffer.Get(),
					nullptr,
					&g_backBufferRTV))) {
			logger::error(
				"CreateRenderTargetView failed");

			return false;
		}

		return true;
	}

	// ========================================================
	// UPLOAD WEAPON RGBA
	// ========================================================

	void UploadOverlayTexture()
	{
		if (
			!g_overlayTexture ||
			!g_context) {
			return;
		}

		D3D11_MAPPED_SUBRESOURCE
		mapped{};

		if (
			FAILED(
				g_context->Map(
					g_overlayTexture.Get(),
					0,
					D3D11_MAP_WRITE_DISCARD,
					0,
					&mapped))) {
			return;
		}

		for (
			std::uint32_t y = 0;
			y <
			SKYDOOM_OVERLAY_HEIGHT;
			++y) {
			std::memcpy(
				static_cast<
					std::uint8_t*>(
					mapped.pData) +
					y *
						mapped.RowPitch,

				g_overlayPixels.data() +
					y *
						SKYDOOM_OVERLAY_WIDTH *
						4,

				SKYDOOM_OVERLAY_WIDTH *
					4);
		}

		g_context->Unmap(
			g_overlayTexture.Get(),
			0);
	}

	// ========================================================
	// BUILD FULL-SCREEN WEAPON QUAD
	// ========================================================

	bool UpdateWeaponVertices()
	{
		if (
			!g_vertexBuffer ||
			!g_context ||
			g_backBufferWidth == 0 ||
			g_backBufferHeight == 0) {
			return false;
		}

		const float widthScale =
			static_cast<float>(
				g_backBufferWidth) /
			static_cast<float>(
				SKYDOOM_OVERLAY_WIDTH);

		const float heightScale =
			static_cast<float>(
				g_backBufferHeight) /
			static_cast<float>(
				SKYDOOM_OVERLAY_HEIGHT);

		const float scale =
			widthScale <
					heightScale ?
				widthScale :
				heightScale;

		const float drawWidth =
			static_cast<float>(
				SKYDOOM_OVERLAY_WIDTH) *
			scale;

		const float drawHeight =
			static_cast<float>(
				SKYDOOM_OVERLAY_HEIGHT) *
			scale;

		const float leftPixels =
			(static_cast<float>(
				 g_backBufferWidth) -
				drawWidth) *
			0.5f;

		const float topPixels =
			static_cast<float>(
				g_backBufferHeight) -
			drawHeight;

		const float rightPixels =
			leftPixels +
			drawWidth;

		const float bottomPixels =
			topPixels +
			drawHeight;

		const float left =
			(leftPixels /
				static_cast<float>(
					g_backBufferWidth)) *
				2.0f -
			1.0f;

		const float right =
			(rightPixels /
				static_cast<float>(
					g_backBufferWidth)) *
				2.0f -
			1.0f;

		const float top =
			1.0f -
			(topPixels /
				static_cast<float>(
					g_backBufferHeight)) *
				2.0f;

		const float bottom =
			1.0f -
			(bottomPixels /
				static_cast<float>(
					g_backBufferHeight)) *
				2.0f;

		const OverlayVertex vertices[6] = {
			{ left,
				top,
				0.0f,
				0.0f,
				0.0f },

			{ right,
				top,
				0.0f,
				1.0f,
				0.0f },

			{ right,
				bottom,
				0.0f,
				1.0f,
				1.0f },

			{ left,
				top,
				0.0f,
				0.0f,
				0.0f },

			{ right,
				bottom,
				0.0f,
				1.0f,
				1.0f },

			{ left,
				bottom,
				0.0f,
				0.0f,
				1.0f }
		};

		D3D11_MAPPED_SUBRESOURCE
		mapped{};

		if (
			FAILED(
				g_context->Map(
					g_vertexBuffer.Get(),
					0,
					D3D11_MAP_WRITE_DISCARD,
					0,
					&mapped))) {
			return false;
		}

		std::memcpy(
			mapped.pData,
			vertices,
			sizeof(
				vertices));

		g_context->Unmap(
			g_vertexBuffer.Get(),
			0);

		return true;
	}

	// ========================================================
	// DRAW SKYDOOM WEAPON
	// ========================================================



    // ========================================================
    // SKYDOOM TARGET-SENSITIVE CROSSHAIR V15.7
    // ========================================================
    //
    // This is presentation only.
    //
    // IMPORTANT:
    //   Do not reuse this target to alter firing direction,
    //   hit detection, damage, projectile movement or collision.
    //
    // Skyrim's own CrosshairPickData decides whether the centre
    // hover is currently a live Actor.  SkyDoom only changes the
    // colour of five pixels in its existing transparent 320x200
    // overlay:
    //
    //       #
    //      ###
    //       #
    //
    // Green  = normal
    // Yellow = living Actor under Skyrim's crosshair
    //
    // SKYDOOM_TARGET_CROSSHAIR_V15_7
    // SKYDOOM_TARGET_CROSSHAIR_V15_7_R1
    bool SkyDoomCrosshairHasLiveActorTargetV15_7()
    {
        auto* player =
            RE::PlayerCharacter::
                GetSingleton();

        if (!player)
        {
            return false;
        }

        /*
            Fast path: keep Skyrim's native CrosshairPickData for
            nearby interaction-range targets.
        */
        auto* pickData =
            RE::CrosshairPickData::
                GetSingleton();

        if (pickData)
        {
            auto target =
                pickData->
                    GetActiveTarget().
                    get();

            if (target)
            {
                auto* actor =
                    target->
                        As<RE::Actor>();

                if (
                    actor &&
                    actor != player &&
                    !actor->
                        IsDisabled() &&
                    actor->
                        Is3DLoaded() &&
                    !actor->
                        IsGhost() &&
                    !actor->
                        IsDead()
                )
                {
                    return true;
                }
            }
        }

        /*
            CrosshairPickData is primarily Skyrim's world/activation
            picker, so it can stop reporting Actors well before a DOOM
            hitscan runs out of range.

            For the visual yellow state only, fall back to the SAME
            centre-camera Actor AABB rules already used by the known
            SkyDoom pistol bridge.

            This does not feed anything back into combat.
        */

        static std::uint64_t
            lastLongRangeScanMs =
                0u;

        static bool
            lastLongRangeResult =
                false;

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        /*
            Cap the heavier ProcessLists/Havok visual query at about
            30 Hz.  The actual reticle still renders every frame.
        */
        if (
            lastLongRangeScanMs != 0u &&
            now >=
                lastLongRangeScanMs &&
            now -
                lastLongRangeScanMs <
                33u
        )
        {
            return
                lastLongRangeResult;
        }

        lastLongRangeScanMs =
            now;

        lastLongRangeResult =
            false;

        auto* camera =
            RE::PlayerCamera::
                GetSingleton();

        if (
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return false;
        }

        const RE::NiPoint3
            rayOrigin =
                RE::PlayerCamera::
                    GetActiveCameraPosition();

        RE::NiPoint3
            rayDirection =
                camera->
                    cameraRoot->
                    world.
                    rotate *
                RE::NiPoint3{
                    0.0f,
                    1.0f,
                    0.0f
                };

        if (
            rayDirection.
                Unitize() <=
            0.0f
        )
        {
            return false;
        }

        auto* processLists =
            RE::ProcessLists::
                GetSingleton();

        if (!processLists)
        {
            return false;
        }

        RE::Actor*
            nearestActor =
                nullptr;

        float nearestDistance =
            SKYDOOM_PISTOL_HITSCAN_RANGE +
            1.0f;

        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();

            auto* actor =
                actorPtr.
                    get();

            if (
                !actor ||
                actor == player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost() ||
                actor->
                    IsDead()
            )
            {
                continue;
            }

            const RE::NiPoint3
                actorPosition =
                    actor->
                        GetPosition();

            const float dx =
                actorPosition.x -
                rayOrigin.x;

            const float dy =
                actorPosition.y -
                rayOrigin.y;

            const float dz =
                actorPosition.z -
                rayOrigin.z;

            const float
                roughDistanceSquared =
                    dx * dx +
                    dy * dy +
                    dz * dz;

            const float
                maxCandidateDistance =
                    SKYDOOM_PISTOL_HITSCAN_RANGE +
                    900.0f;

            if (
                roughDistanceSquared >
                maxCandidateDistance *
                    maxCandidateDistance
            )
            {
                continue;
            }

            float actorHeight =
                actor->
                    GetHeight();

            if (actorHeight < 21.0f)
            {
                actorHeight =
                    21.0f;
            }

            if (actorHeight > 840.0f)
            {
                actorHeight =
                    840.0f;
            }

            float actorWidth =
                actor->
                    GetBoundRadius() *
                2.0f;

            if (actorWidth < 21.0f)
            {
                actorWidth =
                    21.0f;
            }

            if (actorWidth > 420.0f)
            {
                actorWidth =
                    420.0f;
            }

            const float actorRadius =
                actorWidth *
                    0.5f +
                4.0f;

            const float minX =
                actorPosition.x -
                actorRadius;

            const float maxX =
                actorPosition.x +
                actorRadius;

            const float minY =
                actorPosition.y -
                actorRadius;

            const float maxY =
                actorPosition.y +
                actorRadius;

            const float minZ =
                actorPosition.z -
                4.0f;

            const float maxZ =
                actorPosition.z +
                actorHeight;

            float tMin =
                0.0f;

            float tMax =
                SKYDOOM_PISTOL_HITSCAN_RANGE;

            auto intersectSlab =
                [&tMin, &tMax](
                    float origin,
                    float direction,
                    float slabMin,
                    float slabMax
                ) -> bool
                {
                    if (
                        direction >
                            -0.000001f &&
                        direction <
                            0.000001f
                    )
                    {
                        return
                            origin >=
                                slabMin &&
                            origin <=
                                slabMax;
                    }

                    const float
                        inverseDirection =
                            1.0f /
                            direction;

                    float t1 =
                        (
                            slabMin -
                            origin
                        ) *
                        inverseDirection;

                    float t2 =
                        (
                            slabMax -
                            origin
                        ) *
                        inverseDirection;

                    if (t1 > t2)
                    {
                        const float swap =
                            t1;

                        t1 =
                            t2;

                        t2 =
                            swap;
                    }

                    if (t1 > tMin)
                    {
                        tMin =
                            t1;
                    }

                    if (t2 < tMax)
                    {
                        tMax =
                            t2;
                    }

                    return
                        tMax >=
                        tMin;
                };

            if (
                !intersectSlab(
                    rayOrigin.x,
                    rayDirection.x,
                    minX,
                    maxX
                ) ||
                !intersectSlab(
                    rayOrigin.y,
                    rayDirection.y,
                    minY,
                    maxY
                ) ||
                !intersectSlab(
                    rayOrigin.z,
                    rayDirection.z,
                    minZ,
                    maxZ
                )
            )
            {
                continue;
            }

            if (tMax < 0.0f)
            {
                continue;
            }

            float hitDistance =
                tMin;

            if (hitDistance < 0.0f)
            {
                hitDistance =
                    0.0f;
            }

            if (
                hitDistance >
                    SKYDOOM_PISTOL_HITSCAN_RANGE ||
                hitDistance >=
                    nearestDistance
            )
            {
                continue;
            }

            nearestDistance =
                hitDistance;

            nearestActor =
                actor;
        }

        if (!nearestActor)
        {
            return false;
        }

        /*
            Mirror the existing pistol bridge's strict world-geometry
            occlusion test so the colour does not turn yellow through
            a wall just because a loaded Actor happens to be behind it.
        */
        auto* occlusionController =
            player->
                GetCharController();

        if (occlusionController)
        {
            auto* occlusionWorld =
                occlusionController->
                    GetHavokWorld();

            if (occlusionWorld)
            {
                float
                    occlusionStartDistance =
                        nearestDistance *
                        0.25f;

                if (
                    occlusionStartDistance >
                    48.0f
                )
                {
                    occlusionStartDistance =
                        48.0f;
                }

                if (
                    occlusionStartDistance <
                    4.0f
                )
                {
                    occlusionStartDistance =
                        4.0f;
                }

                const float
                    occlusionEndDistance =
                        nearestDistance -
                        8.0f;

                if (
                    occlusionEndDistance >
                    occlusionStartDistance
                )
                {
                    const RE::NiPoint3
                        occlusionStartWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionStartDistance;

                    const RE::NiPoint3
                        occlusionEndWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionEndDistance;

                    const RE::NiPoint3
                        playerWorld =
                            player->
                                GetPosition();

                    RE::hkVector4
                        playerHavok{};

                    occlusionController->
                        GetPosition(
                            playerHavok,
                            false
                        );

                    const RE::NiPoint3
                        startDeltaWorld{
                            occlusionStartWorld.x -
                                playerWorld.x,

                            occlusionStartWorld.y -
                                playerWorld.y,

                            occlusionStartWorld.z -
                                playerWorld.z
                        };

                    const RE::NiPoint3
                        endDeltaWorld{
                            occlusionEndWorld.x -
                                playerWorld.x,

                            occlusionEndWorld.y -
                                playerWorld.y,

                            occlusionEndWorld.z -
                                playerWorld.z
                        };

                    const float havokScale =
                        RE::bhkWorld::
                            GetWorldScale();

                    const RE::hkVector4
                        havokScaleVector{
                            havokScale
                        };

                    const RE::hkVector4
                        occlusionStartHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    startDeltaWorld
                                ) *
                                havokScaleVector
                            );

                    const RE::hkVector4
                        occlusionEndHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    endDeltaWorld
                                ) *
                                havokScaleVector
                            );

                    RE::bhkPickData
                        occlusionPick{};

                    occlusionPick.
                        rayInput.
                        enableShapeCollectionFilter =
                            true;

                    occlusionPick.
                        rayInput.
                        from =
                            occlusionStartHavok;

                    occlusionPick.
                        rayInput.
                        to =
                            occlusionEndHavok;

                    occlusionController->
                        GetCollisionFilterInfo(
                            occlusionPick.
                                rayInput.
                                filterInfo
                        );

                    const bool occlusionHit =
                        occlusionWorld->
                            PickObject(
                                occlusionPick
                            );

                    if (
                        occlusionHit &&
                        occlusionPick.
                            rayOutput.
                            rootCollidable
                    )
                    {
                        auto* blockerRef =
                            RE::TESHavokUtilities::
                                FindCollidableRef(
                                    *occlusionPick.
                                        rayOutput.
                                        rootCollidable
                                );

                        if (
                            blockerRef !=
                            nearestActor
                        )
                        {
                            return false;
                        }
                    }
                }
            }
        }

        lastLongRangeResult =
            true;

        return true;
    }




    /*
        SKYDOOM_TARGET_CROSSHAIR_V15_7_R1

        Hide only Skyrim's normal HUD crosshair while SkyDoom is
        actively supplying its own reticle.

        Calling the HUD movie's SetCrosshairEnabled function avoids
        altering the user's INI file.

        Track the HUD movie pointer too: if Skyrim rebuilds/reloads the
        HUD movie while SkyDoom remains active, re-apply the hidden
        state to the new movie.
    */
    static bool
        g_skyDoomSkyrimCrosshairHiddenV15_7 =
            false;

    static RE::GFxMovieView*
        g_skyDoomSkyrimCrosshairMovieV15_7 =
            nullptr;


    void SetSkyDoomSkyrimCrosshairHiddenV15_7(
        bool hide
    )
    {
        auto* ui =
            RE::UI::
                GetSingleton();

        auto* strings =
            RE::InterfaceStrings::
                GetSingleton();

        if (
            !ui ||
            !strings
        )
        {
            return;
        }

        auto menu =
            ui->
                GetMenu(
                    strings->
                        hudMenu
                );

        if (
            !menu ||
            !menu->
                uiMovie ||
            !menu->
                fxDelegate
        )
        {
            return;
        }

        auto* movie =
            menu->
                uiMovie.
                get();

        /*
            SKYDOOM_CROSSHAIR_REASSERT_V15_8_R3

            When hide == true, deliberately DO NOT early-return just
            because we already told this HUD movie to hide the crosshair.

            Skyrim can internally re-enable its crosshair on the same
            HUD movie after HUD/cell/state refreshes.  Reasserting
            SetCrosshairEnabled(false) from Present keeps the Skyrim
            reticle suppressed for as long as SkyDoom owns the HUD.

            When hide == false, retain the cache so restoration is only
            sent once rather than every frame.
        */
        if (
            !hide &&
            g_skyDoomSkyrimCrosshairHiddenV15_7 ==
                hide &&
            g_skyDoomSkyrimCrosshairMovieV15_7 ==
                movie
        )
        {
            return;
        }

        RE::FxResponseArgsEx<1>
            args;

        args[0].
            SetBoolean(
                !hide
            );

        menu->
            fxDelegate->
            Invoke(
                movie,
                "SetCrosshairEnabled",
                args
            );

        g_skyDoomSkyrimCrosshairHiddenV15_7 =
            hide;

        g_skyDoomSkyrimCrosshairMovieV15_7 =
            movie;
    }

    void SetSkyDoomCrosshairPixelV15_7(
        int x,
        int y,
        std::uint8_t red,
        std::uint8_t green,
        std::uint8_t blue
    )
    {
        if (
            x < 0 ||
            y < 0 ||
            x >=
                static_cast<int>(
                    SKYDOOM_OVERLAY_WIDTH
                ) ||
            y >=
                static_cast<int>(
                    SKYDOOM_OVERLAY_HEIGHT
                )
        )
        {
            return;
        }

        const std::size_t index =
            (
                static_cast<std::size_t>(
                    y
                ) *
                    SKYDOOM_OVERLAY_WIDTH +
                static_cast<std::size_t>(
                    x
                )
            ) *
                4u;

        g_overlayPixels[
            index + 0u
        ] =
            red;

        g_overlayPixels[
            index + 1u
        ] =
            green;

        g_overlayPixels[
            index + 2u
        ] =
            blue;

        g_overlayPixels[
            index + 3u
        ] =
            255u;
    }


    void ApplySkyDoomTargetCrosshairV15_7()
    {
        if (
            !g_state ||
            !g_state->
                doom.
                running ||
            !g_state->
                doom.
                in_level
        )
        {
            return;
        }

        const bool actorTarget =
            SkyDoomCrosshairHasLiveActorTargetV15_7();

        const std::uint8_t red =
            actorTarget ?
                255u :
                0u;

        const std::uint8_t green =
            255u;

        const std::uint8_t blue =
            0u;

        const int centerX =
            static_cast<int>(
                SKYDOOM_OVERLAY_WIDTH /
                2u
            );

        const int centerY =
            static_cast<int>(
                SKYDOOM_OVERLAY_HEIGHT /
                2u
            );

        /*
            SKYDOOM_TARGET_CROSSHAIR_V15_7_R1

            Match the reference more closely:

                    dot

              dot       dot

                    dot

            There is deliberately NO centre pixel and one full native
            Doom pixel of empty space between the centre and each dot.

            Because SkyDoom point-scales the 320x200 overlay, each
            native pixel becomes a small crisp square on the final
            Skyrim backbuffer instead of joining into the solid plus
            produced by v15.7.
        */
        SetSkyDoomCrosshairPixelV15_7(
            centerX,
            centerY - 2,
            red,
            green,
            blue
        );

        SetSkyDoomCrosshairPixelV15_7(
            centerX - 2,
            centerY,
            red,
            green,
            blue
        );

        SetSkyDoomCrosshairPixelV15_7(
            centerX + 2,
            centerY,
            red,
            green,
            blue
        );

        SetSkyDoomCrosshairPixelV15_7(
            centerX,
            centerY + 2,
            red,
            green,
            blue
        );
    }

	void RenderSkyDoomOverlay(
		IDXGISwapChain*
			swapChain)
	{
		if (
			!g_state ||
			!g_state->skyrim.in_game ||
			g_state->skyrim.paused ||
			!DoomHeartbeatIsFresh()) {
			return;
		}

		if (
			!InitializeOverlayGraphics(
				swapChain)) {
			return;
		}

		if (
			!EnsureBackBufferRTV(
				swapChain)) {
			return;
		}

		        /*
            SKYDOOM_TARGET_CROSSHAIR_V15_7

            Always repaint the crosshair immediately before uploading.
            CopyLatestOverlay() refreshes the Doom weapon/HUD pixels
            whenever Chocolate publishes a new frame; on intervening
            Skyrim frames we simply recolour the same five centre
            pixels so target feedback remains responsive.
        */
        CopyLatestOverlay();

        ApplySkyDoomTargetCrosshairV15_7();

        UploadOverlayTexture();

		if (
			g_lastOverlayFrame == 0 ||
			!g_overlaySRV) {
			return;
		}

		if (
			!UpdateWeaponVertices()) {
			return;
		}

		g_context->OMSetRenderTargets(
			1,
			g_backBufferRTV.GetAddressOf(),
			nullptr);

		D3D11_VIEWPORT viewport{};

		viewport.TopLeftX =
			0.0f;

		viewport.TopLeftY =
			0.0f;

		viewport.Width =
			static_cast<float>(
				g_backBufferWidth);

		viewport.Height =
			static_cast<float>(
				g_backBufferHeight);

		viewport.MinDepth =
			0.0f;

		viewport.MaxDepth =
			1.0f;

		g_context->RSSetViewports(
			1,
			&viewport);

		g_context->RSSetState(
			g_rasterizer.Get());

		const UINT stride =
			sizeof(
				OverlayVertex);

		const UINT offset =
			0;

		ID3D11Buffer*
			vertexBuffers[] = {
				g_vertexBuffer.Get()
			};

		g_context->IASetVertexBuffers(
			0,
			1,
			vertexBuffers,
			&stride,
			&offset);

		g_context->IASetInputLayout(
			g_inputLayout.Get());

		g_context->IASetPrimitiveTopology(
			D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		g_context->VSSetShader(
			g_vertexShader.Get(),
			nullptr,
			0);

		g_context->PSSetShader(
			g_pixelShader.Get(),
			nullptr,
			0);

		ID3D11ShaderResourceView*
			srvs[] = {
				g_overlaySRV.Get()
			};

		g_context->PSSetShaderResources(
			0,
			1,
			srvs);

		ID3D11SamplerState*
			samplers[] = {
				g_pointSampler.Get()
			};

		g_context->PSSetSamplers(
			0,
			1,
			samplers);

		const float blendFactor[4] = {
			0.0f,
			0.0f,
			0.0f,
			0.0f
		};

		g_context->OMSetBlendState(
			g_alphaBlend.Get(),
			blendFactor,
			0xFFFFFFFFu);

		g_context->OMSetDepthStencilState(
			g_depthDisabled.Get(),
			0);

		g_context->Draw(
			6,
			0);

		/*
            Unbind the SRV afterwards.

            This avoids accidental resource hazards if Skyrim
            later reuses the same slots.
        */

		ID3D11ShaderResourceView*
			nullSRV =
				nullptr;

		g_context->PSSetShaderResources(
			0,
			1,
			&nullSRV);
	}

    // ========================================================
    // PHYSICAL MOUSE -> DOOM FIRE
    // ========================================================

    bool SkyDoomHasForegroundFocus()
    {
        const HWND foregroundWindow =
            GetForegroundWindow();


        if (!foregroundWindow)
        {
            return false;
        }


        DWORD foregroundProcess =
            0;


        GetWindowThreadProcessId(
            foregroundWindow,
            &foregroundProcess
        );


        return
            foregroundProcess ==
            GetCurrentProcessId();
    }


    void PollPhysicalDoomFire()

    {

        // SKYDOOM_REAL_SHOTGUN_V6

        // SKYDOOM_REAL_CHAINGUN_V9

        // SKYDOOM_REAL_MELEE_V10

        // SKYDOOM_REAL_ROCKET_V11



        static bool physicalMeleeKeyDown = false;

        static bool physicalPistolKeyDown = false;

        static bool physicalShotgunKeyDown = false;

        static bool physicalChaingunKeyDown = false;

        static bool physicalRocketKeyDown = false;
        static bool physicalPlasmaKeyDown = false;
        static bool physicalBfgKeyDown = false;

        // SKYDOOM_MUSIC_TOGGLE_V15_8
        static bool
            physicalMusicToggleKeyDown =
                false;





        if (!g_state)

        {

            return;

        }





        const bool active =

            g_state->skyrim.in_game &&

            !g_state->skyrim.paused &&

            DoomHeartbeatIsFresh() &&

            SkyDoomHasForegroundFocus();





        bool physicalFireDown = false;

        bool meleeKeyDown = false;

        bool pistolKeyDown = false;

        bool shotgunKeyDown = false;

        bool chaingunKeyDown = false;

        bool rocketKeyDown = false;
        bool plasmaKeyDown = false;
        bool bfgKeyDown = false;

        bool musicToggleKeyDown =
            false;





        if (active)

        {

            // SKYDOOM_MUSIC_TOGGLE_V15_8
            musicToggleKeyDown =
                (
                    GetAsyncKeyState(
                        VK_F10
                    ) &
                    0x8000
                ) !=
                0;

            physicalFireDown =

                (

                    GetAsyncKeyState(

                        VK_LBUTTON

                    ) &

                    0x8000

                ) != 0;





            meleeKeyDown =

                (

                    GetAsyncKeyState(

                        '1'

                    ) &

                    0x8000

                ) != 0;





            pistolKeyDown =

                (

                    GetAsyncKeyState(

                        '2'

                    ) &

                    0x8000

                ) != 0;





            shotgunKeyDown =

                (

                    GetAsyncKeyState(

                        '3'

                    ) &

                    0x8000

                ) != 0;





            chaingunKeyDown =

                (

                    GetAsyncKeyState(

                        '4'

                    ) &

                    0x8000

                ) != 0;





            rocketKeyDown =

                (

                    GetAsyncKeyState(

                        '5'

                    ) &

                    0x8000

                ) != 0;

            plasmaKeyDown =
                (
                    GetAsyncKeyState(
                        '6'
                    ) &
                    0x8000
                ) != 0;



            bfgKeyDown =

                (

                    GetAsyncKeyState(

                        '7'

                    ) &

                    0x8000

                ) != 0;

        }





        if (

            meleeKeyDown &&

            !physicalMeleeKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_MELEE,

                1

            );

        }





        if (

            pistolKeyDown &&

            !physicalPistolKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_PISTOL,

                1

            );

        }





        if (

            shotgunKeyDown &&

            !physicalShotgunKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_SHOTGUN,

                1

            );

        }





        if (

            chaingunKeyDown &&

            !physicalChaingunKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_CHAINGUN,

                1

            );

        }





        if (

            rocketKeyDown &&

            !physicalRocketKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_ROCKET,

                1

            );

        }





        if (
            plasmaKeyDown &&
            !physicalPlasmaKeyDown
        )
        {
            PushInputEvent(
                SKYDOOM_INPUT_EVENT_BUTTON,
                SKYDOOM_INPUT_WEAPON_PLASMA,
                1
            );
        }


        if (

            bfgKeyDown &&

            !physicalBfgKeyDown

        )

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_WEAPON_BFG,

                1

            );

        }



                /*
            SKYDOOM_MUSIC_TOGGLE_V15_8

            Rising edge only: holding F10 cannot rapidly flip the
            state every Present frame.
        */
        if (
            musicToggleKeyDown &&
            !physicalMusicToggleKeyDown
        )
        {
            PushInputEvent(
                SKYDOOM_INPUT_EVENT_MUSIC_TOGGLE,
                0,
                1
            );
        }

physicalMeleeKeyDown =

            meleeKeyDown;



        physicalPistolKeyDown =

            pistolKeyDown;



        physicalShotgunKeyDown =

            shotgunKeyDown;



        physicalChaingunKeyDown =

            chaingunKeyDown;



        physicalRocketKeyDown =

            rocketKeyDown;

        physicalPlasmaKeyDown =
            plasmaKeyDown;

        physicalBfgKeyDown =
            bfgKeyDown;

        physicalMusicToggleKeyDown =
            musicToggleKeyDown;





        if (

            physicalFireDown !=

            g_physicalFireDown

        )

        {

            g_physicalFireDown =

                physicalFireDown;





            PushInputEvent(

                SKYDOOM_INPUT_EVENT_BUTTON,

                SKYDOOM_INPUT_FIRE,

                physicalFireDown ?

                    1 :

                    0

            );

        }

    }

















	// ========================================================
	// SWAP CHAIN HOOKS
	// ========================================================

	HRESULT __stdcall HookPresent(
		IDXGISwapChain*
			 swapChain,
		UINT syncInterval,
		UINT flags)
	{
        // SKYDOOM_TARGET_CROSSHAIR_V15_7_R1
        const bool skyDoomOwnsCrosshair =
            g_state &&
            g_state->
                doom.
                running &&
            g_state->
                doom.
                in_level &&
            g_state->
                skyrim.
                in_game &&
            DoomHeartbeatIsFresh();

        SetSkyDoomSkyrimCrosshairHiddenV15_7(
            skyDoomOwnsCrosshair
        );

        // SKYDOOM_POLL_PHYSICAL_LMB_V4
        PollPhysicalDoomFire();

		RenderSkyDoomOverlay(
			swapChain);

        // SKYDOOM_VISIBLE_ROCKET_PRESENT_V11_1

        DrawSkyDoomRocketSpritesV11_1();
        // SKYDOOM_ROCKET_EXPLOSION_PRESENT_V11_2
        DrawSkyDoomRocketExplosionsV11_2();
        // SKYDOOM_VISIBLE_PLASMA_PRESENT_V12_2A
        DrawSkyDoomPlasmaSpritesV12_2();
        // SKYDOOM_PLASMA_IMPACT_PRESENT_V13
        DrawSkyDoomPlasmaImpactsV13();
        // SKYDOOM_VISIBLE_BFG_PRESENT_V14_2A
        DrawSkyDoomBFGSpritesV14_2A();
        // SKYDOOM_BFG_IMPACT_PRESENT_V14_2B
        DrawSkyDoomBFGImpactsV14_2B();
        // SKYDOOM_BFG_EXTRA_PRESENT_V14_3
        DrawSkyDoomBFGExtrasV14_3();
        // SKYDOOM_PICKUP_PRESENT_V15
        DrawSkyDoomPhysicalPickupsV15();

		return g_originalPresent(
			swapChain,
			syncInterval,
			flags);
	}

	HRESULT __stdcall HookResizeBuffers(
		IDXGISwapChain*
					swapChain,
		UINT        bufferCount,
		UINT        width,
		UINT        height,
		DXGI_FORMAT newFormat,
		UINT        swapChainFlags)
	{
		g_backBufferRTV.Reset();

		g_backBufferWidth =
			0;

		g_backBufferHeight =
			0;

		return g_originalResizeBuffers(
			swapChain,
			bufferCount,
			width,
			height,
			newFormat,
			swapChainFlags);
	}

	bool PatchVTableEntry(
		void** entry,
		void*  replacement,
		void** original)
	{
		DWORD oldProtect =
			0;

		if (
			!VirtualProtect(
				entry,
				sizeof(
					void*),
				PAGE_EXECUTE_READWRITE,
				&oldProtect)) {
			return false;
		}

		*original =
			*entry;

		*entry =
			replacement;

		DWORD ignored =
			0;

		VirtualProtect(
			entry,
			sizeof(
				void*),
			oldProtect,
			&ignored);

		FlushInstructionCache(
			GetCurrentProcess(),
			entry,
			sizeof(
				void*));

		return true;
	}

	bool InstallRenderHook()
	{
		if (
			g_renderHookInstalled) {
			return true;
		}

		auto* renderWindow =
			RE::BSGraphics::
				Renderer::
					GetCurrentRenderWindow();

		if (!renderWindow) {
			logger::error(
				"CommonLib returned no current Skyrim render window");

			return false;
		}

		if (
			!renderWindow->swapChain) {
			logger::error(
				"Current Skyrim render window has no swap chain");

			return false;
		}

		auto* swapChain =
			reinterpret_cast<
				IDXGISwapChain*>(
				renderWindow->swapChain);

		auto** vtable =
			*reinterpret_cast<
				void***>(
				swapChain);

		if (!vtable) {
			logger::error(
				"Skyrim swap-chain vtable was null");

			return false;
		}

		void* originalPresent =
			nullptr;

		void* originalResize =
			nullptr;

		if (
			!PatchVTableEntry(
				&vtable[8],
				reinterpret_cast<
					void*>(
					&HookPresent),
				&originalPresent)) {
			logger::error(
				"Could not hook IDXGISwapChain::Present");

			return false;
		}

		if (
			!PatchVTableEntry(
				&vtable[13],
				reinterpret_cast<
					void*>(
					&HookResizeBuffers),
				&originalResize)) {
			logger::error(
				"Could not hook IDXGISwapChain::ResizeBuffers");

			return false;
		}

		g_originalPresent =
			reinterpret_cast<
				PresentFn>(
				originalPresent);

		g_originalResizeBuffers =
			reinterpret_cast<
				ResizeBuffersFn>(
				originalResize);

		g_renderHookInstalled =
			true;

		logger::info(
			"SkyDoom native D3D11 overlay hook installed");

		return true;
	}

	// ========================================================
	// SKYRIM STATE
	// ========================================================

    // ========================================================
    // SKYDOOM FIRST-PERSON PRESENTATION
    // ========================================================

    // SKYDOOM_PRESENTATION_V4_BEGIN

    void ApplySkyDoomPresentation(
        bool active
    )
    {
        /*
            SkyDoom owns first-person combat while active.

            Disable Skyrim's fighting controls temporarily so
            it does not punch, swing weapons or block.

            The third ToggleControls argument is FALSE because
            this must NOT alter Skyrim's stored control state.
        */

        auto* controlMap =
            RE::ControlMap::
                GetSingleton();


        if (controlMap)
        {
            using UEFlag =
                RE::ControlMap::
                    UEFlag;


            if (active)
            {
                if (
                    controlMap->
                        IsFightingControlsEnabled()
                )
                {
                    controlMap->
                        ToggleControls(
                            UEFlag::kFighting,
                            false,
                            false
                        );


                    g_skyDoomDisabledFightingControls =
                        true;
                }
            }
            else if (
                g_skyDoomDisabledFightingControls
            )
            {
                controlMap->
                    ToggleControls(
                        UEFlag::kFighting,
                        true,
                        false
                    );


                g_skyDoomDisabledFightingControls =
                    false;
            }
        }


        /*
            Hide Skyrim's first-person player model.

            This removes the fists / hands / Skyrim weapon.

            The DOOM weapon is drawn separately by SkyDoom's
            D3D11 overlay, so it remains visible.
        */

        auto* player =
            RE::PlayerCharacter::
                GetSingleton();


        if (!player)
        {
            return;
        }


        auto* firstPerson =
            player->
                Get3D(
                    true
                );


        if (!firstPerson)
        {
            return;
        }


        if (active)
        {
            /*
                Repeat the cull while active because Skyrim can
                rebuild its first-person model after animation,
                equipment or camera changes.
            */

            firstPerson->
                CullNode(
                    true
                );


            g_skyDoomCulledFirstPerson =
                true;
        }
        else if (
            g_skyDoomCulledFirstPerson
        )
        {
            firstPerson->
                CullNode(
                    false
                );


            g_skyDoomCulledFirstPerson =
                false;
        }
    }

    // SKYDOOM_PRESENTATION_V4_END


    // ========================================================
    // DOOM PISTOL -> SKYRIM ACTOR DAMAGE
    // ========================================================

    // SKYDOOM_REAL_PISTOL_DAMAGE_BRIDGE_V5



    std::uint64_t

        g_lastDoomPistolShotSerial =

            0;





    std::uint64_t

        g_lastDoomPistolDamageTotal =

            0;





    void ResetDoomCombatSnapshot()

    {

        g_haveDoomCombatSnapshot =

            false;



        g_lastDoomBullets =

            0;



        g_lastDoomWeapon =

            -1;



        g_lastDoomCombatTick =

            0;



        g_lastDoomPistolShotSerial =

            0;



        g_lastDoomPistolDamageTotal =

            0;

    }







    // ========================================================

    // SKYDOOM NATIVE SKYRIM HIT PIPELINE

    // ========================================================



    // SKYDOOM_NATIVE_HITDATA_PISTOL



    /*

        Chocolate Doom remains authoritative for the damage.



        This layer only asks Skyrim to DELIVER that already-decided

        damage through its normal native hit-processing machinery.



        The address-resolution strategy mirrors SkyCraft's working

        combat implementation for AE runtimes.

    */



    using SkyDoomProcessHitFn =

        void(

            RE::Actor*,

            RE::HitData&

        );





    using SkyDoomHitDataCtorFn =

        RE::HitData*(

            RE::HitData*

        );





    SkyDoomProcessHitFn*

        g_skyDoomProcessHit =

            nullptr;





    SkyDoomHitDataCtorFn*

        g_skyDoomHitDataCtor =

            nullptr;





    bool

        g_skyDoomHitPipelineResolved =

            false;





    bool

        g_skyDoomHitPipelineAvailable =

            false;





    void ResolveSkyDoomHitPipeline()

    {

        if (g_skyDoomHitPipelineResolved)

        {

            return;

        }





        g_skyDoomHitPipelineResolved =

            true;





        /*

            These addresses are the AE route used by SkyCraft.



            Never call them blindly on another runtime.

        */



        if (!REL::Module::IsAE())

        {

            SKSE::log::warn(

                "[skydoomskse] Native HitData pipeline unavailable: runtime is not AE. DoDamage fallback will be used."

            );



            return;

        }





        /*

            Skyrim's melee HitFrame handler calls the native

            victim hit processor here.



            Verify that the instruction is still the expected CALL

            and that it resolves to the expected target before

            enabling the pipeline.

        */



        const auto callSite =

            REL::ID(

                38627

            ).address() +

            0x4A8;





        const auto* code =

            reinterpret_cast<

                const std::uint8_t*

            >(

                callSite

            );





        const auto expectedTarget =

            REL::ID(

                38586

            ).address();





        std::int32_t relativeOffset =

            0;





        std::memcpy(

            &relativeOffset,

            code + 1,

            sizeof(

                relativeOffset

            )

        );





        const auto actualTarget =

            callSite +

            5 +

            relativeOffset;





        if (

            code[0] !=

                0xE8 ||

            actualTarget !=

                expectedTarget

        )

        {

            SKSE::log::warn(

                "[skydoomskse] Native HitData pipeline validation failed. DoDamage fallback will be used."

            );



            return;

        }





        g_skyDoomProcessHit =

            reinterpret_cast<

                SkyDoomProcessHitFn*

            >(

                expectedTarget

            );





        g_skyDoomHitDataCtor =

            reinterpret_cast<

                SkyDoomHitDataCtorFn*

            >(

                REL::ID(

                    43995

                ).address()

            );





        g_skyDoomHitPipelineAvailable =

            g_skyDoomProcessHit !=

                nullptr &&

            g_skyDoomHitDataCtor !=

                nullptr;





        if (g_skyDoomHitPipelineAvailable)

        {

            SKSE::log::info(

                "[skydoomskse] Native Skyrim HitData pistol pipeline resolved successfully."

            );

        }

        else

        {

            SKSE::log::warn(

                "[skydoomskse] Native HitData addresses could not be resolved. DoDamage fallback will be used."

            );

        }

    }





    RE::NiAVObject*

    GetSkyDoomPistolHitNode(

        RE::Actor* actor

    )

    {

        if (!actor)

        {

            return nullptr;

        }





        auto* root =

            actor->

                Get3D();





        if (!root)

        {

            return nullptr;

        }





        /*

            Same broad torso-node preference used by SkyCraft.



            This gives Skyrim's impact system a sensible node for

            blood / flesh effects.

        */



        const char* nodeNames[] =

        {

            "NPC Spine2 [Spn2]",

            "NPC Spine1 [Spn1]",

            "NPC Spine [Spn0]",

            "NPC Pelvis [Pelv]"

        };





        for (

            const char* nodeName :

            nodeNames

        )

        {

            auto* node =

                root->

                    GetObjectByName(

                        nodeName

                    );





            if (node)

            {

                return node;

            }

        }





        return root;

    }





    bool ApplySkyDoomNativePistolHit(

        RE::PlayerCharacter* player,

        RE::Actor* target,

        float damage,

        const RE::NiPoint3& hitPosition,

        const RE::NiPoint3& hitDirection

    )

    {

        if (

            !player ||

            !target ||

            damage <= 0.0f

        )

        {

            return false;

        }





        ResolveSkyDoomHitPipeline();





        if (

            !g_skyDoomHitPipelineAvailable ||

            !g_skyDoomProcessHit ||

            !g_skyDoomHitDataCtor

        )

        {

            return false;

        }





        /*

            Construct the HitData using Skyrim's own constructor,

            exactly as the native game expects.

        */



        alignas(16)

        std::uint8_t hitStorage[

            sizeof(

                RE::HitData

            )

        ]{};





        auto* hit =

            reinterpret_cast<

                RE::HitData*

            >(

                hitStorage

            );





        g_skyDoomHitDataCtor(

            hit

        );





        /*

            Let Skyrim initialise all normal hit state first.



            There is deliberately no Skyrim inventory weapon here:

            DOOM owns the actual weapon and its damage mechanics.

        */



        hit->

            Populate(

                player,

                target,

                nullptr

            );





        /*

            Use a base-game dagger internally as an impact-data

            stand-in.



            It is NOT equipped and does NOT determine damage.



            Its only purpose is to give Skyrim a normal physical

            weapon impact definition for flesh/blood effects.

        */



        auto* impactWeapon =

            RE::TESForm::

                LookupByID<

                    RE::TESObjectWEAP

                >(

                    0x0001397E

                );





        hit->

            weapon =

                impactWeapon;





        hit->

            hitPosition =

                hitPosition;





        hit->

            hitDirection =

                hitDirection;





        /*

            The important part:



            Chocolate Doom has already rolled exactly 5, 10 or 15.



            Do not let Skyrim replace that number with weapon

            damage, armour calculation, sneak bonuses or criticals.

        */



        hit->

            totalDamage =

                damage;





        hit->

            physicalDamage =

                damage;





        hit->

            percentBlocked =

                0.0f;





        hit->

            resistedPhysicalDamage =

                0.0f;





        hit->

            resistedTypedDamage =

                0.0f;





        hit->

            stagger =

                0.0f;





        hit->

            sneakAttackBonus =

                1.0f;





        hit->

            bonusHealthDamageMult =

                1.0f;





        hit->

            pushBack =

                0.0f;





        /*

            This synthetic hit should not secretly become a Skyrim

            melee/power/sneak/critical attack.



            DOOM is deciding the weapon mechanics.

        */



        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kBlocked

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kBlockWithWeapon

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kBlockCandidate

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kCritical

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kSneakAttack

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kPowerAttack

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kBash

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kTimedBash

            );





        hit->

            flags.

            reset(

                RE::HitData::Flag::

                    kMeleeAttack

            );





        hit->

            skill =

                RE::ActorValue::

                    kNone;





        /*

            Hand the complete hit to Skyrim's native victim hit

            processor.



            This is the important replacement for plain DoDamage().

        */



        g_skyDoomProcessHit(

            target,

            *hit

        );





        /*

            SkyCraft explicitly asks Skyrim's impact manager to

            play the weapon-on-target impact as well.



            Do the same here so the synthetic DOOM bullet can

            produce Skyrim blood/flesh impact behaviour.

        */



        auto* hitNode =

            GetSkyDoomPistolHitNode(

                target

            );





        if (

            auto* impacts =

                RE::BGSImpactManager::

                    GetSingleton();



            impacts &&

            impactWeapon &&

            impactWeapon->

                impactDataSet &&

            hitNode

        )

        {
            /*
                SKOOM_VANILLA_BLOOD_SPRAY_V15_9E2

                The real hit/damage above is still processed ONCE.

                Keep one ordinary Skyrim flesh/weapon impact, then add
                Skyrim's dedicated vanilla BloodSprayImpactSetRed five
                times with a small visual spread.
            */

            RE::NiPoint3 impactDirection =
                hitDirection;


            impacts->
                PlayImpactEffect(
                    target,
                    impactWeapon->
                        impactDataSet,
                    hitNode->
                        name.
                        c_str(),
                    impactDirection,
                    128.0f,
                    false,
                    false
                );


            static RE::BGSImpactDataSet*
                bloodSprayImpactSet =
                    nullptr;

            static bool
                bloodSprayLookupAttempted =
                    false;


            if (!bloodSprayLookupAttempted)
            {
                bloodSprayLookupAttempted =
                    true;

                bloodSprayImpactSet =
                    RE::TESForm::
                        LookupByEditorID<
                            RE::BGSImpactDataSet
                        >(
                            "BloodSprayImpactSetRed"
                        );

                if (bloodSprayImpactSet)
                {
                    SKSE::log::info(
                        "[skoom] BloodSprayImpactSetRed resolved: {:08X}",
                        bloodSprayImpactSet->
                            GetFormID()
                    );
                }
                else
                {
                    SKSE::log::warn(
                        "[skoom] BloodSprayImpactSetRed lookup failed; extra gore disabled"
                    );
                }
            }


            if (bloodSprayImpactSet)
            {
                RE::NiPoint3 forward =
                    impactDirection;

                if (
                    forward.
                        Unitize() <=
                    0.0f
                )
                {
                    forward =
                        RE::NiPoint3{
                            0.0f,
                            1.0f,
                            0.0f
                        };
                }


                RE::NiPoint3 right{
                    forward.y,
                    -forward.x,
                    0.0f
                };

                if (
                    right.
                        Unitize() <=
                    0.0f
                )
                {
                    right =
                        RE::NiPoint3{
                            1.0f,
                            0.0f,
                            0.0f
                        };
                }


                RE::NiPoint3 localUp{
                    right.y * forward.z -
                        right.z * forward.y,

                    right.z * forward.x -
                        right.x * forward.z,

                    right.x * forward.y -
                        right.y * forward.x
                };

                if (
                    localUp.
                        Unitize() <=
                    0.0f
                )
                {
                    localUp =
                        RE::NiPoint3{
                            0.0f,
                            0.0f,
                            1.0f
                        };
                }


                constexpr std::uint32_t
                    SKOOM_BLOOD_SPRAY_COUNT_V15_9E2 =
                        5u;


                constexpr float
                    sideOffsets[
                        SKOOM_BLOOD_SPRAY_COUNT_V15_9E2
                    ] =
                    {
                        0.0f,
                        0.12f,
                        -0.12f,
                        0.075f,
                        -0.075f
                    };


                constexpr float
                    upOffsets[
                        SKOOM_BLOOD_SPRAY_COUNT_V15_9E2
                    ] =
                    {
                        0.0f,
                        0.055f,
                        0.055f,
                        -0.095f,
                        -0.095f
                    };


                for (
                    std::uint32_t spray = 0u;
                    spray <
                        SKOOM_BLOOD_SPRAY_COUNT_V15_9E2;
                    ++spray
                )
                {
                    RE::NiPoint3 sprayDirection{
                        forward.x +
                            right.x *
                                sideOffsets[spray] +
                            localUp.x *
                                upOffsets[spray],

                        forward.y +
                            right.y *
                                sideOffsets[spray] +
                            localUp.y *
                                upOffsets[spray],

                        forward.z +
                            right.z *
                                sideOffsets[spray] +
                            localUp.z *
                                upOffsets[spray]
                    };


                    if (
                        sprayDirection.
                            Unitize() <=
                        0.0f
                    )
                    {
                        sprayDirection =
                            forward;
                    }


                    impacts->
                        PlayImpactEffect(
                            target,
                            bloodSprayImpactSet,
                            hitNode->
                                name.
                                c_str(),
                            sprayDirection,
                            192.0f,
                            false,
                            false
                        );
                }
            }}





        return true;

    }





    void ApplyDoomPistolDamageToCrosshairActor(
        std::int32_t doomDamage
    )
    {
        // SKYDOOM_ACTOR_HITBOX_V4

        /*
            A real Chocolate Doom pistol shot has already been
            confirmed by ammo consumption before this function
            is called.

            Target selection here deliberately follows the same
            broad architecture SkyCraft uses for Skyrim NPCs:

              Skyrim ProcessLists
                  ->
              highActorHandles
                  ->
              real Actor position / height / radius

            We do NOT use CrosshairPickData, bhkWorld::PickObject
            or TES::Pick here.

            Instead we cast the centre-camera ray mathematically
            against each nearby Skyrim Actor's bounds and select
            the nearest intersected Actor.
        */

        if (doomDamage <= 0)
        {
            return;
        }


        auto* player =
            RE::PlayerCharacter::
                GetSingleton();


        if (!player)
        {
            return;
        }


        auto* camera =
            RE::PlayerCamera::
                GetSingleton();


        if (
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return;
        }


        /*
            The DOOM weapon sprite is centred on screen.

            Therefore the Skyrim camera's forward vector is our
            hitscan direction.
        */

        const RE::NiPoint3
            rayOrigin =
                RE::PlayerCamera::
                    GetActiveCameraPosition();


        RE::NiPoint3 rayDirection =
            camera->
                cameraRoot->
                world.
                rotate *
            RE::NiPoint3{
                0.0f,
                1.0f,
                0.0f
            };


        if (
            rayDirection.
                Unitize() <=
            0.0f
        )
        {
            return;
        }


        auto* processLists =
            RE::ProcessLists::
                GetSingleton();


        if (!processLists)
        {
            return;
        }


        RE::Actor*
            nearestActor =
                nullptr;


        float nearestDistance =
            SKYDOOM_PISTOL_HITSCAN_RANGE +
            1.0f;


        std::uint32_t
            testedActors =
                0;


        /*
            Ray versus axis-aligned Actor bounds.

            Skyrim's Actor::GetPosition() is treated as the feet
            position, matching the way SkyCraft exports Actors.

            Width is based on GetBoundRadius()*2.
            Height is based on GetHeight().

            The dimensions are constrained to the same broad
            ranges SkyCraft uses for its NPC proxy dimensions:

              height: 21 .. 840 Skyrim units
              width : 21 .. 420 Skyrim units
        */

        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();


            auto* actor =
                actorPtr.
                    get();


            if (
                !actor ||
                actor == player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost() ||
                actor->
                    IsDead()
            )
            {
                continue;
            }


            ++testedActors;


            const RE::NiPoint3
                actorPosition =
                    actor->
                        GetPosition();


            /*
                Reject Actors that cannot possibly be within the
                pistol's maximum distance before doing the more
                expensive slab intersection.

                Include their dimensions in the allowance so
                large creatures near the range boundary remain
                hittable.
            */

            const float
                dx =
                    actorPosition.x -
                    rayOrigin.x;


            const float
                dy =
                    actorPosition.y -
                    rayOrigin.y;


            const float
                dz =
                    actorPosition.z -
                    rayOrigin.z;


            const float
                roughDistanceSquared =
                    dx * dx +
                    dy * dy +
                    dz * dz;


            const float
                maxCandidateDistance =
                    SKYDOOM_PISTOL_HITSCAN_RANGE +
                    900.0f;


            if (
                roughDistanceSquared >
                maxCandidateDistance *
                    maxCandidateDistance
            )
            {
                continue;
            }


            float actorHeight =
                actor->
                    GetHeight();


            if (actorHeight < 21.0f)
            {
                actorHeight =
                    21.0f;
            }


            if (actorHeight > 840.0f)
            {
                actorHeight =
                    840.0f;
            }


            float actorWidth =
                actor->
                    GetBoundRadius() *
                2.0f;


            if (actorWidth < 21.0f)
            {
                actorWidth =
                    21.0f;
            }


            if (actorWidth > 420.0f)
            {
                actorWidth =
                    420.0f;
            }


            /*
                A tiny amount of extra width makes the pistol
                feel like a normal game hitscan rather than a
                mathematically perfect one-pixel laser.

                This is only 4 Skyrim units on each side.
            */

            const float actorRadius =
                actorWidth *
                    0.5f +
                4.0f;


            const float minX =
                actorPosition.x -
                actorRadius;


            const float maxX =
                actorPosition.x +
                actorRadius;


            const float minY =
                actorPosition.y -
                actorRadius;


            const float maxY =
                actorPosition.y +
                actorRadius;


            /*
                ActorPosition is the feet position.

                Give the bottom a small allowance because actors
                can visually sink slightly into terrain.
            */

            const float minZ =
                actorPosition.z -
                4.0f;


            const float maxZ =
                actorPosition.z +
                actorHeight;


            float tMin =
                0.0f;


            float tMax =
                SKYDOOM_PISTOL_HITSCAN_RANGE;


            /*
                Standard ray/AABB slab test.

                Keeping it inline avoids introducing another
                helper or changing any other part of Plugin.cpp.
            */

            auto intersectSlab =
                [&tMin, &tMax](
                    float origin,
                    float direction,
                    float slabMin,
                    float slabMax
                ) -> bool
                {
                    if (
                        direction >
                            -0.000001f &&
                        direction <
                            0.000001f
                    )
                    {
                        return
                            origin >= slabMin &&
                            origin <= slabMax;
                    }


                    const float inverseDirection =
                        1.0f /
                        direction;


                    float t1 =
                        (
                            slabMin -
                            origin
                        ) *
                        inverseDirection;


                    float t2 =
                        (
                            slabMax -
                            origin
                        ) *
                        inverseDirection;


                    if (t1 > t2)
                    {
                        const float swap =
                            t1;

                        t1 =
                            t2;

                        t2 =
                            swap;
                    }


                    if (t1 > tMin)
                    {
                        tMin =
                            t1;
                    }


                    if (t2 < tMax)
                    {
                        tMax =
                            t2;
                    }


                    return
                        tMax >=
                        tMin;
                };


            if (
                !intersectSlab(
                    rayOrigin.x,
                    rayDirection.x,
                    minX,
                    maxX
                ) ||
                !intersectSlab(
                    rayOrigin.y,
                    rayDirection.y,
                    minY,
                    maxY
                ) ||
                !intersectSlab(
                    rayOrigin.z,
                    rayDirection.z,
                    minZ,
                    maxZ
                )
            )
            {
                continue;
            }


            if (
                tMax <
                    0.0f
            )
            {
                continue;
            }


            float hitDistance =
                tMin;


            /*
                Point-blank case: camera can technically begin
                inside a large creature's bounds.
            */

            if (hitDistance < 0.0f)
            {
                hitDistance =
                    0.0f;
            }


            if (
                hitDistance >
                    SKYDOOM_PISTOL_HITSCAN_RANGE ||
                hitDistance >=
                    nearestDistance
            )
            {
                continue;
            }


            nearestDistance =
                hitDistance;


            nearestActor =
                actor;
        }


        if (!nearestActor)
        {
            SKSE::log::info(
                "[skydoomskse] DOOM pistol actor hitscan: MISS candidates={}",
                testedActors
            );

            return;
        }


        // SKYDOOM_STRICT_GEOMETRY_OCCLUSION_V4

        /*
            Actor targeting has ALREADY succeeded.

            nearestActor and nearestDistance come from our
            known-good mathematical Actor-hitbox system.

            Havok is used here ONLY to answer:

                "Is solid Skyrim geometry between the camera
                 and the Actor we already selected?"

            Havok is NOT responsible for selecting the NPC.
        */

        auto* occlusionController =
            player->
                GetCharController();


        if (occlusionController)
        {
            auto* occlusionWorld =
                occlusionController->
                    GetHavokWorld();


            if (occlusionWorld)
            {
                /*
                    Start slightly in front of the camera to
                    avoid the player's own controller.

                    For very close targets, shorten this
                    automatically.
                */

                float occlusionStartDistance =
                    nearestDistance *
                    0.25f;


                if (occlusionStartDistance > 48.0f)
                {
                    occlusionStartDistance =
                        48.0f;
                }


                if (occlusionStartDistance < 4.0f)
                {
                    occlusionStartDistance =
                        4.0f;
                }


                /*
                    Stop slightly before the mathematical Actor
                    hitbox so the target itself should not count
                    as scenery.
                */

                const float occlusionEndDistance =
                    nearestDistance -
                    8.0f;


                if (
                    occlusionEndDistance >
                    occlusionStartDistance
                )
                {
                    const RE::NiPoint3
                        occlusionStartWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionStartDistance;


                    const RE::NiPoint3
                        occlusionEndWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionEndDistance;


                    /*
                        Convert Bethesda-world coordinates into
                        the active Havok world's coordinate
                        frame using the player's controller as
                        the origin anchor.
                    */

                    const RE::NiPoint3
                        playerWorld =
                            player->
                                GetPosition();


                    RE::hkVector4
                        playerHavok{};


                    occlusionController->
                        GetPosition(
                            playerHavok,
                            false
                        );

                    const RE::NiPoint3
                        startDeltaWorld{
                            occlusionStartWorld.x -
                                playerWorld.x,

                            occlusionStartWorld.y -
                                playerWorld.y,

                            occlusionStartWorld.z -
                                playerWorld.z
                        };


                    const RE::NiPoint3
                        endDeltaWorld{
                            occlusionEndWorld.x -
                                playerWorld.x,

                            occlusionEndWorld.y -
                                playerWorld.y,

                            occlusionEndWorld.z -
                                playerWorld.z
                        };


                                                            // SKYDOOM_OCCLUSION_DIAGNOSTICS_CLEANED_V4// SKYDOOM_WORLD_SCALE_FIX_V4
                    /*
                        IMPORTANT:

                        Bethesda world-space -> Havok-space uses
                        bhkWorld::GetWorldScale().

                        GetWorldScaleInverse() converts in the
                        opposite direction and was making our
                        camera scenery ray vastly mis-scaled.

                        Keep the existing player-Havok anchor
                        so shifted Havok world origins continue
                        to be handled correctly.
                    */

                    const float havokScale =
                        RE::bhkWorld::
                            GetWorldScale();


                    const RE::hkVector4
                        havokScaleVector{
                            havokScale
                        };




                    const RE::hkVector4
                        occlusionStartHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    startDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    const RE::hkVector4
                        occlusionEndHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    endDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    RE::bhkPickData
                        occlusionPick{};


                    occlusionPick.
                        rayInput.
                        enableShapeCollectionFilter =
                            true;


                    occlusionPick.
                        rayInput.
                        from =
                            occlusionStartHavok;


                    occlusionPick.
                        rayInput.
                        to =
                            occlusionEndHavok;


                    /*
                        Start from the player's actual collision
                        filter so Skyrim's current system-group
                        information is retained.
                    */

                    occlusionController->
                        GetCollisionFilterInfo(
                            occlusionPick.
                                rayInput.
                                filterInfo
                        );


                    /*
                        Then change only the collision layer used
                        for the scenery query.
                    */

                                        // SKYDOOM_CONTROLLER_FILTER_OCCLUSION_V4
                    /*
                        IMPORTANT:

                        Keep the collision filter returned by the
                        player's bhkCharacterController exactly
                        as Skyrim supplied it.

                        Do NOT replace its collision layer.

                        The controller's real filter already
                        contains the layer/system-group data used
                        to collide with solid Skyrim world
                        geometry.
                    */


                    /*
                        bhkPickData contains an explicit direction
                        vector as well as the input endpoints.
                    */

                    // SKYDOOM_NO_MANUAL_PICK_RAY_V4
                    /*
                        Do NOT populate occlusionPick.ray here.

                        Our successful native-Havok ground probe,
                        and known working Skyrim/CommonLib
                        PickObject usage, populate only:

                            rayInput.from
                            rayInput.to
                            rayInput.filterInfo

                        Let bhkWorld::PickObject handle the rest.
                    */


                    const bool occlusionHit =
                        occlusionWorld->
                            PickObject(
                                occlusionPick
                            );


                    if (
                        occlusionHit &&
                        occlusionPick.
                            rayOutput.
                            rootCollidable
                    )
                    {
                        auto* blockerRef =
                            RE::TESHavokUtilities::
                                FindCollidableRef(
                                    *occlusionPick.
                                        rayOutput.
                                        rootCollidable
                                );


                        /*
                            Do not let the selected target's own
                            physics body count as scenery.
                        */

                        if (
                            blockerRef !=
                            nearestActor
                        )
                        {
                            const float rayLength =
                                occlusionEndDistance -
                                occlusionStartDistance;


                            const float blockerDistance =
                                occlusionStartDistance +
                                (
                                    occlusionPick.
                                        rayOutput.
                                        hitFraction *
                                    rayLength
                                );


                            if (blockerRef)
                            {
                                SKSE::log::info(
                                    "[skydoomskse] DOOM pistol geometry BLOCKED: target={:08X} blocker={:08X} blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerRef->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }
                            else
                            {
                                /*
                                    Static architecture and
                                    terrain can have collision
                                    without a normal TESObjectREFR.

                                    It still blocks the bullet.
                                */

                                SKSE::log::info(
                                    "[skydoomskse] DOOM pistol geometry BLOCKED: target={:08X} blocker=STATIC blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }


                            return;
                        }
                    }


                                        SKSE::log::info(
                        "[skydoomskse] DOOM pistol geometry CLEAR: target={:08X} distance={} filter=0x{:08X}",
                        nearestActor->
                            GetFormID(),
                        nearestDistance,
                        occlusionPick.
                            rayInput.
                            filterInfo.
                            filter
                    );
                }
            }
        }

        auto* actorValueOwner =
            nearestActor->
                AsActorValueOwner();


        if (!actorValueOwner)
        {
            return;
        }


        const float healthBefore =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        if (healthBefore <= 0.0f)
        {
            return;
        }


        const float damage =

            static_cast<float>(

                doomDamage

            );


        /*
            SkyCraft uses Skyrim's HitData pipeline when
            available and falls back to Actor::DoDamage().

            For this SkyDoom milestone we use that reliable
            native fallback rather than merely editing the
            Health ActorValue.

            This also gives Skyrim the player as the attacker.
        */

        /*

            SKYDOOM_NATIVE_HITDATA_DELIVERY



            Target selection and geometry occlusion have already

            succeeded above.



            The exact damage value still came from Chocolate Doom.

        */



        const RE::NiPoint3

            nativeHitPosition =

                rayOrigin +

                rayDirection *

                    nearestDistance;





        const bool

            usedNativeHitData =

                ApplySkyDoomNativePistolHit(

                    player,

                    nearestActor,

                    damage,

                    nativeHitPosition,

                    rayDirection

                );





        const char*

            damagePath =

                usedNativeHitData ?

                    "HitData" :

                    "DoDamage";





        /*

            Safety fallback.



            If our runtime/address validation ever rejects the

            native HitData path, retain the already-proven v5

            behaviour rather than losing DOOM combat entirely.

        */



        if (!usedNativeHitData)

        {

            nearestActor->

                DoDamage(

                    damage,

                    player,

                    true

                );

        }


        const float healthAfter =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        /*
            Make a surviving non-teammate respond to the player.

            SkyCraft does the same after applying its Minecraft
            weapon hit.
        */

        if (
            !nearestActor->
                IsDead() &&
            !nearestActor->
                IsPlayerTeammate() &&
            !nearestActor->
                IsInCombat()
        )
        {
            nearestActor->
                StartCombat(
                    player
                );
        }


        SKSE::log::info(
            "[skydoomskse] DOOM pistol actor HIT: actor={:08X} doomDamage={} appliedDamage={} distance={} health={} -> {} candidates={} path={}",
            nearestActor->
                GetFormID(),
            doomDamage,
            damage,
            nearestDistance,
            healthBefore,
            healthAfter,
            testedActors,
            damagePath
        );
    }

    void ApplyDoomShotgunPelletDamageToCrosshairActor(

        std::int32_t doomDamage,

        std::int32_t doomAngleOffset

    )
    {
        // SKYDOOM_ACTOR_HITBOX_V4

        /*
            A real Chocolate Doom pistol shot has already been
            confirmed by ammo consumption before this function
            is called.

            Target selection here deliberately follows the same
            broad architecture SkyCraft uses for Skyrim NPCs:

              Skyrim ProcessLists
                  ->
              highActorHandles
                  ->
              real Actor position / height / radius

            We do NOT use CrosshairPickData, bhkWorld::PickObject
            or TES::Pick here.

            Instead we cast the centre-camera ray mathematically
            against each nearby Skyrim Actor's bounds and select
            the nearest intersected Actor.
        */

        if (doomDamage <= 0)
        {
            return;
        }


        auto* player =
            RE::PlayerCharacter::
                GetSingleton();


        if (!player)
        {
            return;
        }


        auto* camera =
            RE::PlayerCamera::
                GetSingleton();


        if (
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return;
        }


        /*
            The DOOM weapon sprite is centred on screen.

            Therefore the Skyrim camera's forward vector is our
            hitscan direction.
        */

        const RE::NiPoint3
            rayOrigin =
                RE::PlayerCamera::
                    GetActiveCameraPosition();


        RE::NiPoint3 rayDirection =
            camera->
                cameraRoot->
                world.
                rotate *
            RE::NiPoint3{
                0.0f,
                1.0f,
                0.0f
            };



        /*

            SKYDOOM_EXACT_SHOTGUN_SPREAD_V6



            DOOM angle_t represents one full revolution as 2^32.



            doomAngleOffset is the exact signed delta that

            Chocolate Doom already used for this pellet.



            Rotate only around Skyrim world Z so the original

            camera pitch is preserved.

        */



        const double

            doomSpreadRadians =

                static_cast<double>(

                    doomAngleOffset

                ) *

                (

                    6.28318530717958647692 /

                    4294967296.0

                );





        const float spreadCos =

            static_cast<float>(

                std::cos(

                    doomSpreadRadians

                )

            );





        const float spreadSin =

            static_cast<float>(

                std::sin(

                    doomSpreadRadians

                )

            );





        const float baseRayX =

            rayDirection.x;





        const float baseRayY =

            rayDirection.y;





        rayDirection.x =

            baseRayX *

                spreadCos -

            baseRayY *

                spreadSin;





        rayDirection.y =

            baseRayX *

                spreadSin +

            baseRayY *

                spreadCos;






        if (
            rayDirection.
                Unitize() <=
            0.0f
        )
        {
            return;
        }


        auto* processLists =
            RE::ProcessLists::
                GetSingleton();


        if (!processLists)
        {
            return;
        }


        RE::Actor*
            nearestActor =
                nullptr;


        float nearestDistance =
            SKYDOOM_PISTOL_HITSCAN_RANGE +
            1.0f;


        std::uint32_t
            testedActors =
                0;


        /*
            Ray versus axis-aligned Actor bounds.

            Skyrim's Actor::GetPosition() is treated as the feet
            position, matching the way SkyCraft exports Actors.

            Width is based on GetBoundRadius()*2.
            Height is based on GetHeight().

            The dimensions are constrained to the same broad
            ranges SkyCraft uses for its NPC proxy dimensions:

              height: 21 .. 840 Skyrim units
              width : 21 .. 420 Skyrim units
        */

        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();


            auto* actor =
                actorPtr.
                    get();


            if (
                !actor ||
                actor == player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost() ||
                actor->
                    IsDead()
            )
            {
                continue;
            }


            ++testedActors;


            const RE::NiPoint3
                actorPosition =
                    actor->
                        GetPosition();


            /*
                Reject Actors that cannot possibly be within the
                pistol's maximum distance before doing the more
                expensive slab intersection.

                Include their dimensions in the allowance so
                large creatures near the range boundary remain
                hittable.
            */

            const float
                dx =
                    actorPosition.x -
                    rayOrigin.x;


            const float
                dy =
                    actorPosition.y -
                    rayOrigin.y;


            const float
                dz =
                    actorPosition.z -
                    rayOrigin.z;


            const float
                roughDistanceSquared =
                    dx * dx +
                    dy * dy +
                    dz * dz;


            const float
                maxCandidateDistance =
                    SKYDOOM_PISTOL_HITSCAN_RANGE +
                    900.0f;


            if (
                roughDistanceSquared >
                maxCandidateDistance *
                    maxCandidateDistance
            )
            {
                continue;
            }


            float actorHeight =
                actor->
                    GetHeight();


            if (actorHeight < 21.0f)
            {
                actorHeight =
                    21.0f;
            }


            if (actorHeight > 840.0f)
            {
                actorHeight =
                    840.0f;
            }


            float actorWidth =
                actor->
                    GetBoundRadius() *
                2.0f;


            if (actorWidth < 21.0f)
            {
                actorWidth =
                    21.0f;
            }


            if (actorWidth > 420.0f)
            {
                actorWidth =
                    420.0f;
            }


            /*
                A tiny amount of extra width makes the pistol
                feel like a normal game hitscan rather than a
                mathematically perfect one-pixel laser.

                This is only 4 Skyrim units on each side.
            */

            const float actorRadius =
                actorWidth *
                    0.5f +
                4.0f;


            const float minX =
                actorPosition.x -
                actorRadius;


            const float maxX =
                actorPosition.x +
                actorRadius;


            const float minY =
                actorPosition.y -
                actorRadius;


            const float maxY =
                actorPosition.y +
                actorRadius;


            /*
                ActorPosition is the feet position.

                Give the bottom a small allowance because actors
                can visually sink slightly into terrain.
            */

            const float minZ =
                actorPosition.z -
                4.0f;


            const float maxZ =
                actorPosition.z +
                actorHeight;


            float tMin =
                0.0f;


            float tMax =
                SKYDOOM_PISTOL_HITSCAN_RANGE;


            /*
                Standard ray/AABB slab test.

                Keeping it inline avoids introducing another
                helper or changing any other part of Plugin.cpp.
            */

            auto intersectSlab =
                [&tMin, &tMax](
                    float origin,
                    float direction,
                    float slabMin,
                    float slabMax
                ) -> bool
                {
                    if (
                        direction >
                            -0.000001f &&
                        direction <
                            0.000001f
                    )
                    {
                        return
                            origin >= slabMin &&
                            origin <= slabMax;
                    }


                    const float inverseDirection =
                        1.0f /
                        direction;


                    float t1 =
                        (
                            slabMin -
                            origin
                        ) *
                        inverseDirection;


                    float t2 =
                        (
                            slabMax -
                            origin
                        ) *
                        inverseDirection;


                    if (t1 > t2)
                    {
                        const float swap =
                            t1;

                        t1 =
                            t2;

                        t2 =
                            swap;
                    }


                    if (t1 > tMin)
                    {
                        tMin =
                            t1;
                    }


                    if (t2 < tMax)
                    {
                        tMax =
                            t2;
                    }


                    return
                        tMax >=
                        tMin;
                };


            if (
                !intersectSlab(
                    rayOrigin.x,
                    rayDirection.x,
                    minX,
                    maxX
                ) ||
                !intersectSlab(
                    rayOrigin.y,
                    rayDirection.y,
                    minY,
                    maxY
                ) ||
                !intersectSlab(
                    rayOrigin.z,
                    rayDirection.z,
                    minZ,
                    maxZ
                )
            )
            {
                continue;
            }


            if (
                tMax <
                    0.0f
            )
            {
                continue;
            }


            float hitDistance =
                tMin;


            /*
                Point-blank case: camera can technically begin
                inside a large creature's bounds.
            */

            if (hitDistance < 0.0f)
            {
                hitDistance =
                    0.0f;
            }


            if (
                hitDistance >
                    SKYDOOM_PISTOL_HITSCAN_RANGE ||
                hitDistance >=
                    nearestDistance
            )
            {
                continue;
            }


            nearestDistance =
                hitDistance;


            nearestActor =
                actor;
        }


        if (!nearestActor)
        {
            SKSE::log::info(
                "[skydoomskse] DOOM shotgun pellet actor hitscan: MISS candidates={}",
                testedActors
            );

            return;
        }


        // SKYDOOM_STRICT_GEOMETRY_OCCLUSION_V4

        /*
            Actor targeting has ALREADY succeeded.

            nearestActor and nearestDistance come from our
            known-good mathematical Actor-hitbox system.

            Havok is used here ONLY to answer:

                "Is solid Skyrim geometry between the camera
                 and the Actor we already selected?"

            Havok is NOT responsible for selecting the NPC.
        */

        auto* occlusionController =
            player->
                GetCharController();


        if (occlusionController)
        {
            auto* occlusionWorld =
                occlusionController->
                    GetHavokWorld();


            if (occlusionWorld)
            {
                /*
                    Start slightly in front of the camera to
                    avoid the player's own controller.

                    For very close targets, shorten this
                    automatically.
                */

                float occlusionStartDistance =
                    nearestDistance *
                    0.25f;


                if (occlusionStartDistance > 48.0f)
                {
                    occlusionStartDistance =
                        48.0f;
                }


                if (occlusionStartDistance < 4.0f)
                {
                    occlusionStartDistance =
                        4.0f;
                }


                /*
                    Stop slightly before the mathematical Actor
                    hitbox so the target itself should not count
                    as scenery.
                */

                const float occlusionEndDistance =
                    nearestDistance -
                    8.0f;


                if (
                    occlusionEndDistance >
                    occlusionStartDistance
                )
                {
                    const RE::NiPoint3
                        occlusionStartWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionStartDistance;


                    const RE::NiPoint3
                        occlusionEndWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionEndDistance;


                    /*
                        Convert Bethesda-world coordinates into
                        the active Havok world's coordinate
                        frame using the player's controller as
                        the origin anchor.
                    */

                    const RE::NiPoint3
                        playerWorld =
                            player->
                                GetPosition();


                    RE::hkVector4
                        playerHavok{};


                    occlusionController->
                        GetPosition(
                            playerHavok,
                            false
                        );

                    const RE::NiPoint3
                        startDeltaWorld{
                            occlusionStartWorld.x -
                                playerWorld.x,

                            occlusionStartWorld.y -
                                playerWorld.y,

                            occlusionStartWorld.z -
                                playerWorld.z
                        };


                    const RE::NiPoint3
                        endDeltaWorld{
                            occlusionEndWorld.x -
                                playerWorld.x,

                            occlusionEndWorld.y -
                                playerWorld.y,

                            occlusionEndWorld.z -
                                playerWorld.z
                        };


                                                            // SKYDOOM_OCCLUSION_DIAGNOSTICS_CLEANED_V4// SKYDOOM_WORLD_SCALE_FIX_V4
                    /*
                        IMPORTANT:

                        Bethesda world-space -> Havok-space uses
                        bhkWorld::GetWorldScale().

                        GetWorldScaleInverse() converts in the
                        opposite direction and was making our
                        camera scenery ray vastly mis-scaled.

                        Keep the existing player-Havok anchor
                        so shifted Havok world origins continue
                        to be handled correctly.
                    */

                    const float havokScale =
                        RE::bhkWorld::
                            GetWorldScale();


                    const RE::hkVector4
                        havokScaleVector{
                            havokScale
                        };




                    const RE::hkVector4
                        occlusionStartHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    startDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    const RE::hkVector4
                        occlusionEndHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    endDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    RE::bhkPickData
                        occlusionPick{};


                    occlusionPick.
                        rayInput.
                        enableShapeCollectionFilter =
                            true;


                    occlusionPick.
                        rayInput.
                        from =
                            occlusionStartHavok;


                    occlusionPick.
                        rayInput.
                        to =
                            occlusionEndHavok;


                    /*
                        Start from the player's actual collision
                        filter so Skyrim's current system-group
                        information is retained.
                    */

                    occlusionController->
                        GetCollisionFilterInfo(
                            occlusionPick.
                                rayInput.
                                filterInfo
                        );


                    /*
                        Then change only the collision layer used
                        for the scenery query.
                    */

                                        // SKYDOOM_CONTROLLER_FILTER_OCCLUSION_V4
                    /*
                        IMPORTANT:

                        Keep the collision filter returned by the
                        player's bhkCharacterController exactly
                        as Skyrim supplied it.

                        Do NOT replace its collision layer.

                        The controller's real filter already
                        contains the layer/system-group data used
                        to collide with solid Skyrim world
                        geometry.
                    */


                    /*
                        bhkPickData contains an explicit direction
                        vector as well as the input endpoints.
                    */

                    // SKYDOOM_NO_MANUAL_PICK_RAY_V4
                    /*
                        Do NOT populate occlusionPick.ray here.

                        Our successful native-Havok ground probe,
                        and known working Skyrim/CommonLib
                        PickObject usage, populate only:

                            rayInput.from
                            rayInput.to
                            rayInput.filterInfo

                        Let bhkWorld::PickObject handle the rest.
                    */


                    const bool occlusionHit =
                        occlusionWorld->
                            PickObject(
                                occlusionPick
                            );


                    if (
                        occlusionHit &&
                        occlusionPick.
                            rayOutput.
                            rootCollidable
                    )
                    {
                        auto* blockerRef =
                            RE::TESHavokUtilities::
                                FindCollidableRef(
                                    *occlusionPick.
                                        rayOutput.
                                        rootCollidable
                                );


                        /*
                            Do not let the selected target's own
                            physics body count as scenery.
                        */

                        if (
                            blockerRef !=
                            nearestActor
                        )
                        {
                            const float rayLength =
                                occlusionEndDistance -
                                occlusionStartDistance;


                            const float blockerDistance =
                                occlusionStartDistance +
                                (
                                    occlusionPick.
                                        rayOutput.
                                        hitFraction *
                                    rayLength
                                );


                            if (blockerRef)
                            {
                                SKSE::log::info(
                                    "[skydoomskse] DOOM shotgun pellet geometry BLOCKED: target={:08X} blocker={:08X} blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerRef->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }
                            else
                            {
                                /*
                                    Static architecture and
                                    terrain can have collision
                                    without a normal TESObjectREFR.

                                    It still blocks the bullet.
                                */

                                SKSE::log::info(
                                    "[skydoomskse] DOOM shotgun pellet geometry BLOCKED: target={:08X} blocker=STATIC blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }


                            return;
                        }
                    }


                                        SKSE::log::info(
                        "[skydoomskse] DOOM shotgun pellet geometry CLEAR: target={:08X} distance={} filter=0x{:08X}",
                        nearestActor->
                            GetFormID(),
                        nearestDistance,
                        occlusionPick.
                            rayInput.
                            filterInfo.
                            filter
                    );
                }
            }
        }

        auto* actorValueOwner =
            nearestActor->
                AsActorValueOwner();


        if (!actorValueOwner)
        {
            return;
        }


        const float healthBefore =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        if (healthBefore <= 0.0f)
        {
            return;
        }


        const float damage =

            static_cast<float>(

                doomDamage

            );


        /*
            SkyCraft uses Skyrim's HitData pipeline when
            available and falls back to Actor::DoDamage().

            For this SkyDoom milestone we use that reliable
            native fallback rather than merely editing the
            Health ActorValue.

            This also gives Skyrim the player as the attacker.
        */

        /*

            SKYDOOM_NATIVE_HITDATA_DELIVERY



            Target selection and geometry occlusion have already

            succeeded above.



            The exact damage value still came from Chocolate Doom.

        */



        const RE::NiPoint3

            nativeHitPosition =

                rayOrigin +

                rayDirection *

                    nearestDistance;





        const bool

            usedNativeHitData =

                ApplySkyDoomNativePistolHit(

                    player,

                    nearestActor,

                    damage,

                    nativeHitPosition,

                    rayDirection

                );





        const char*

            damagePath =

                usedNativeHitData ?

                    "HitData" :

                    "DoDamage";





        /*

            Safety fallback.



            If our runtime/address validation ever rejects the

            native HitData path, retain the already-proven v5

            behaviour rather than losing DOOM combat entirely.

        */



        if (!usedNativeHitData)

        {

            nearestActor->

                DoDamage(

                    damage,

                    player,

                    true

                );

        }


        const float healthAfter =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        /*
            Make a surviving non-teammate respond to the player.

            SkyCraft does the same after applying its Minecraft
            weapon hit.
        */

        if (
            !nearestActor->
                IsDead() &&
            !nearestActor->
                IsPlayerTeammate() &&
            !nearestActor->
                IsInCombat()
        )
        {
            nearestActor->
                StartCombat(
                    player
                );
        }


        SKSE::log::info(
            "[skydoomskse] DOOM shotgun pellet actor HIT: actor={:08X} doomDamage={} appliedDamage={} distance={} health={} -> {} candidates={} path={}",
            nearestActor->
                GetFormID(),
            doomDamage,
            damage,
            nearestDistance,
            healthBefore,
            healthAfter,
            testedActors,
            damagePath
        );
    }



    void ApplyDoomChaingunBulletDamageToCrosshairActor(

        std::int32_t doomDamage,

        std::int32_t doomAngleOffset

    )
    {
        // SKYDOOM_ACTOR_HITBOX_V4

        /*
            A real Chocolate Doom pistol shot has already been
            confirmed by ammo consumption before this function
            is called.

            Target selection here deliberately follows the same
            broad architecture SkyCraft uses for Skyrim NPCs:

              Skyrim ProcessLists
                  ->
              highActorHandles
                  ->
              real Actor position / height / radius

            We do NOT use CrosshairPickData, bhkWorld::PickObject
            or TES::Pick here.

            Instead we cast the centre-camera ray mathematically
            against each nearby Skyrim Actor's bounds and select
            the nearest intersected Actor.
        */

        if (doomDamage <= 0)
        {
            return;
        }


        auto* player =
            RE::PlayerCharacter::
                GetSingleton();


        if (!player)
        {
            return;
        }


        auto* camera =
            RE::PlayerCamera::
                GetSingleton();


        if (
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return;
        }


        /*
            The DOOM weapon sprite is centred on screen.

            Therefore the Skyrim camera's forward vector is our
            hitscan direction.
        */

        const RE::NiPoint3
            rayOrigin =
                RE::PlayerCamera::
                    GetActiveCameraPosition();


        RE::NiPoint3 rayDirection =
            camera->
                cameraRoot->
                world.
                rotate *
            RE::NiPoint3{
                0.0f,
                1.0f,
                0.0f
            };



        /*

            SKYDOOM_EXACT_CHAINGUN_SPREAD_V9



            DOOM angle_t represents one full revolution as 2^32.



            doomAngleOffset is the exact signed delta that

            Chocolate Doom already used for this pellet.



            Rotate only around Skyrim world Z so the original

            camera pitch is preserved.

        */



        const double

            doomSpreadRadians =

                static_cast<double>(

                    doomAngleOffset

                ) *

                (

                    6.28318530717958647692 /

                    4294967296.0

                );





        const float spreadCos =

            static_cast<float>(

                std::cos(

                    doomSpreadRadians

                )

            );





        const float spreadSin =

            static_cast<float>(

                std::sin(

                    doomSpreadRadians

                )

            );





        const float baseRayX =

            rayDirection.x;





        const float baseRayY =

            rayDirection.y;





        rayDirection.x =

            baseRayX *

                spreadCos -

            baseRayY *

                spreadSin;





        rayDirection.y =

            baseRayX *

                spreadSin +

            baseRayY *

                spreadCos;






        if (
            rayDirection.
                Unitize() <=
            0.0f
        )
        {
            return;
        }


        auto* processLists =
            RE::ProcessLists::
                GetSingleton();


        if (!processLists)
        {
            return;
        }


        RE::Actor*
            nearestActor =
                nullptr;


        float nearestDistance =
            SKYDOOM_PISTOL_HITSCAN_RANGE +
            1.0f;


        std::uint32_t
            testedActors =
                0;


        /*
            Ray versus axis-aligned Actor bounds.

            Skyrim's Actor::GetPosition() is treated as the feet
            position, matching the way SkyCraft exports Actors.

            Width is based on GetBoundRadius()*2.
            Height is based on GetHeight().

            The dimensions are constrained to the same broad
            ranges SkyCraft uses for its NPC proxy dimensions:

              height: 21 .. 840 Skyrim units
              width : 21 .. 420 Skyrim units
        */

        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();


            auto* actor =
                actorPtr.
                    get();


            if (
                !actor ||
                actor == player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost() ||
                actor->
                    IsDead()
            )
            {
                continue;
            }


            ++testedActors;


            const RE::NiPoint3
                actorPosition =
                    actor->
                        GetPosition();


            /*
                Reject Actors that cannot possibly be within the
                pistol's maximum distance before doing the more
                expensive slab intersection.

                Include their dimensions in the allowance so
                large creatures near the range boundary remain
                hittable.
            */

            const float
                dx =
                    actorPosition.x -
                    rayOrigin.x;


            const float
                dy =
                    actorPosition.y -
                    rayOrigin.y;


            const float
                dz =
                    actorPosition.z -
                    rayOrigin.z;


            const float
                roughDistanceSquared =
                    dx * dx +
                    dy * dy +
                    dz * dz;


            const float
                maxCandidateDistance =
                    SKYDOOM_PISTOL_HITSCAN_RANGE +
                    900.0f;


            if (
                roughDistanceSquared >
                maxCandidateDistance *
                    maxCandidateDistance
            )
            {
                continue;
            }


            float actorHeight =
                actor->
                    GetHeight();


            if (actorHeight < 21.0f)
            {
                actorHeight =
                    21.0f;
            }


            if (actorHeight > 840.0f)
            {
                actorHeight =
                    840.0f;
            }


            float actorWidth =
                actor->
                    GetBoundRadius() *
                2.0f;


            if (actorWidth < 21.0f)
            {
                actorWidth =
                    21.0f;
            }


            if (actorWidth > 420.0f)
            {
                actorWidth =
                    420.0f;
            }


            /*
                A tiny amount of extra width makes the pistol
                feel like a normal game hitscan rather than a
                mathematically perfect one-pixel laser.

                This is only 4 Skyrim units on each side.
            */

            const float actorRadius =
                actorWidth *
                    0.5f +
                4.0f;


            const float minX =
                actorPosition.x -
                actorRadius;


            const float maxX =
                actorPosition.x +
                actorRadius;


            const float minY =
                actorPosition.y -
                actorRadius;


            const float maxY =
                actorPosition.y +
                actorRadius;


            /*
                ActorPosition is the feet position.

                Give the bottom a small allowance because actors
                can visually sink slightly into terrain.
            */

            const float minZ =
                actorPosition.z -
                4.0f;


            const float maxZ =
                actorPosition.z +
                actorHeight;


            float tMin =
                0.0f;


            float tMax =
                SKYDOOM_PISTOL_HITSCAN_RANGE;


            /*
                Standard ray/AABB slab test.

                Keeping it inline avoids introducing another
                helper or changing any other part of Plugin.cpp.
            */

            auto intersectSlab =
                [&tMin, &tMax](
                    float origin,
                    float direction,
                    float slabMin,
                    float slabMax
                ) -> bool
                {
                    if (
                        direction >
                            -0.000001f &&
                        direction <
                            0.000001f
                    )
                    {
                        return
                            origin >= slabMin &&
                            origin <= slabMax;
                    }


                    const float inverseDirection =
                        1.0f /
                        direction;


                    float t1 =
                        (
                            slabMin -
                            origin
                        ) *
                        inverseDirection;


                    float t2 =
                        (
                            slabMax -
                            origin
                        ) *
                        inverseDirection;


                    if (t1 > t2)
                    {
                        const float swap =
                            t1;

                        t1 =
                            t2;

                        t2 =
                            swap;
                    }


                    if (t1 > tMin)
                    {
                        tMin =
                            t1;
                    }


                    if (t2 < tMax)
                    {
                        tMax =
                            t2;
                    }


                    return
                        tMax >=
                        tMin;
                };


            if (
                !intersectSlab(
                    rayOrigin.x,
                    rayDirection.x,
                    minX,
                    maxX
                ) ||
                !intersectSlab(
                    rayOrigin.y,
                    rayDirection.y,
                    minY,
                    maxY
                ) ||
                !intersectSlab(
                    rayOrigin.z,
                    rayDirection.z,
                    minZ,
                    maxZ
                )
            )
            {
                continue;
            }


            if (
                tMax <
                    0.0f
            )
            {
                continue;
            }


            float hitDistance =
                tMin;


            /*
                Point-blank case: camera can technically begin
                inside a large creature's bounds.
            */

            if (hitDistance < 0.0f)
            {
                hitDistance =
                    0.0f;
            }


            if (
                hitDistance >
                    SKYDOOM_PISTOL_HITSCAN_RANGE ||
                hitDistance >=
                    nearestDistance
            )
            {
                continue;
            }


            nearestDistance =
                hitDistance;


            nearestActor =
                actor;
        }


        if (!nearestActor)
        {
            SKSE::log::info(
                "[skydoomskse] DOOM chaingun bullet actor hitscan: MISS candidates={}",
                testedActors
            );

            return;
        }


        // SKYDOOM_STRICT_GEOMETRY_OCCLUSION_V4

        /*
            Actor targeting has ALREADY succeeded.

            nearestActor and nearestDistance come from our
            known-good mathematical Actor-hitbox system.

            Havok is used here ONLY to answer:

                "Is solid Skyrim geometry between the camera
                 and the Actor we already selected?"

            Havok is NOT responsible for selecting the NPC.
        */

        auto* occlusionController =
            player->
                GetCharController();


        if (occlusionController)
        {
            auto* occlusionWorld =
                occlusionController->
                    GetHavokWorld();


            if (occlusionWorld)
            {
                /*
                    Start slightly in front of the camera to
                    avoid the player's own controller.

                    For very close targets, shorten this
                    automatically.
                */

                float occlusionStartDistance =
                    nearestDistance *
                    0.25f;


                if (occlusionStartDistance > 48.0f)
                {
                    occlusionStartDistance =
                        48.0f;
                }


                if (occlusionStartDistance < 4.0f)
                {
                    occlusionStartDistance =
                        4.0f;
                }


                /*
                    Stop slightly before the mathematical Actor
                    hitbox so the target itself should not count
                    as scenery.
                */

                const float occlusionEndDistance =
                    nearestDistance -
                    8.0f;


                if (
                    occlusionEndDistance >
                    occlusionStartDistance
                )
                {
                    const RE::NiPoint3
                        occlusionStartWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionStartDistance;


                    const RE::NiPoint3
                        occlusionEndWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionEndDistance;


                    /*
                        Convert Bethesda-world coordinates into
                        the active Havok world's coordinate
                        frame using the player's controller as
                        the origin anchor.
                    */

                    const RE::NiPoint3
                        playerWorld =
                            player->
                                GetPosition();


                    RE::hkVector4
                        playerHavok{};


                    occlusionController->
                        GetPosition(
                            playerHavok,
                            false
                        );

                    const RE::NiPoint3
                        startDeltaWorld{
                            occlusionStartWorld.x -
                                playerWorld.x,

                            occlusionStartWorld.y -
                                playerWorld.y,

                            occlusionStartWorld.z -
                                playerWorld.z
                        };


                    const RE::NiPoint3
                        endDeltaWorld{
                            occlusionEndWorld.x -
                                playerWorld.x,

                            occlusionEndWorld.y -
                                playerWorld.y,

                            occlusionEndWorld.z -
                                playerWorld.z
                        };


                                                            // SKYDOOM_OCCLUSION_DIAGNOSTICS_CLEANED_V4// SKYDOOM_WORLD_SCALE_FIX_V4
                    /*
                        IMPORTANT:

                        Bethesda world-space -> Havok-space uses
                        bhkWorld::GetWorldScale().

                        GetWorldScaleInverse() converts in the
                        opposite direction and was making our
                        camera scenery ray vastly mis-scaled.

                        Keep the existing player-Havok anchor
                        so shifted Havok world origins continue
                        to be handled correctly.
                    */

                    const float havokScale =
                        RE::bhkWorld::
                            GetWorldScale();


                    const RE::hkVector4
                        havokScaleVector{
                            havokScale
                        };




                    const RE::hkVector4
                        occlusionStartHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    startDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    const RE::hkVector4
                        occlusionEndHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    endDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    RE::bhkPickData
                        occlusionPick{};


                    occlusionPick.
                        rayInput.
                        enableShapeCollectionFilter =
                            true;


                    occlusionPick.
                        rayInput.
                        from =
                            occlusionStartHavok;


                    occlusionPick.
                        rayInput.
                        to =
                            occlusionEndHavok;


                    /*
                        Start from the player's actual collision
                        filter so Skyrim's current system-group
                        information is retained.
                    */

                    occlusionController->
                        GetCollisionFilterInfo(
                            occlusionPick.
                                rayInput.
                                filterInfo
                        );


                    /*
                        Then change only the collision layer used
                        for the scenery query.
                    */

                                        // SKYDOOM_CONTROLLER_FILTER_OCCLUSION_V4
                    /*
                        IMPORTANT:

                        Keep the collision filter returned by the
                        player's bhkCharacterController exactly
                        as Skyrim supplied it.

                        Do NOT replace its collision layer.

                        The controller's real filter already
                        contains the layer/system-group data used
                        to collide with solid Skyrim world
                        geometry.
                    */


                    /*
                        bhkPickData contains an explicit direction
                        vector as well as the input endpoints.
                    */

                    // SKYDOOM_NO_MANUAL_PICK_RAY_V4
                    /*
                        Do NOT populate occlusionPick.ray here.

                        Our successful native-Havok ground probe,
                        and known working Skyrim/CommonLib
                        PickObject usage, populate only:

                            rayInput.from
                            rayInput.to
                            rayInput.filterInfo

                        Let bhkWorld::PickObject handle the rest.
                    */


                    const bool occlusionHit =
                        occlusionWorld->
                            PickObject(
                                occlusionPick
                            );


                    if (
                        occlusionHit &&
                        occlusionPick.
                            rayOutput.
                            rootCollidable
                    )
                    {
                        auto* blockerRef =
                            RE::TESHavokUtilities::
                                FindCollidableRef(
                                    *occlusionPick.
                                        rayOutput.
                                        rootCollidable
                                );


                        /*
                            Do not let the selected target's own
                            physics body count as scenery.
                        */

                        if (
                            blockerRef !=
                            nearestActor
                        )
                        {
                            const float rayLength =
                                occlusionEndDistance -
                                occlusionStartDistance;


                            const float blockerDistance =
                                occlusionStartDistance +
                                (
                                    occlusionPick.
                                        rayOutput.
                                        hitFraction *
                                    rayLength
                                );


                            if (blockerRef)
                            {
                                SKSE::log::info(
                                    "[skydoomskse] DOOM chaingun bullet geometry BLOCKED: target={:08X} blocker={:08X} blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerRef->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }
                            else
                            {
                                /*
                                    Static architecture and
                                    terrain can have collision
                                    without a normal TESObjectREFR.

                                    It still blocks the bullet.
                                */

                                SKSE::log::info(
                                    "[skydoomskse] DOOM chaingun bullet geometry BLOCKED: target={:08X} blocker=STATIC blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }


                            return;
                        }
                    }


                                        SKSE::log::info(
                        "[skydoomskse] DOOM chaingun bullet geometry CLEAR: target={:08X} distance={} filter=0x{:08X}",
                        nearestActor->
                            GetFormID(),
                        nearestDistance,
                        occlusionPick.
                            rayInput.
                            filterInfo.
                            filter
                    );
                }
            }
        }

        auto* actorValueOwner =
            nearestActor->
                AsActorValueOwner();


        if (!actorValueOwner)
        {
            return;
        }


        const float healthBefore =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        if (healthBefore <= 0.0f)
        {
            return;
        }


        const float damage =

            static_cast<float>(

                doomDamage

            );


        /*
            SkyCraft uses Skyrim's HitData pipeline when
            available and falls back to Actor::DoDamage().

            For this SkyDoom milestone we use that reliable
            native fallback rather than merely editing the
            Health ActorValue.

            This also gives Skyrim the player as the attacker.
        */

        /*

            SKYDOOM_NATIVE_HITDATA_DELIVERY



            Target selection and geometry occlusion have already

            succeeded above.



            The exact damage value still came from Chocolate Doom.

        */



        const RE::NiPoint3

            nativeHitPosition =

                rayOrigin +

                rayDirection *

                    nearestDistance;





        const bool

            usedNativeHitData =

                ApplySkyDoomNativePistolHit(

                    player,

                    nearestActor,

                    damage,

                    nativeHitPosition,

                    rayDirection

                );





        const char*

            damagePath =

                usedNativeHitData ?

                    "HitData" :

                    "DoDamage";





        /*

            Safety fallback.



            If our runtime/address validation ever rejects the

            native HitData path, retain the already-proven v5

            behaviour rather than losing DOOM combat entirely.

        */



        if (!usedNativeHitData)

        {

            nearestActor->

                DoDamage(

                    damage,

                    player,

                    true

                );

        }


        const float healthAfter =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        /*
            Make a surviving non-teammate respond to the player.

            SkyCraft does the same after applying its Minecraft
            weapon hit.
        */

        if (
            !nearestActor->
                IsDead() &&
            !nearestActor->
                IsPlayerTeammate() &&
            !nearestActor->
                IsInCombat()
        )
        {
            nearestActor->
                StartCombat(
                    player
                );
        }


        SKSE::log::info(
            "[skydoomskse] DOOM chaingun bullet actor HIT: actor={:08X} doomDamage={} appliedDamage={} distance={} health={} -> {} candidates={} path={}",
            nearestActor->
                GetFormID(),
            doomDamage,
            damage,
            nearestDistance,
            healthBefore,
            healthAfter,
            testedActors,
            damagePath
        );
    }





    void ApplyDoomMeleeDamageToCrosshairActor(

        std::int32_t doomDamage,

        std::int32_t doomAngleOffset

    )
    {
        // SKYDOOM_ACTOR_HITBOX_V4

        /*
            A real Chocolate Doom pistol shot has already been
            confirmed by ammo consumption before this function
            is called.

            Target selection here deliberately follows the same
            broad architecture SkyCraft uses for Skyrim NPCs:

              Skyrim ProcessLists
                  ->
              highActorHandles
                  ->
              real Actor position / height / radius

            We do NOT use CrosshairPickData, bhkWorld::PickObject
            or TES::Pick here.

            Instead we cast the centre-camera ray mathematically
            against each nearby Skyrim Actor's bounds and select
            the nearest intersected Actor.
        */

        if (doomDamage <= 0)
        {
            return;
        }


        auto* player =
            RE::PlayerCharacter::
                GetSingleton();


        if (!player)
        {
            return;
        }


        auto* camera =
            RE::PlayerCamera::
                GetSingleton();


        if (
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return;
        }


        /*
            The DOOM weapon sprite is centred on screen.

            Therefore the Skyrim camera's forward vector is our
            hitscan direction.
        */

        const RE::NiPoint3
            rayOrigin =
                RE::PlayerCamera::
                    GetActiveCameraPosition();


        RE::NiPoint3 rayDirection =
            camera->
                cameraRoot->
                world.
                rotate *
            RE::NiPoint3{
                0.0f,
                1.0f,
                0.0f
            };



        /*

            SKYDOOM_MELEE_RANGE_AND_SPREAD_V10



            DOOM angle_t represents one full revolution as 2^32.



            doomAngleOffset is the exact signed delta that

            Chocolate Doom already used for this pellet.



            Rotate only around Skyrim world Z so the original

            camera pitch is preserved.

        */



        const double

            doomSpreadRadians =

                static_cast<double>(

                    doomAngleOffset

                ) *

                (

                    6.28318530717958647692 /

                    4294967296.0

                );





        const float spreadCos =

            static_cast<float>(

                std::cos(

                    doomSpreadRadians

                )

            );





        const float spreadSin =

            static_cast<float>(

                std::sin(

                    doomSpreadRadians

                )

            );





        const float baseRayX =

            rayDirection.x;





        const float baseRayY =

            rayDirection.y;





        rayDirection.x =

            baseRayX *

                spreadCos -

            baseRayY *

                spreadSin;





        rayDirection.y =

            baseRayX *

                spreadSin +

            baseRayY *

                spreadCos;






        if (
            rayDirection.
                Unitize() <=
            0.0f
        )
        {
            return;
        }


        auto* processLists =
            RE::ProcessLists::
                GetSingleton();


        if (!processLists)
        {
            return;
        }


        RE::Actor*
            nearestActor =
                nullptr;


        float nearestDistance =
            150.0f +
            1.0f;


        std::uint32_t
            testedActors =
                0;


        /*
            Ray versus axis-aligned Actor bounds.

            Skyrim's Actor::GetPosition() is treated as the feet
            position, matching the way SkyCraft exports Actors.

            Width is based on GetBoundRadius()*2.
            Height is based on GetHeight().

            The dimensions are constrained to the same broad
            ranges SkyCraft uses for its NPC proxy dimensions:

              height: 21 .. 840 Skyrim units
              width : 21 .. 420 Skyrim units
        */

        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();


            auto* actor =
                actorPtr.
                    get();


            if (
                !actor ||
                actor == player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost() ||
                actor->
                    IsDead()
            )
            {
                continue;
            }


            ++testedActors;


            const RE::NiPoint3
                actorPosition =
                    actor->
                        GetPosition();


            /*
                Reject Actors that cannot possibly be within the
                pistol's maximum distance before doing the more
                expensive slab intersection.

                Include their dimensions in the allowance so
                large creatures near the range boundary remain
                hittable.
            */

            const float
                dx =
                    actorPosition.x -
                    rayOrigin.x;


            const float
                dy =
                    actorPosition.y -
                    rayOrigin.y;


            const float
                dz =
                    actorPosition.z -
                    rayOrigin.z;


            const float
                roughDistanceSquared =
                    dx * dx +
                    dy * dy +
                    dz * dz;


            const float
                maxCandidateDistance =
                    150.0f +
                    900.0f;


            if (
                roughDistanceSquared >
                maxCandidateDistance *
                    maxCandidateDistance
            )
            {
                continue;
            }


            float actorHeight =
                actor->
                    GetHeight();


            if (actorHeight < 21.0f)
            {
                actorHeight =
                    21.0f;
            }


            if (actorHeight > 840.0f)
            {
                actorHeight =
                    840.0f;
            }


            float actorWidth =
                actor->
                    GetBoundRadius() *
                2.0f;


            if (actorWidth < 21.0f)
            {
                actorWidth =
                    21.0f;
            }


            if (actorWidth > 420.0f)
            {
                actorWidth =
                    420.0f;
            }


            /*
                A tiny amount of extra width makes the pistol
                feel like a normal game hitscan rather than a
                mathematically perfect one-pixel laser.

                This is only 4 Skyrim units on each side.
            */

            const float actorRadius =
                actorWidth *
                    0.5f +
                4.0f;


            const float minX =
                actorPosition.x -
                actorRadius;


            const float maxX =
                actorPosition.x +
                actorRadius;


            const float minY =
                actorPosition.y -
                actorRadius;


            const float maxY =
                actorPosition.y +
                actorRadius;


            /*
                ActorPosition is the feet position.

                Give the bottom a small allowance because actors
                can visually sink slightly into terrain.
            */

            const float minZ =
                actorPosition.z -
                4.0f;


            const float maxZ =
                actorPosition.z +
                actorHeight;


            float tMin =
                0.0f;


            float tMax =
                150.0f;


            /*
                Standard ray/AABB slab test.

                Keeping it inline avoids introducing another
                helper or changing any other part of Plugin.cpp.
            */

            auto intersectSlab =
                [&tMin, &tMax](
                    float origin,
                    float direction,
                    float slabMin,
                    float slabMax
                ) -> bool
                {
                    if (
                        direction >
                            -0.000001f &&
                        direction <
                            0.000001f
                    )
                    {
                        return
                            origin >= slabMin &&
                            origin <= slabMax;
                    }


                    const float inverseDirection =
                        1.0f /
                        direction;


                    float t1 =
                        (
                            slabMin -
                            origin
                        ) *
                        inverseDirection;


                    float t2 =
                        (
                            slabMax -
                            origin
                        ) *
                        inverseDirection;


                    if (t1 > t2)
                    {
                        const float swap =
                            t1;

                        t1 =
                            t2;

                        t2 =
                            swap;
                    }


                    if (t1 > tMin)
                    {
                        tMin =
                            t1;
                    }


                    if (t2 < tMax)
                    {
                        tMax =
                            t2;
                    }


                    return
                        tMax >=
                        tMin;
                };


            if (
                !intersectSlab(
                    rayOrigin.x,
                    rayDirection.x,
                    minX,
                    maxX
                ) ||
                !intersectSlab(
                    rayOrigin.y,
                    rayDirection.y,
                    minY,
                    maxY
                ) ||
                !intersectSlab(
                    rayOrigin.z,
                    rayDirection.z,
                    minZ,
                    maxZ
                )
            )
            {
                continue;
            }


            if (
                tMax <
                    0.0f
            )
            {
                continue;
            }


            float hitDistance =
                tMin;


            /*
                Point-blank case: camera can technically begin
                inside a large creature's bounds.
            */

            if (hitDistance < 0.0f)
            {
                hitDistance =
                    0.0f;
            }


            if (
                hitDistance >
                    150.0f ||
                hitDistance >=
                    nearestDistance
            )
            {
                continue;
            }


            nearestDistance =
                hitDistance;


            nearestActor =
                actor;
        }


        if (!nearestActor)
        {
            SKSE::log::info(
                "[skydoomskse] DOOM melee attack actor hitscan: MISS candidates={}",
                testedActors
            );

            return;
        }


        // SKYDOOM_STRICT_GEOMETRY_OCCLUSION_V4

        /*
            Actor targeting has ALREADY succeeded.

            nearestActor and nearestDistance come from our
            known-good mathematical Actor-hitbox system.

            Havok is used here ONLY to answer:

                "Is solid Skyrim geometry between the camera
                 and the Actor we already selected?"

            Havok is NOT responsible for selecting the NPC.
        */

        auto* occlusionController =
            player->
                GetCharController();


        if (occlusionController)
        {
            auto* occlusionWorld =
                occlusionController->
                    GetHavokWorld();


            if (occlusionWorld)
            {
                /*
                    Start slightly in front of the camera to
                    avoid the player's own controller.

                    For very close targets, shorten this
                    automatically.
                */

                float occlusionStartDistance =
                    nearestDistance *
                    0.25f;


                if (occlusionStartDistance > 48.0f)
                {
                    occlusionStartDistance =
                        48.0f;
                }


                if (occlusionStartDistance < 4.0f)
                {
                    occlusionStartDistance =
                        4.0f;
                }


                /*
                    Stop slightly before the mathematical Actor
                    hitbox so the target itself should not count
                    as scenery.
                */

                const float occlusionEndDistance =
                    nearestDistance -
                    8.0f;


                if (
                    occlusionEndDistance >
                    occlusionStartDistance
                )
                {
                    const RE::NiPoint3
                        occlusionStartWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionStartDistance;


                    const RE::NiPoint3
                        occlusionEndWorld =
                            rayOrigin +
                            rayDirection *
                                occlusionEndDistance;


                    /*
                        Convert Bethesda-world coordinates into
                        the active Havok world's coordinate
                        frame using the player's controller as
                        the origin anchor.
                    */

                    const RE::NiPoint3
                        playerWorld =
                            player->
                                GetPosition();


                    RE::hkVector4
                        playerHavok{};


                    occlusionController->
                        GetPosition(
                            playerHavok,
                            false
                        );

                    const RE::NiPoint3
                        startDeltaWorld{
                            occlusionStartWorld.x -
                                playerWorld.x,

                            occlusionStartWorld.y -
                                playerWorld.y,

                            occlusionStartWorld.z -
                                playerWorld.z
                        };


                    const RE::NiPoint3
                        endDeltaWorld{
                            occlusionEndWorld.x -
                                playerWorld.x,

                            occlusionEndWorld.y -
                                playerWorld.y,

                            occlusionEndWorld.z -
                                playerWorld.z
                        };


                                                            // SKYDOOM_OCCLUSION_DIAGNOSTICS_CLEANED_V4// SKYDOOM_WORLD_SCALE_FIX_V4
                    /*
                        IMPORTANT:

                        Bethesda world-space -> Havok-space uses
                        bhkWorld::GetWorldScale().

                        GetWorldScaleInverse() converts in the
                        opposite direction and was making our
                        camera scenery ray vastly mis-scaled.

                        Keep the existing player-Havok anchor
                        so shifted Havok world origins continue
                        to be handled correctly.
                    */

                    const float havokScale =
                        RE::bhkWorld::
                            GetWorldScale();


                    const RE::hkVector4
                        havokScaleVector{
                            havokScale
                        };




                    const RE::hkVector4
                        occlusionStartHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    startDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    const RE::hkVector4
                        occlusionEndHavok =
                            playerHavok +
                            (
                                RE::hkVector4(
                                    endDeltaWorld
                                ) *
                                havokScaleVector
                            );


                    RE::bhkPickData
                        occlusionPick{};


                    occlusionPick.
                        rayInput.
                        enableShapeCollectionFilter =
                            true;


                    occlusionPick.
                        rayInput.
                        from =
                            occlusionStartHavok;


                    occlusionPick.
                        rayInput.
                        to =
                            occlusionEndHavok;


                    /*
                        Start from the player's actual collision
                        filter so Skyrim's current system-group
                        information is retained.
                    */

                    occlusionController->
                        GetCollisionFilterInfo(
                            occlusionPick.
                                rayInput.
                                filterInfo
                        );


                    /*
                        Then change only the collision layer used
                        for the scenery query.
                    */

                                        // SKYDOOM_CONTROLLER_FILTER_OCCLUSION_V4
                    /*
                        IMPORTANT:

                        Keep the collision filter returned by the
                        player's bhkCharacterController exactly
                        as Skyrim supplied it.

                        Do NOT replace its collision layer.

                        The controller's real filter already
                        contains the layer/system-group data used
                        to collide with solid Skyrim world
                        geometry.
                    */


                    /*
                        bhkPickData contains an explicit direction
                        vector as well as the input endpoints.
                    */

                    // SKYDOOM_NO_MANUAL_PICK_RAY_V4
                    /*
                        Do NOT populate occlusionPick.ray here.

                        Our successful native-Havok ground probe,
                        and known working Skyrim/CommonLib
                        PickObject usage, populate only:

                            rayInput.from
                            rayInput.to
                            rayInput.filterInfo

                        Let bhkWorld::PickObject handle the rest.
                    */


                    const bool occlusionHit =
                        occlusionWorld->
                            PickObject(
                                occlusionPick
                            );


                    if (
                        occlusionHit &&
                        occlusionPick.
                            rayOutput.
                            rootCollidable
                    )
                    {
                        auto* blockerRef =
                            RE::TESHavokUtilities::
                                FindCollidableRef(
                                    *occlusionPick.
                                        rayOutput.
                                        rootCollidable
                                );


                        /*
                            Do not let the selected target's own
                            physics body count as scenery.
                        */

                        if (
                            blockerRef !=
                            nearestActor
                        )
                        {
                            const float rayLength =
                                occlusionEndDistance -
                                occlusionStartDistance;


                            const float blockerDistance =
                                occlusionStartDistance +
                                (
                                    occlusionPick.
                                        rayOutput.
                                        hitFraction *
                                    rayLength
                                );


                            if (blockerRef)
                            {
                                SKSE::log::info(
                                    "[skydoomskse] DOOM melee attack geometry BLOCKED: target={:08X} blocker={:08X} blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerRef->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }
                            else
                            {
                                /*
                                    Static architecture and
                                    terrain can have collision
                                    without a normal TESObjectREFR.

                                    It still blocks the bullet.
                                */

                                SKSE::log::info(
                                    "[skydoomskse] DOOM melee attack geometry BLOCKED: target={:08X} blocker=STATIC blockerDistance={} targetDistance={}",
                                    nearestActor->
                                        GetFormID(),
                                    blockerDistance,
                                    nearestDistance
                                );
                            }


                            return;
                        }
                    }


                                        SKSE::log::info(
                        "[skydoomskse] DOOM melee attack geometry CLEAR: target={:08X} distance={} filter=0x{:08X}",
                        nearestActor->
                            GetFormID(),
                        nearestDistance,
                        occlusionPick.
                            rayInput.
                            filterInfo.
                            filter
                    );
                }
            }
        }

        auto* actorValueOwner =
            nearestActor->
                AsActorValueOwner();


        if (!actorValueOwner)
        {
            return;
        }


        const float healthBefore =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        if (healthBefore <= 0.0f)
        {
            return;
        }


        const float damage =

            static_cast<float>(

                doomDamage

            );


        /*
            SkyCraft uses Skyrim's HitData pipeline when
            available and falls back to Actor::DoDamage().

            For this SkyDoom milestone we use that reliable
            native fallback rather than merely editing the
            Health ActorValue.

            This also gives Skyrim the player as the attacker.
        */

        /*

            SKYDOOM_NATIVE_HITDATA_DELIVERY



            Target selection and geometry occlusion have already

            succeeded above.



            The exact damage value still came from Chocolate Doom.

        */



        const RE::NiPoint3

            nativeHitPosition =

                rayOrigin +

                rayDirection *

                    nearestDistance;





        const bool

            usedNativeHitData =

                ApplySkyDoomNativePistolHit(

                    player,

                    nearestActor,

                    damage,

                    nativeHitPosition,

                    rayDirection

                );





        const char*

            damagePath =

                usedNativeHitData ?

                    "HitData" :

                    "DoDamage";





        /*

            Safety fallback.



            If our runtime/address validation ever rejects the

            native HitData path, retain the already-proven v5

            behaviour rather than losing DOOM combat entirely.

        */



        if (!usedNativeHitData)

        {

            nearestActor->

                DoDamage(

                    damage,

                    player,

                    true

                );

        }


        const float healthAfter =
            actorValueOwner->
                GetActorValue(
                    RE::ActorValue::
                        kHealth
                );


        /*
            Make a surviving non-teammate respond to the player.

            SkyCraft does the same after applying its Minecraft
            weapon hit.
        */

        if (
            !nearestActor->
                IsDead() &&
            !nearestActor->
                IsPlayerTeammate() &&
            !nearestActor->
                IsInCombat()
        )
        {
            nearestActor->
                StartCombat(
                    player
                );
        }


        SKSE::log::info(
            "[skydoomskse] DOOM melee attack actor HIT: actor={:08X} doomDamage={} appliedDamage={} distance={} health={} -> {} candidates={} path={}",
            nearestActor->
                GetFormID(),
            doomDamage,
            damage,
            nearestDistance,
            healthBefore,
            healthAfter,
            testedActors,
            damagePath
        );
    }







    void ProcessDoomPistolCombatBridge(

        bool doomFresh

    )

    {

        /*

            SKYDOOM v5



            Chocolate Doom itself now owns:



              - the real pistol firing event

              - the 5 / 10 / 15 damage roll



            Skyrim owns only:



              - which Skyrim Actor the centre-screen shot reaches

              - world/wall occlusion

              - applying that DOOM damage to the Actor

        */



        if (

            !g_state ||

            !doomFresh ||

            !g_state->doom.running ||

            !g_state->doom.in_level ||

            !g_state->skyrim.in_game ||

            g_state->skyrim.paused

        )

        {

            ResetDoomCombatSnapshot();

            return;

        }





        const std::int32_t bullets =

            g_state->doom.ammo_bullets;





        const std::int32_t weapon =

            g_state->doom.weapon;





        const std::uint64_t tick =

            g_state->doom.tick_counter;





        const std::uint64_t shotSerial =

            g_state->

                doom.

                pistol_shot_serial;





        const std::uint64_t damageTotal =

            g_state->

                doom.

                pistol_damage_total;





        /*

            First valid sample or guest/protocol restart:

            establish a baseline without synthesising a shot.

        */



        if (

            !g_haveDoomCombatSnapshot ||

            tick <

                g_lastDoomCombatTick ||

            shotSerial <

                g_lastDoomPistolShotSerial ||

            damageTotal <

                g_lastDoomPistolDamageTotal

        )

        {

            g_lastDoomBullets =

                bullets;



            g_lastDoomWeapon =

                weapon;



            g_lastDoomCombatTick =

                tick;



            g_lastDoomPistolShotSerial =

                shotSerial;



            g_lastDoomPistolDamageTotal =

                damageTotal;



            g_haveDoomCombatSnapshot =

                true;



            return;

        }





        const std::uint64_t

            previousShotSerial =

                g_lastDoomPistolShotSerial;





        const std::uint64_t

            previousDamageTotal =

                g_lastDoomPistolDamageTotal;





        /*

            Consume the counters before touching Skyrim so this

            exact DOOM event can never be applied twice.

        */



        g_lastDoomBullets =

            bullets;



        g_lastDoomWeapon =

            weapon;



        g_lastDoomCombatTick =

            tick;



        g_lastDoomPistolShotSerial =

            shotSerial;



        g_lastDoomPistolDamageTotal =

            damageTotal;





        if (

            shotSerial ==

            previousShotSerial

        )

        {

            return;

        }





        const std::uint64_t eventCount =

            shotSerial -

            previousShotSerial;





        const std::uint64_t damageDelta =

            damageTotal -

            previousDamageTotal;





        if (

            damageDelta == 0 ||

            damageDelta >

                2147483647ULL

        )

        {

            SKSE::log::warn(

                "[skydoomskse] Invalid DOOM pistol damage event: serial={} events={} damageDelta={}",

                shotSerial,

                eventCount,

                damageDelta

            );



            return;

        }





        const std::int32_t doomDamage =

            static_cast<std::int32_t>(

                damageDelta

            );





        SKSE::log::info(

            "[skydoomskse] REAL DOOM pistol event: serial={} events={} damage={} bullets={} weapon={} tick={}",

            shotSerial,

            eventCount,

            doomDamage,

            bullets,

            weapon,

            tick

        );





        ApplyDoomPistolDamageToCrosshairActor(

            doomDamage

        );

    }





    // ========================================================

    // SKYDOOM_SKYRIM_ROCKET_PROJECTILE_V11

    //

    // Chocolate Doom owns:

    //   weapon state

    //   ammo

    //   cadence

    //   weapon animation

    //   launch sound

    //   direct-hit RNG

    //

    // Skyrim owns:

    //   projectile world position

    //   projectile collision

    //   explosion position

    //   scenery line-of-sight

    //

    // DOOM rocket constants:

    //

    //   speed       = 20 units / tic

    //   tic rate    = 35 Hz

    //   speed       = 700 DOOM units / second

    //   radius      = 11 DOOM units

    //   height      = 8 DOOM units

    //   splash base = 128

    //

    // One DOOM unit is scaled into Skyrim using the ratio

    // between Skyrim player height and DOOM's 56-unit player.

    // ========================================================





    constexpr std::uint32_t

        SKYDOOM_MAX_ACTIVE_ROCKETS =

            16u;





    constexpr std::uint32_t

        SKYDOOM_MAX_PENDING_ROCKET_IMPACTS =

            16u;





    struct SkyDoomLogicalRocket

    {

        bool active = false;



        RE::NiPoint3 position{};



        RE::NiPoint3 direction{};



        float doomToSkyrimScale =

            1.0f;



        std::uint64_t bornMs =

            0;



        std::uint64_t lastUpdateMs =

            0;

    };





    struct SkyDoomPendingRocketImpact

    {

        bool active = false;



        std::int32_t requestId =

            0;



        std::uint32_t targetFormId =

            0;



        RE::NiPoint3 impactPosition{};



        RE::NiPoint3 direction{};



        float doomToSkyrimScale =

            1.0f;



        std::uint64_t createdMs =

            0;

    };





    SkyDoomLogicalRocket

        g_skyDoomRockets[

            SKYDOOM_MAX_ACTIVE_ROCKETS

        ]{};





    SkyDoomPendingRocketImpact

        g_skyDoomPendingRocketImpacts[

            SKYDOOM_MAX_PENDING_ROCKET_IMPACTS

        ]{};





    std::int32_t

        g_skyDoomNextRocketRequestId =

            1;



    // ========================================================

    // SKYDOOM_VISIBLE_ROCKET_SPRITE_V11_1

    //

    // The v11 logical projectile remains authoritative.

    //

    // This section is presentation only:

    //

    //   DOOM.WAD -> MISLA0

    //   PLAYPAL -> genuine DOOM colours

    //   logical Skyrim rocket position -> NiCamera projection

    //   D3D11 camera-facing screen-space billboard

    //

    // No projectile damage/collision logic is changed here.

    // ========================================================





    struct SkyDoomRocketRenderSnapshot

    {

        bool active =

            false;





        RE::NiPoint3 position{};





        RE::NiPoint3 direction{};





        float doomToSkyrimScale =

            1.0f;





        std::uint64_t sampleMs =

            0;

    };





    std::mutex

        g_skyDoomRocketRenderMutex;





    std::array<

        SkyDoomRocketRenderSnapshot,

        SKYDOOM_MAX_ACTIVE_ROCKETS>

        g_skyDoomRocketRenderSnapshots{};





    ComPtr<

        ID3D11Texture2D>

        g_skyDoomRocketSpriteTexture;





    ComPtr<

        ID3D11ShaderResourceView>

        g_skyDoomRocketSpriteSRV;





    std::uint32_t

        g_skyDoomRocketSpriteWidth =

            0;





    std::uint32_t

        g_skyDoomRocketSpriteHeight =

            0;





    bool

        g_skyDoomRocketSpriteLoadAttempted =

            false;





    void PublishSkyDoomRocketRenderSnapshots()

    {

        const auto now =

            static_cast<

                std::uint64_t

            >(

                GetTickCount64()

            );





        std::lock_guard<

            std::mutex>

            lock(

                g_skyDoomRocketRenderMutex

            );





        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_ACTIVE_ROCKETS;

            ++i

        )

        {

            auto&

                destination =

                    g_skyDoomRocketRenderSnapshots[

                        i

                    ];





            const auto&

                source =

                    g_skyDoomRockets[

                        i

                    ];





            destination.active =

                source.active;





            destination.position =

                source.position;





            destination.direction =

                source.direction;





            destination.doomToSkyrimScale =

                source.doomToSkyrimScale;





            destination.sampleMs =

                now;

        }

    }





    std::uint16_t SkyDoomReadLE16(

        const std::uint8_t* data

    )

    {

        return

            static_cast<

                std::uint16_t

            >(

                static_cast<

                    std::uint16_t

                >(

                    data[0]

                ) |

                (

                    static_cast<

                        std::uint16_t

                    >(

                        data[1]

                    )

                    <<

                    8

                )

            );

    }





    std::uint32_t SkyDoomReadLE32(

        const std::uint8_t* data

    )

    {

        return

            static_cast<

                std::uint32_t

            >(

                static_cast<

                    std::uint32_t

                >(

                    data[0]

                ) |

                (

                    static_cast<

                        std::uint32_t

                    >(

                        data[1]

                    )

                    <<

                    8

                ) |

                (

                    static_cast<

                        std::uint32_t

                    >(

                        data[2]

                    )

                    <<

                    16

                ) |

                (

                    static_cast<

                        std::uint32_t

                    >(

                        data[3]

                    )

                    <<

                    24

                )

            );

    }





    bool EnsureSkyDoomRocketSpriteTexture()

    {

        if (

            g_skyDoomRocketSpriteSRV

        )

        {

            return true;

        }





        if (

            !g_device

        )

        {

            return false;

        }





        if (

            g_skyDoomRocketSpriteLoadAttempted

        )

        {

            return false;

        }





        g_skyDoomRocketSpriteLoadAttempted =

            true;





        HANDLE file =

            CreateFileW(

                g_doomWadPath.c_str(),

                GENERIC_READ,

                FILE_SHARE_READ,

                nullptr,

                OPEN_EXISTING,

                FILE_ATTRIBUTE_NORMAL,

                nullptr

            );





        if (

            file ==

            INVALID_HANDLE_VALUE

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: could not open DOOM.WAD"

            );



            return false;

        }





        LARGE_INTEGER

            fileSize{};





        if (

            !GetFileSizeEx(

                file,

                &fileSize

            ) ||

            fileSize.QuadPart <

                12 ||

            fileSize.QuadPart >

                64ll *

                1024ll *

                1024ll

        )

        {

            CloseHandle(

                file

            );





            SKSE::log::error(

                "[skydoomskse] Visible rocket: invalid DOOM.WAD size"

            );



            return false;

        }





        std::vector<

            std::uint8_t>

            wad(

                static_cast<

                    std::size_t

                >(

                    fileSize.

                        QuadPart

                )

            );





        std::size_t totalRead =

            0;





        while (

            totalRead <

            wad.size()

        )

        {

            const std::size_t

                remaining =

                    wad.size() -

                    totalRead;





            const DWORD

                request =

                    static_cast<

                        DWORD

                    >(

                        remaining >

                            1024u *

                            1024u ?

                            1024u *

                            1024u :

                            remaining

                    );





            DWORD bytesRead =

                0;





            if (

                !ReadFile(

                    file,

                    wad.data() +

                        totalRead,

                    request,

                    &bytesRead,

                    nullptr

                ) ||

                bytesRead ==

                    0

            )

            {

                CloseHandle(

                    file

                );





                SKSE::log::error(

                    "[skydoomskse] Visible rocket: failed reading DOOM.WAD"

                );



                return false;

            }





            totalRead +=

                bytesRead;

        }





        CloseHandle(

            file

        );





        if (

            wad.size() <

            12

        )

        {

            return false;

        }





        const std::uint32_t

            lumpCount =

                SkyDoomReadLE32(

                    wad.data() +

                    4

                );





        const std::uint32_t

            directoryOffset =

                SkyDoomReadLE32(

                    wad.data() +

                    8

                );





        if (

            lumpCount ==

                0 ||

            directoryOffset >=

                wad.size() ||

            static_cast<

                std::uint64_t

            >(

                directoryOffset

            ) +

                static_cast<

                    std::uint64_t

                >(

                    lumpCount

                ) *

                16ull >

                wad.size()

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: invalid WAD directory"

            );



            return false;

        }





        struct LumpLocation

        {

            bool found =

                false;





            std::uint32_t offset =

                0;





            std::uint32_t size =

                0;

        };





        auto findLump =

            [

                &wad,

                lumpCount,

                directoryOffset

            ](

                const char* wanted

            ) -> LumpLocation

            {

                char wantedName[

                    8

                ]{};





                for (

                    std::uint32_t i = 0;

                    i <

                        8u &&

                    wanted[i] !=

                        '\0';

                    ++i

                )

                {

                    wantedName[i] =

                        wanted[i];

                }





                for (

                    std::uint32_t i = 0;

                    i <

                        lumpCount;

                    ++i

                )

                {

                    const std::size_t

                        entryOffset =

                            static_cast<

                                std::size_t

                            >(

                                directoryOffset

                            ) +

                            static_cast<

                                std::size_t

                            >(

                                i

                            ) *

                            16u;





                    bool nameMatches =

                        true;





                    for (

                        std::uint32_t c = 0;

                        c <

                            8u;

                        ++c

                    )

                    {

                        if (

                            static_cast<

                                char

                            >(

                                wad[

                                    entryOffset +

                                    8u +

                                    c

                                ]

                            ) !=

                            wantedName[c]

                        )

                        {

                            nameMatches =

                                false;



                            break;

                        }

                    }





                    if (!nameMatches)

                    {

                        continue;

                    }





                    LumpLocation

                        result{};





                    result.offset =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset

                        );





                    result.size =

                        SkyDoomReadLE32(

                            wad.data() +

                            entryOffset +

                            4u

                        );





                    if (

                        static_cast<

                            std::uint64_t

                        >(

                            result.offset

                        ) +

                            result.size >

                        wad.size()

                    )

                    {

                        return

                            LumpLocation{};

                    }





                    result.found =

                        true;





                    return result;

                }





                return

                    LumpLocation{};

            };





                const LumpLocation
            paletteLump =
                findLump(
                    "PLAYPAL"
                );


        /*
            SKYDOOM_ROTATED_ROCKET_SPRITE_FIX_V11_1

            The DOOM rocket is a rotational sprite rather than an
            A0 single-view sprite.

            Prefer rotation 5 for our own rocket travelling away
            from the camera, then fall back through the other
            standard combined/rotational MISL A-frame lumps.

            This changes presentation only. The authoritative
            v11 logical projectile is completely untouched.
        */

        LumpLocation
            rocketLump{};


        const char*
            selectedRocketLumpName =
                nullptr;


        const char*
            rocketCandidates[]{
                "MISLA5",
                "MISLA1",
                "MISLA2A8",
                "MISLA3A7",
                "MISLA4A6",
                "MISLA2",
                "MISLA3",
                "MISLA4",
                "MISLA6",
                "MISLA7",
                "MISLA8",
                "MISLA0"
            };


        for (
            const char*
                candidate :
                    rocketCandidates
        )
        {
            const LumpLocation
                candidateLump =
                    findLump(
                        candidate
                    );


            if (
                candidateLump.
                    found
            )
            {
                rocketLump =
                    candidateLump;


                selectedRocketLumpName =
                    candidate;


                break;
            }
        }





        if (

            !paletteLump.found ||

            paletteLump.size <

                768u

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: PLAYPAL lump not found"

            );



            return false;

        }





        if (

            !rocketLump.found ||

            rocketLump.size <

                12u

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: no usable MISL A-frame rotation lump found in DOOM.WAD"

            );



            return false;

        }





        const auto*

            patch =

                wad.data() +

                rocketLump.

                    offset;





        const std::uint32_t

            width =

                SkyDoomReadLE16(

                    patch +

                    0

                );





        const std::uint32_t

            height =

                SkyDoomReadLE16(

                    patch +

                    2

                );





        if (

            width ==

                0 ||

            height ==

                0 ||

            width >

                512u ||

            height >

                512u ||

            8u +

                width *

                4u >

                rocketLump.

                    size

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: invalid MISLA0 patch dimensions {}x{}",

                width,

                height

            );



            return false;

        }





        std::vector<

            std::uint8_t>

            rgba(

                static_cast<

                    std::size_t

                >(

                    width

                ) *

                static_cast<

                    std::size_t

                >(

                    height

                ) *

                4u,

                0u

            );





        const auto*

            palette =

                wad.data() +

                paletteLump.

                    offset;





        const std::size_t

            rocketStart =

                rocketLump.

                    offset;





        const std::size_t

            rocketEnd =

                rocketStart +

                rocketLump.

                    size;





        for (

            std::uint32_t x = 0;

            x <

                width;

            ++x

        )

        {

            const std::uint32_t

                columnOffset =

                    SkyDoomReadLE32(

                        patch +

                        8u +

                        x *

                        4u

                    );





            std::size_t pos =

                rocketStart +

                columnOffset;





            if (

                pos >=

                rocketEnd

            )

            {

                continue;

            }





            while (

                pos <

                rocketEnd

            )

            {

                const std::uint8_t

                    topDelta =

                        wad[

                            pos++

                        ];





                if (

                    topDelta ==

                    0xFFu

                )

                {

                    break;

                }





                if (

                    pos +

                    2u >

                    rocketEnd

                )

                {

                    break;

                }





                const std::uint8_t

                    length =

                        wad[

                            pos++

                        ];





                /*

                    Doom patch post dummy byte.

                */



                ++pos;





                if (

                    pos +

                    length +

                    1u >

                    rocketEnd

                )

                {

                    break;

                }





                for (

                    std::uint32_t y = 0;

                    y <

                        length;

                    ++y

                )

                {

                    const std::uint32_t

                        destinationY =

                            static_cast<

                                std::uint32_t

                            >(

                                topDelta

                            ) +

                            y;





                    if (

                        destinationY >=

                        height

                    )

                    {

                        continue;

                    }





                    const std::uint8_t

                        paletteIndex =

                            wad[

                                pos +

                                y

                            ];





                    const std::size_t

                        destination =

                            (

                                static_cast<

                                    std::size_t

                                >(

                                    destinationY

                                ) *

                                width +

                                x

                            ) *

                            4u;





                    rgba[

                        destination +

                        0u

                    ] =

                        palette[

                            paletteIndex *

                            3u +

                            0u

                        ];





                    rgba[

                        destination +

                        1u

                    ] =

                        palette[

                            paletteIndex *

                            3u +

                            1u

                        ];





                    rgba[

                        destination +

                        2u

                    ] =

                        palette[

                            paletteIndex *

                            3u +

                            2u

                        ];





                    rgba[

                        destination +

                        3u

                    ] =

                        255u;

                }





                pos +=

                    length;





                /*

                    Doom patch trailing dummy byte.

                */



                ++pos;

            }

        }





        D3D11_TEXTURE2D_DESC

            textureDesc{};





        textureDesc.Width =

            width;





        textureDesc.Height =

            height;





        textureDesc.MipLevels =

            1;





        textureDesc.ArraySize =

            1;





        textureDesc.Format =

            DXGI_FORMAT_R8G8B8A8_UNORM;





        textureDesc.SampleDesc.Count =

            1;





        textureDesc.Usage =

            D3D11_USAGE_IMMUTABLE;





        textureDesc.BindFlags =

            D3D11_BIND_SHADER_RESOURCE;





        D3D11_SUBRESOURCE_DATA

            initialData{};





        initialData.pSysMem =

            rgba.data();





        initialData.SysMemPitch =

            width *

            4u;





        ComPtr<

            ID3D11Texture2D>

            texture;





        if (

            FAILED(

                g_device->

                    CreateTexture2D(

                        &textureDesc,

                        &initialData,

                        &texture

                    )

            )

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: CreateTexture2D(MISLA0) failed"

            );



            return false;

        }





        ComPtr<

            ID3D11ShaderResourceView>

            srv;





        if (

            FAILED(

                g_device->

                    CreateShaderResourceView(

                        texture.Get(),

                        nullptr,

                        &srv

                    )

            )

        )

        {

            SKSE::log::error(

                "[skydoomskse] Visible rocket: CreateShaderResourceView(MISLA0) failed"

            );



            return false;

        }





        g_skyDoomRocketSpriteTexture =

            texture;





        g_skyDoomRocketSpriteSRV =

            srv;





        g_skyDoomRocketSpriteWidth =

            width;





        g_skyDoomRocketSpriteHeight =

            height;





                SKSE::log::info(
            "[skydoomskse] Visible REAL DOOM rocket sprite loaded from DOOM.WAD: {} {}x{}",
            selectedRocketLumpName ?
                selectedRocketLumpName :
                "UNKNOWN",
            width,
            height
        );





        return true;

    }





    // ========================================================

    // SKYDOOM_PLASMA_IMPACT_DAMAGE_V13

    //

    // Genuine DOOM plasma death animation:

    //   PLSE A/B/C/D/E, 4 tics per frame.

    //   20 tics total at 35 Hz ~= 571 ms.

    // ========================================================



    constexpr std::uint32_t

        SKYDOOM_MAX_PLASMA_IMPACTS_V13 =

            64u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_IMPACT_A_END_MS_V13 =

            114u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_IMPACT_B_END_MS_V13 =

            229u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_IMPACT_C_END_MS_V13 =

            343u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_IMPACT_D_END_MS_V13 =

            457u;



    constexpr std::uint64_t

        SKYDOOM_PLASMA_IMPACT_END_MS_V13 =

            572u;



    struct SkyDoomPlasmaImpactVisualV13

    {

        bool active =

            false;



        RE::NiPoint3 position{};



        float doomToSkyrimScale =

            1.0f;



        std::uint64_t startMs =

            0;

    };



    std::mutex

        g_skyDoomPlasmaImpactMutexV13;



    std::array<

        SkyDoomPlasmaImpactVisualV13,

        SKYDOOM_MAX_PLASMA_IMPACTS_V13>

        g_skyDoomPlasmaImpactsV13{};



    void StartSkyDoomPlasmaImpactVisualV13(

        const RE::NiPoint3& position,

        float doomToSkyrimScale

    )

    {

        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        std::lock_guard<std::mutex>

            lock(

                g_skyDoomPlasmaImpactMutexV13

            );





        SkyDoomPlasmaImpactVisualV13*

            selected =

                nullptr;





        for (

            auto& explosion :

                g_skyDoomPlasmaImpactsV13

        )

        {

            if (

                !explosion.active ||

                now -

                    explosion.startMs >=

                    SKYDOOM_PLASMA_IMPACT_END_MS_V13

            )

            {

                selected =

                    &explosion;



                break;

            }

        }





        if (!selected)

        {

            /*

                Extremely unlikely with only one launcher, but

                overwrite the oldest slot rather than dropping

                the impact visual completely.

            */



            selected =

                &g_skyDoomPlasmaImpactsV13[0];





            for (

                auto& explosion :

                    g_skyDoomPlasmaImpactsV13

            )

            {

                if (

                    explosion.startMs <

                    selected->startMs

                )

                {

                    selected =

                        &explosion;

                }

            }

        }





        selected->active =

            true;





        selected->position =

            position;





        selected->doomToSkyrimScale =

            doomToSkyrimScale;





        selected->startMs =

            now;





        SKSE::log::info(

            "[skydoomskse] REAL DOOM plasma impact visual started"

        );

    }

    void DrawSkyDoomPlasmaImpactsV13()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomPlasmaImpactVisualV13,

            SKYDOOM_MAX_PLASMA_IMPACTS_V13>

            explosions{};





        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomPlasmaImpactMutexV13

                );





            explosions =

                g_skyDoomPlasmaImpactsV13;

        }





        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        bool haveExplosion =

            false;





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                explosion.active &&

                now >=

                    explosion.startMs &&

                now -

                    explosion.startMs <

                    SKYDOOM_PLASMA_IMPACT_END_MS_V13

            )

            {

                haveExplosion =

                    true;



                break;

            }

        }





        if (!haveExplosion)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                !explosion.active ||

                now <

                    explosion.startMs

            )

            {

                continue;

            }





            const std::uint64_t elapsed =

                now -

                explosion.startMs;





            if (

                elapsed >=

                SKYDOOM_PLASMA_IMPACT_END_MS_V13

            )

            {

                continue;

            }





            std::uint32_t frameIndex =

                    5u;



                if (

                    elapsed >=

                    SKYDOOM_PLASMA_IMPACT_D_END_MS_V13

                )

                {

                    frameIndex =

                        9u;

                }

                else if (

                    elapsed >=

                    SKYDOOM_PLASMA_IMPACT_C_END_MS_V13

                )

                {

                    frameIndex =

                        8u;

                }

                else if (

                    elapsed >=

                    SKYDOOM_PLASMA_IMPACT_B_END_MS_V13

                )

                {

                    frameIndex =

                        7u;

                }

                else if (

                    elapsed >=

                    SKYDOOM_PLASMA_IMPACT_A_END_MS_V13

                )

                {

                    frameIndex =

                        6u;

                }





float screenX =

                0.0f;





            float screenY =

                0.0f;





            float screenZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        explosion.position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }





            const float halfWorldHeight =

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                ) *

                explosion.

                    doomToSkyrimScale *

                0.5f;





            const RE::NiPoint3 topWorld =

                explosion.position +

                cameraUp *

                    halfWorldHeight;





            float topX =

                0.0f;





            float topY =

                0.0f;





            float topZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );





            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }





            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }





            const float aspect =

                static_cast<float>(

                    g_skyDoomRocketExplosionWidths[

                        frameIndex

                    ]

                ) /

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                );





            const float fullWidthPixels =

                fullHeightPixels *

                aspect;





            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;





            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;





            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;





            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;





            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };





            D3D11_MAPPED_SUBRESOURCE mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();





            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }



    // ========================================================

    // SKYDOOM_VISIBLE_BFG_V14_2A

    //

    // Visual-only first BFG projectile pass.

    //

    // Chocolate Doom remains authoritative for:

    //   - weapon ownership

    //   - cell ammo consumption

    //   - firing cadence

    //   - genuine A_FireBFG events

    //

    // This renderer uses the real DOOM BFS1A0 / BFS1B0 sprites.

    //

    // MT_BFG native speed:

    //   25 DOOM units/tic * 35 tics/sec = 875 units/sec.

    //

    // Collision and damage are intentionally NOT added in 14.2A.

    // They will be layered on after the BFG flight path is

    // confirmed in Skyrim, keeping rocket and plasma frozen.

    // ========================================================



    constexpr std::uint32_t

        SKYDOOM_MAX_ACTIVE_BFG_VISUALS_V14_2A =

            16u;



    constexpr std::uint64_t

        SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A =

            3000u;



    constexpr std::uint64_t

        SKYDOOM_BFG_FRAME_MS_V14_2A =

            114u;



    struct SkyDoomBFGVisualV14_2A

    {

        bool active =

            false;



        RE::NiPoint3 origin{};



        RE::NiPoint3 direction{};



        float doomToSkyrimScale =

            1.0f;

        // SKYDOOM_BFG_COLLISION_IMPACT_V14_2B
        float stopDistance =
            0.0f;

        std::uint32_t collisionTargetFormID =
            0u;

        bool collisionActorHit =
            false;

        bool collisionWorldHit =
            false;

        std::uint64_t startMs =

            0;

    };



    std::mutex

        g_skyDoomBFGVisualMutexV14_2A;



    std::array<

        SkyDoomBFGVisualV14_2A,

        SKYDOOM_MAX_ACTIVE_BFG_VISUALS_V14_2A>

        g_skyDoomBFGVisualsV14_2A{};



    // ========================================================
    // SKYDOOM_BFG_COLLISION_IMPACT_V14_2B
    //
    // Genuine MT_BFG deathstate:
    //   BFE1 A/B/C/D/E/F
    //   8 Doom tics per frame
    //   48 tics total ~= 1.371 seconds.
    //
    // A_BFGSpray belongs to entry into the THIRD BFE1 frame.
    // Stage 2B intentionally does NOT trigger that yet.
    // ========================================================

    constexpr std::uint32_t
        SKYDOOM_MAX_BFG_IMPACTS_V14_2B =
            16u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_A_END_MS_V14_2B =
            229u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_B_END_MS_V14_2B =
            457u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_C_END_MS_V14_2B =
            686u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_D_END_MS_V14_2B =
            914u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_E_END_MS_V14_2B =
            1143u;

    constexpr std::uint64_t
        SKYDOOM_BFG_IMPACT_END_MS_V14_2B =
            1372u;

    struct SkyDoomBFGImpactVisualV14_2B
    {
        bool active =
            false;

        RE::NiPoint3 position{};

        float doomToSkyrimScale =
            1.0f;

        RE::NiPoint3 attackDirection{};

        bool sprayTriggered =
            false;

        std::uint64_t startMs =
            0;
    };

    std::mutex
        g_skyDoomBFGImpactMutexV14_2B;

    std::array<
        SkyDoomBFGImpactVisualV14_2B,
        SKYDOOM_MAX_BFG_IMPACTS_V14_2B>
        g_skyDoomBFGImpactsV14_2B{};

    // ========================================================
    // SKYDOOM_BFG_DAMAGE_SPRAY_V14_3
    //
    // Genuine MT_EXTRABFG visual:
    //   BFE2 A/B/C/D
    //   8 Doom tics per frame
    //   32 tics total ~= 914 ms.
    // ========================================================

    constexpr std::uint32_t
        SKYDOOM_MAX_BFG_EXTRAS_V14_3 =
            128u;

    constexpr std::uint64_t
        SKYDOOM_BFG_EXTRA_A_END_MS_V14_3 =
            229u;

    constexpr std::uint64_t
        SKYDOOM_BFG_EXTRA_B_END_MS_V14_3 =
            457u;

    constexpr std::uint64_t
        SKYDOOM_BFG_EXTRA_C_END_MS_V14_3 =
            686u;

    constexpr std::uint64_t
        SKYDOOM_BFG_EXTRA_END_MS_V14_3 =
            915u;

    struct SkyDoomBFGExtraVisualV14_3
    {
        bool active =
            false;

        RE::NiPoint3 position{};

        float doomToSkyrimScale =
            1.0f;

        std::uint64_t startMs =
            0;
    };

    std::mutex
        g_skyDoomBFGExtraMutexV14_3;

    std::array<
        SkyDoomBFGExtraVisualV14_3,
        SKYDOOM_MAX_BFG_EXTRAS_V14_3>
        g_skyDoomBFGExtrasV14_3{};

    void StartSkyDoomBFGExtraVisualV14_3(

        const RE::NiPoint3& position,

        float doomToSkyrimScale

    )

    {

        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        std::lock_guard<std::mutex>

            lock(

                g_skyDoomBFGExtraMutexV14_3

            );





        SkyDoomBFGExtraVisualV14_3*

            selected =

                nullptr;





        for (

            auto& explosion :

                g_skyDoomBFGExtrasV14_3

        )

        {

            if (

                !explosion.active ||

                now -

                    explosion.startMs >=

                    SKYDOOM_BFG_EXTRA_END_MS_V14_3

            )

            {

                selected =

                    &explosion;



                break;

            }

        }





        if (!selected)

        {

            /*

                Extremely unlikely with only one launcher, but

                overwrite the oldest slot rather than dropping

                the impact visual completely.

            */



            selected =

                &g_skyDoomBFGExtrasV14_3[0];





            for (

                auto& explosion :

                    g_skyDoomBFGExtrasV14_3

            )

            {

                if (

                    explosion.startMs <

                    selected->startMs

                )

                {

                    selected =

                        &explosion;

                }

            }

        }





        selected->active =

            true;





        selected->position =

            position;





        selected->doomToSkyrimScale =

            doomToSkyrimScale;





        selected->startMs =

            now;





        SKSE::log::info(

            "[skydoomskse] REAL DOOM BFG spray target visual started"

        );

    }



    void DrawSkyDoomBFGExtrasV14_3()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomBFGExtraVisualV14_3,

            SKYDOOM_MAX_BFG_EXTRAS_V14_3>

            explosions{};





        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomBFGExtraMutexV14_3

                );





            explosions =

                g_skyDoomBFGExtrasV14_3;

        }





        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        bool haveExplosion =

            false;





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                explosion.active &&

                now >=

                    explosion.startMs &&

                now -

                    explosion.startMs <

                    SKYDOOM_BFG_EXTRA_END_MS_V14_3

            )

            {

                haveExplosion =

                    true;



                break;

            }

        }





        if (!haveExplosion)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                !explosion.active ||

                now <

                    explosion.startMs

            )

            {

                continue;

            }





            const std::uint64_t elapsed =

                now -

                explosion.startMs;





            if (

                elapsed >=

                SKYDOOM_BFG_EXTRA_END_MS_V14_3

            )

            {

                continue;

            }





            std::uint32_t frameIndex =
                    18u;

                if (
                    elapsed >=
                    SKYDOOM_BFG_EXTRA_C_END_MS_V14_3
                )
                {
                    frameIndex =
                        21u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_EXTRA_B_END_MS_V14_3
                )
                {
                    frameIndex =
                        20u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_EXTRA_A_END_MS_V14_3
                )
                {
                    frameIndex =
                        19u;
                }


float screenX =

                0.0f;





            float screenY =

                0.0f;





            float screenZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        explosion.position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }





            const float halfWorldHeight =

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                ) *

                explosion.

                    doomToSkyrimScale *

                0.5f;





            const RE::NiPoint3 topWorld =

                explosion.position +

                cameraUp *

                    halfWorldHeight;





            float topX =

                0.0f;





            float topY =

                0.0f;





            float topZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );





            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }





            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }





            const float aspect =

                static_cast<float>(

                    g_skyDoomRocketExplosionWidths[

                        frameIndex

                    ]

                ) /

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                );





            const float fullWidthPixels =

                fullHeightPixels *

                aspect;





            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;





            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;





            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;





            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;





            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };





            D3D11_MAPPED_SUBRESOURCE mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();





            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }

    void StartSkyDoomBFGImpactVisualV14_2B(

        const RE::NiPoint3& position,

        float doomToSkyrimScale,
        const RE::NiPoint3& attackDirection
    )

    {

        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        std::lock_guard<std::mutex>

            lock(

                g_skyDoomBFGImpactMutexV14_2B

            );





        SkyDoomBFGImpactVisualV14_2B*

            selected =

                nullptr;





        for (

            auto& explosion :

                g_skyDoomBFGImpactsV14_2B

        )

        {

            if (

                !explosion.active ||

                now -

                    explosion.startMs >=

                    SKYDOOM_BFG_IMPACT_END_MS_V14_2B

            )

            {

                selected =

                    &explosion;



                break;

            }

        }





        if (!selected)

        {

            /*

                Extremely unlikely with only one launcher, but

                overwrite the oldest slot rather than dropping

                the impact visual completely.

            */



            selected =

                &g_skyDoomBFGImpactsV14_2B[0];





            for (

                auto& explosion :

                    g_skyDoomBFGImpactsV14_2B

            )

            {

                if (

                    explosion.startMs <

                    selected->startMs

                )

                {

                    selected =

                        &explosion;

                }

            }

        }





        selected->active =

            true;





        selected->position =

            position;





        selected->doomToSkyrimScale =

            doomToSkyrimScale;
        selected->attackDirection =
            attackDirection;

        selected->sprayTriggered =
            false;





        selected->startMs =

            now;





        SKSE::log::info(

            "[skydoomskse] REAL DOOM BFG impact visual started"

        );

    }

    void DrawSkyDoomBFGImpactsV14_2B()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomBFGImpactVisualV14_2B,

            SKYDOOM_MAX_BFG_IMPACTS_V14_2B>

            explosions{};





        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomBFGImpactMutexV14_2B

                );





            explosions =

                g_skyDoomBFGImpactsV14_2B;

        }





        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );





        bool haveExplosion =

            false;





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                explosion.active &&

                now >=

                    explosion.startMs &&

                now -

                    explosion.startMs <

                    SKYDOOM_BFG_IMPACT_END_MS_V14_2B

            )

            {

                haveExplosion =

                    true;



                break;

            }

        }





        if (!haveExplosion)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto& explosion :

                explosions

        )

        {

            if (

                !explosion.active ||

                now <

                    explosion.startMs

            )

            {

                continue;

            }





            const std::uint64_t elapsed =

                now -

                explosion.startMs;





            if (

                elapsed >=

                SKYDOOM_BFG_IMPACT_END_MS_V14_2B

            )

            {

                continue;

            }





            std::uint32_t frameIndex =
                    12u;

                if (
                    elapsed >=
                    SKYDOOM_BFG_IMPACT_E_END_MS_V14_2B
                )
                {
                    frameIndex =
                        17u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_IMPACT_D_END_MS_V14_2B
                )
                {
                    frameIndex =
                        16u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_IMPACT_C_END_MS_V14_2B
                )
                {
                    frameIndex =
                        15u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_IMPACT_B_END_MS_V14_2B
                )
                {
                    frameIndex =
                        14u;
                }
                else if (
                    elapsed >=
                    SKYDOOM_BFG_IMPACT_A_END_MS_V14_2B
                )
                {
                    frameIndex =
                        13u;
                }


float screenX =

                0.0f;





            float screenY =

                0.0f;





            float screenZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        explosion.position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }





            const float halfWorldHeight =

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                ) *

                explosion.

                    doomToSkyrimScale *

                0.5f;





            const RE::NiPoint3 topWorld =

                explosion.position +

                cameraUp *

                    halfWorldHeight;





            float topX =

                0.0f;





            float topY =

                0.0f;





            float topZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );





            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }





            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }





            const float aspect =

                static_cast<float>(

                    g_skyDoomRocketExplosionWidths[

                        frameIndex

                    ]

                ) /

                static_cast<float>(

                    g_skyDoomRocketExplosionHeights[

                        frameIndex

                    ]

                );





            const float fullWidthPixels =

                fullHeightPixels *

                aspect;





            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );





            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );





            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;





            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;





            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;





            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;





            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };





            D3D11_MAPPED_SUBRESOURCE mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();





            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }

    void DrawSkyDoomBFGSpritesV14_2A()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }



        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }



        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }



        auto* worldCamera =

            RE::Main::

                WorldRootCamera();



        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();



        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }



        std::array<

            SkyDoomBFGVisualV14_2A,

            SKYDOOM_MAX_ACTIVE_BFG_VISUALS_V14_2A>

            visuals{};



        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomBFGVisualMutexV14_2A

                );



            visuals =

                g_skyDoomBFGVisualsV14_2A;

        }



        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        bool haveVisibleBFG =

            false;



        for (

            const auto& bfg :

                visuals

        )

        {

            if (

                bfg.active &&

                now >=

                    bfg.startMs &&

                now -

                    bfg.startMs <

                    SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A

            )

            {

                haveVisibleBFG =

                    true;



                break;

            }

        }



        if (!haveVisibleBFG)

        {

            return;

        }



        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();



        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );



        D3D11_VIEWPORT

            viewport{};



        viewport.TopLeftX =

            0.0f;



        viewport.TopLeftY =

            0.0f;



        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );



        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );



        viewport.MinDepth =

            0.0f;



        viewport.MaxDepth =

            1.0f;



        g_context->

            RSSetViewports(

                1,

                &viewport

            );



        const UINT stride =

            sizeof(

                OverlayVertex

            );



        const UINT offset =

            0;



        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();



        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );



        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );



        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );



        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );



        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );



        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();



        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );



        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };



        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );



        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );



        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );



        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };



        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }



        for (

            const auto& bfg :

                visuals

        )

        {

            if (

                !bfg.active ||

                now <

                    bfg.startMs

            )

            {

                continue;

            }



            const std::uint64_t elapsed =

                now -

                bfg.startMs;



            if (

                elapsed >=

                SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A

            )

            {

                continue;

            }



            const std::uint32_t

                frameIndex =

                    10u +

                    static_cast<std::uint32_t>(

                        (

                            elapsed /

                            SKYDOOM_BFG_FRAME_MS_V14_2A

                        ) &

                        1u

                    );



            const float elapsedSeconds =

                static_cast<float>(

                    elapsed

                ) /

                1000.0f;



            const float speed =
                875.0f *
                bfg.
                    doomToSkyrimScale;

            const float travelDistance =
                speed *
                elapsedSeconds;

            if (
                (
                    bfg.collisionActorHit ||
                    bfg.collisionWorldHit
                ) &&
                travelDistance >=
                    bfg.stopDistance
            )
            {
                continue;
            }

            const RE::NiPoint3 position =
                bfg.origin +
                bfg.direction *
                    travelDistance;



            float screenX =

                0.0f;



            float screenY =

                0.0f;



            float screenZ =

                0.0f;



            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        position,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }



            if (

                screenX <

                    -0.35f ||

                screenX >

                    1.35f ||

                screenY <

                    -0.35f ||

                screenY >

                    1.35f

            )

            {

                continue;

            }



            const auto spriteHeight =

                g_skyDoomRocketExplosionHeights[

                    frameIndex

                ];



            const auto spriteWidth =

                g_skyDoomRocketExplosionWidths[

                    frameIndex

                ];



            if (

                spriteWidth ==

                    0u ||

                spriteHeight ==

                    0u

            )

            {

                continue;

            }



            const float halfWorldHeight =

                static_cast<float>(

                    spriteHeight

                ) *

                bfg.

                    doomToSkyrimScale *

                0.5f;



            const RE::NiPoint3 topWorld =

                position +

                cameraUp *

                    halfWorldHeight;



            float topX =

                0.0f;



            float topY =

                0.0f;



            float topZ =

                0.0f;



            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }



            const float deltaX =

                (

                    topX -

                    screenX

                ) *

                static_cast<float>(

                    g_backBufferWidth

                );



            const float deltaY =

                (

                    topY -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );



            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );



            if (

                fullHeightPixels <

                8.0f

            )

            {

                fullHeightPixels =

                    8.0f;

            }



            if (

                fullHeightPixels >

                320.0f

            )

            {

                fullHeightPixels =

                    320.0f;

            }



            const float aspect =

                static_cast<float>(

                    spriteWidth

                ) /

                static_cast<float>(

                    spriteHeight

                );



            const float fullWidthPixels =

                fullHeightPixels *

                aspect;



            const float centrePixelX =

                screenX *

                static_cast<float>(

                    g_backBufferWidth

                );



            const float centrePixelY =

                (

                    1.0f -

                    screenY

                ) *

                static_cast<float>(

                    g_backBufferHeight

                );



            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;



            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;



            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;



            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;



            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;



            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;



            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;



            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;



            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };



            D3D11_MAPPED_SUBRESOURCE

                mapped{};



            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }



            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );



            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );



            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();



            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );



            g_context->

                Draw(

                    6,

                    0

                );

        }

    }



    void DrawSkyDoomRocketSpritesV11_1()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketSpriteTexture()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomRocketRenderSnapshot,

            SKYDOOM_MAX_ACTIVE_ROCKETS>

            snapshots{};





        {

            std::lock_guard<

                std::mutex>

                lock(

                    g_skyDoomRocketRenderMutex

                );





            snapshots =

                g_skyDoomRocketRenderSnapshots;

        }





        bool haveVisibleRocket =

            false;





        for (

            const auto&

                snapshot :

                    snapshots

        )

        {

            if (snapshot.active)

            {

                haveVisibleRocket =

                    true;



                break;

            }

        }





        if (!haveVisibleRocket)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT

            viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11ShaderResourceView*

            spriteSRV =

                g_skyDoomRocketSpriteSRV.

                    Get();





        g_context->

            PSSetShaderResources(

                0,

                1,

                &spriteSRV

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float

            blendFactor[

                4

            ]{

                0.0f,

                0.0f,

                0.0f,

                0.0f

            };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        const auto now =

            static_cast<

                std::uint64_t

            >(

                GetTickCount64()

            );





        RE::NiPoint3

            cameraUp =

                playerCamera->

                    cameraRoot->

                    world.

                    rotate *

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto&

                snapshot :

                    snapshots

        )

        {

            if (!snapshot.active)

            {

                continue;

            }





            float extrapolateSeconds =

                0.0f;





            if (

                now >

                snapshot.sampleMs

            )

            {

                extrapolateSeconds =

                    static_cast<float>(

                        now -

                        snapshot.sampleMs

                    ) /

                    1000.0f;

            }





            /*

                Main-thread projectile updates are about 50 ms.



                Extrapolate for rendering between updates so the

                sprite appears smooth, but never run too far ahead

                of the authoritative collision projectile.

            */



            if (

                extrapolateSeconds >

                0.075f

            )

            {

                extrapolateSeconds =

                    0.075f;

            }





            const float

                renderSpeed =

                    700.0f *

                    snapshot.

                        doomToSkyrimScale;





            RE::NiPoint3

                renderPosition =

                    snapshot.

                        position +

                    snapshot.

                        direction *

                    (

                        renderSpeed *

                        extrapolateSeconds

                    );





            float screenX =

                0.0f;





            float screenY =

                0.0f;





            float screenZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        renderPosition,

                        screenX,

                        screenY,

                        screenZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            if (

                screenX <

                    -0.25f ||

                screenX >

                    1.25f ||

                screenY <

                    -0.25f ||

                screenY >

                    1.25f

            )

            {

                continue;

            }





            const float

                halfWorldHeight =

                    static_cast<float>(

                        g_skyDoomRocketSpriteHeight

                    ) *

                    snapshot.

                        doomToSkyrimScale *

                    0.5f;





            const RE::NiPoint3

                topWorld =

                    renderPosition +

                    cameraUp *

                    halfWorldHeight;





            float topX =

                0.0f;





            float topY =

                0.0f;





            float topZ =

                0.0f;





            if (

                !worldCamera->

                    WorldPtToScreenPt3(

                        topWorld,

                        topX,

                        topY,

                        topZ,

                        0.00001f

                    )

            )

            {

                continue;

            }





            const float

                deltaX =

                    (

                        topX -

                        screenX

                    ) *

                    static_cast<float>(

                        g_backBufferWidth

                    );





            const float

                deltaY =

                    (

                        topY -

                        screenY

                    ) *

                    static_cast<float>(

                        g_backBufferHeight

                    );





            float fullHeightPixels =

                2.0f *

                std::sqrt(

                    deltaX *

                        deltaX +

                    deltaY *

                        deltaY

                );





            if (

                fullHeightPixels <

                4.0f

            )

            {

                fullHeightPixels =

                    4.0f;

            }





            if (

                fullHeightPixels >

                192.0f

            )

            {

                fullHeightPixels =

                    192.0f;

            }





            const float

                aspect =

                    static_cast<float>(

                        g_skyDoomRocketSpriteWidth

                    ) /

                    static_cast<float>(

                        g_skyDoomRocketSpriteHeight

                    );





            const float

                fullWidthPixels =

                    fullHeightPixels *

                    aspect;





            const float

                centrePixelX =

                    screenX *

                    static_cast<float>(

                        g_backBufferWidth

                    );





            /*

                NiCamera projection Y is bottom-up.

                D3D screen pixels are top-down.

            */



            const float

                centrePixelY =

                    (

                        1.0f -

                        screenY

                    ) *

                    static_cast<float>(

                        g_backBufferHeight

                    );





            const float leftPixel =

                centrePixelX -

                fullWidthPixels *

                0.5f;





            const float rightPixel =

                centrePixelX +

                fullWidthPixels *

                0.5f;





            const float topPixel =

                centrePixelY -

                fullHeightPixels *

                0.5f;





            const float bottomPixel =

                centrePixelY +

                fullHeightPixels *

                0.5f;





            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex

                vertices[

                    6

                ]{

                    {

                        left,

                        top,

                        0.0f,

                        0.0f,

                        0.0f

                    },

                    {

                        right,

                        bottom,

                        0.0f,

                        1.0f,

                        1.0f

                    },

                    {

                        left,

                        bottom,

                        0.0f,

                        0.0f,

                        1.0f

                    },



                    {

                        left,

                        top,

                        0.0f,

                        0.0f,

                        0.0f

                    },

                    {

                        right,

                        top,

                        0.0f,

                        1.0f,

                        0.0f

                    },

                    {

                        right,

                        bottom,

                        0.0f,

                        1.0f,

                        1.0f

                    }

                };





            D3D11_MAPPED_SUBRESOURCE

                mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }









    float GetSkyDoomDoomToSkyrimScale(

        RE::PlayerCharacter* player

    )

    {

        if (!player)

        {

            return 1.0f;

        }





        float playerHeight =

            player->

                GetHeight();





        if (playerHeight < 1.0f)

        {

            playerHeight =

                128.0f;

        }





        float scale =

            playerHeight /

            56.0f;





        if (scale < 1.0f)

        {

            scale =

                1.0f;

        }





        if (scale > 4.0f)

        {

            scale =

                4.0f;

        }





        return scale;

    }





    void ClearSkyDoomRocketState()

    {

        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_ACTIVE_ROCKETS;

            ++i

        )

        {

            g_skyDoomRockets[i].

                active =

                    false;

        }





        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_PENDING_ROCKET_IMPACTS;

            ++i

        )

        {

            g_skyDoomPendingRocketImpacts[i].

                active =

                    false;

        }

    }





    RE::Actor* FindSkyDoomActorByFormId(

        std::uint32_t formId

    )

    {

        if (formId == 0)

        {

            return nullptr;

        }





        auto* processLists =

            RE::ProcessLists::

                GetSingleton();





        if (!processLists)

        {

            return nullptr;

        }





        for (

            auto& handle :

                processLists->

                    highActorHandles

        )

        {

            auto actorPtr =

                handle.

                    get();





            auto* actor =

                actorPtr.

                    get();





            if (

                actor &&

                actor->

                    GetFormID() ==

                    formId

            )

            {

                return actor;

            }

        }





        return nullptr;

    }





    RE::Actor* FindSkyDoomActorByReference(

        RE::TESObjectREFR* reference

    )

    {

        if (!reference)

        {

            return nullptr;

        }





        auto* processLists =

            RE::ProcessLists::

                GetSingleton();





        if (!processLists)

        {

            return nullptr;

        }





        for (

            auto& handle :

                processLists->

                    highActorHandles

        )

        {

            auto actorPtr =

                handle.

                    get();





            auto* actor =

                actorPtr.

                    get();





            if (

                actor &&

                actor ==

                    reference

            )

            {

                return actor;

            }

        }





        return nullptr;

    }





    bool SkyDoomWorldPick(

        const RE::NiPoint3& fromWorld,

        const RE::NiPoint3& toWorld,

        RE::TESObjectREFR*& blockerRef,

        float& hitFraction

    )

    {

        blockerRef =

            nullptr;





        hitFraction =

            1.0f;





        auto* player =

            RE::PlayerCharacter::

                GetSingleton();





        if (!player)

        {

            return false;

        }





        auto* controller =

            player->

                GetCharController();





        if (!controller)

        {

            return false;

        }





        auto* world =

            controller->

                GetHavokWorld();





        if (!world)

        {

            return false;

        }





        const RE::NiPoint3

            playerWorld =

                player->

                    GetPosition();





        RE::hkVector4

            playerHavok{};





        controller->

            GetPosition(

                playerHavok,

                false

            );





        const RE::NiPoint3

            fromDelta{

                fromWorld.x -

                    playerWorld.x,



                fromWorld.y -

                    playerWorld.y,



                fromWorld.z -

                    playerWorld.z

            };





        const RE::NiPoint3

            toDelta{

                toWorld.x -

                    playerWorld.x,



                toWorld.y -

                    playerWorld.y,



                toWorld.z -

                    playerWorld.z

            };





        const float havokScale =

            RE::bhkWorld::

                GetWorldScale();





        const RE::hkVector4

            havokScaleVector{

                havokScale

            };





        const RE::hkVector4

            fromHavok =

                playerHavok +

                (

                    RE::hkVector4(

                        fromDelta

                    ) *

                    havokScaleVector

                );





        const RE::hkVector4

            toHavok =

                playerHavok +

                (

                    RE::hkVector4(

                        toDelta

                    ) *

                    havokScaleVector

                );





        RE::bhkPickData

            pick{};





        pick.

            rayInput.

            enableShapeCollectionFilter =

                true;





        pick.

            rayInput.

            from =

                fromHavok;





        pick.

            rayInput.

            to =

                toHavok;





        controller->

            GetCollisionFilterInfo(

                pick.

                    rayInput.

                    filterInfo

            );





        if (

            !world->

                PickObject(

                    pick

                ) ||

            !pick.

                rayOutput.

                rootCollidable

        )

        {

            return false;

        }





        hitFraction =

            pick.

                rayOutput.

                hitFraction;





        blockerRef =

            RE::TESHavokUtilities::

                FindCollidableRef(

                    *pick.

                        rayOutput.

                        rootCollidable

                );





        return true;

    }





    bool SkyDoomSegmentHitsActor(

        const RE::NiPoint3& origin,

        const RE::NiPoint3& direction,

        float segmentLength,

        RE::Actor* actor,

        float rocketRadius,

        float rocketHalfHeight,

        float& hitDistance

    )

    {

        if (

            !actor ||

            segmentLength <=

                0.0f

        )

        {

            return false;

        }





        const RE::NiPoint3

            actorPosition =

                actor->

                    GetPosition();





        float actorHeight =

            actor->

                GetHeight();





        if (actorHeight < 21.0f)

        {

            actorHeight =

                21.0f;

        }





        if (actorHeight > 840.0f)

        {

            actorHeight =

                840.0f;

        }





        float actorWidth =

            actor->

                GetBoundRadius() *

            2.0f;





        if (actorWidth < 21.0f)

        {

            actorWidth =

                21.0f;

        }





        if (actorWidth > 420.0f)

        {

            actorWidth =

                420.0f;

        }





        const float

            actorRadius =

                actorWidth *

                    0.5f +

                4.0f +

                rocketRadius;





        const float minX =

            actorPosition.x -

            actorRadius;





        const float maxX =

            actorPosition.x +

            actorRadius;





        const float minY =

            actorPosition.y -

            actorRadius;





        const float maxY =

            actorPosition.y +

            actorRadius;





        const float minZ =

            actorPosition.z -

            4.0f -

            rocketHalfHeight;





        const float maxZ =

            actorPosition.z +

            actorHeight +

            rocketHalfHeight;





        float tMin =

            0.0f;





        float tMax =

            segmentLength;





        auto intersectSlab =

            [&tMin, &tMax](

                float slabOrigin,

                float slabDirection,

                float slabMin,

                float slabMax

            ) -> bool

            {

                if (

                    slabDirection >

                        -0.000001f &&

                    slabDirection <

                        0.000001f

                )

                {

                    return

                        slabOrigin >=

                            slabMin &&

                        slabOrigin <=

                            slabMax;

                }





                const float inverse =

                    1.0f /

                    slabDirection;





                float t1 =

                    (

                        slabMin -

                        slabOrigin

                    ) *

                    inverse;





                float t2 =

                    (

                        slabMax -

                        slabOrigin

                    ) *

                    inverse;





                if (t1 > t2)

                {

                    const float swap =

                        t1;



                    t1 =

                        t2;



                    t2 =

                        swap;

                }





                if (t1 > tMin)

                {

                    tMin =

                        t1;

                }





                if (t2 < tMax)

                {

                    tMax =

                        t2;

                }





                return

                    tMax >=

                    tMin;

            };





        if (

            !intersectSlab(

                origin.x,

                direction.x,

                minX,

                maxX

            ) ||

            !intersectSlab(

                origin.y,

                direction.y,

                minY,

                maxY

            ) ||

            !intersectSlab(

                origin.z,

                direction.z,

                minZ,

                maxZ

            )

        )

        {

            return false;

        }





        if (

            tMax <

                0.0f

        )

        {

            return false;

        }





        hitDistance =

            tMin;





        if (hitDistance < 0.0f)

        {

            hitDistance =

                0.0f;

        }





        return

            hitDistance <=

            segmentLength;

    }





    bool SkyDoomRocketExplosionHasSight(

        const RE::NiPoint3& explosion,

        const RE::NiPoint3& targetPoint,

        RE::Actor* target

    )

    {

        RE::TESObjectREFR*

            blockerRef =

                nullptr;





        float hitFraction =

            1.0f;





        if (

            !SkyDoomWorldPick(

                explosion,

                targetPoint,

                blockerRef,

                hitFraction

            )

        )

        {

            return true;

        }





        if (

            blockerRef ==

            target

        )

        {

            return true;

        }





        /*

            DOOM's P_CheckSight is blocked by map geometry,

            not by another monster standing between the blast

            and the target.



            If Havok's first hit is another Actor, treat that

            as transparent for the DOOM-style sight test.

        */



        if (

            FindSkyDoomActorByReference(

                blockerRef

            )

        )

        {

            return true;

        }





        /*

            Very-near-end collisions are normally the target's

            own physics body even when no normal reference maps.

        */



        if (hitFraction >= 0.97f)

        {

            return true;

        }





        return false;

    }





    void ApplySkyDoomRocketSplash(

        const RE::NiPoint3& explosion,

        float doomToSkyrimScale

    )

    {

        // SKYDOOM_EXACT_ROCKET_SPLASH_V11



        auto* player =

            RE::PlayerCharacter::

                GetSingleton();





        auto* processLists =

            RE::ProcessLists::

                GetSingleton();





        if (

            !player ||

            !processLists

        )

        {

            return;

        }





        if (doomToSkyrimScale < 0.001f)

        {

            doomToSkyrimScale =

                1.0f;

        }





        auto applyToActor =

            [

                &explosion,

                doomToSkyrimScale,

                player

            ](

                RE::Actor* actor

            )

            {

                if (

                    !actor ||

                    actor->

                        IsDisabled() ||

                    !actor->

                        Is3DLoaded() ||

                    actor->

                        IsGhost() ||

                    actor->

                        IsDead()

                )

                {

                    return;

                }





                const RE::NiPoint3

                    actorPosition =

                        actor->

                            GetPosition();





                float actorHeight =

                    actor->

                        GetHeight();





                if (actorHeight < 21.0f)

                {

                    actorHeight =

                        21.0f;

                }





                if (actorHeight > 840.0f)

                {

                    actorHeight =

                        840.0f;

                }





                const RE::NiPoint3

                    actorCentre{

                        actorPosition.x,

                        actorPosition.y,

                        actorPosition.z +

                            actorHeight *

                            0.5f

                    };





                const float dxDoom =

                    std::fabs(

                        actorCentre.x -

                        explosion.x

                    ) /

                    doomToSkyrimScale;





                const float dyDoom =

                    std::fabs(

                        actorCentre.y -

                        explosion.y

                    ) /

                    doomToSkyrimScale;





                float maxDistanceDoom =

                    dxDoom;





                if (

                    dyDoom >

                    maxDistanceDoom

                )

                {

                    maxDistanceDoom =

                        dyDoom;

                }





                float radiusDoom =

                    actor->

                        GetBoundRadius() /

                    doomToSkyrimScale;





                if (radiusDoom < 0.0f)

                {

                    radiusDoom =

                        0.0f;

                }





                float distanceAfterRadius =

                    maxDistanceDoom -

                    radiusDoom;





                if (distanceAfterRadius < 0.0f)

                {

                    distanceAfterRadius =

                        0.0f;

                }





                const int doomDistance =

                    static_cast<int>(

                        std::floor(

                            distanceAfterRadius

                        )

                    );





                if (doomDistance >= 128)

                {

                    return;

                }





                if (

                    !SkyDoomRocketExplosionHasSight(

                        explosion,

                        actorCentre,

                        actor

                    )

                )

                {

                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM rocket splash BLOCKED: target={:08X} doomDistance={}",

                        actor->

                            GetFormID(),

                        doomDistance

                    );



                    return;

                }





                const int splashDamage =

                    128 -

                    doomDistance;





                if (splashDamage <= 0)

                {

                    return;

                }





                if (actor == player)

                {

                    /*

                        Do not deliberately damage Skyrim's host HP

                        and then wait for the sensor bridge.



                        Feed self-splash straight into the existing

                        real Chocolate Doom P_DamageMobj path.



                        DOOM armor therefore still works.

                    */



                    PushInputEvent(

                        SKYDOOM_INPUT_EVENT_DAMAGE,

                        0,

                        splashDamage

                    );





                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM rocket self-splash: doomDistance={} damage={}",

                        doomDistance,

                        splashDamage

                    );



                    return;

                }





                RE::NiPoint3

                    damageDirection{

                        actorCentre.x -

                            explosion.x,



                        actorCentre.y -

                            explosion.y,



                        actorCentre.z -

                            explosion.z

                    };





                if (

                    damageDirection.

                        Unitize() <=

                    0.0f

                )

                {

                    damageDirection =

                        RE::NiPoint3{

                            0.0f,

                            1.0f,

                            0.0f

                        };

                }





                auto* actorValueOwner =

                    actor->

                        AsActorValueOwner();





                if (!actorValueOwner)

                {

                    return;

                }





                const float healthBefore =

                    actorValueOwner->

                        GetActorValue(

                            RE::ActorValue::

                                kHealth

                        );





                if (healthBefore <= 0.0f)

                {

                    return;

                }





                const bool usedNativeHitData =

                    ApplySkyDoomNativePistolHit(

                        player,

                        actor,

                        static_cast<float>(

                            splashDamage

                        ),

                        actorCentre,

                        damageDirection

                    );





                if (!usedNativeHitData)

                {

                    actor->

                        DoDamage(

                            static_cast<float>(

                                splashDamage

                            ),

                            player,

                            true

                        );

                }





                const float healthAfter =

                    actorValueOwner->

                        GetActorValue(

                            RE::ActorValue::

                                kHealth

                        );





                SKSE::log::info(

                    "[skydoomskse] REAL DOOM rocket splash HIT: target={:08X} doomDistance={} damage={} health={} -> {} path={}",

                    actor->

                        GetFormID(),

                    doomDistance,

                    splashDamage,

                    healthBefore,

                    healthAfter,

                    usedNativeHitData ?

                        "HitData" :

                        "DoDamage"

                );

            };





        /*

            Include the player explicitly.



            highActorHandles is used for every other actor.

        */



        applyToActor(

            player

        );





        for (

            auto& handle :

                processLists->

                    highActorHandles

        )

        {

            auto actorPtr =

                handle.

                    get();





            auto* actor =

                actorPtr.

                    get();





            if (

                !actor ||

                actor ==

                    player

            )

            {

                continue;

            }





            applyToActor(

                actor

            );

        }

    }





    struct SkyDoomPlasmaCollisionV12_2B
    {
        float distance =
            0.0f;

        std::uint32_t targetFormID =
            0u;

        bool actorHit =
            false;

        bool worldHit =
            false;
    };

    SkyDoomPlasmaCollisionV12_2B
    FindSkyDoomPlasmaCollisionV12_2B(
        const RE::NiPoint3& origin,
        const RE::NiPoint3& direction,
        float maxDistance,
        float projectileRadius,
        RE::PlayerCharacter* player
    )
    {
        SkyDoomPlasmaCollisionV12_2B result{};

        result.distance =
            maxDistance;

        if (
            !player ||
            maxDistance <=
                0.0f
        )
        {
            return result;
        }

        auto* processLists =
            RE::ProcessLists::
                GetSingleton();

        RE::Actor*
            nearestActor =
                nullptr;

        float nearestActorDistance =
            maxDistance +
            1.0f;

        if (processLists)
        {
            for (
                auto& handle :
                    processLists->
                        highActorHandles
            )
            {
                auto actorPtr =
                    handle.
                        get();

                auto* actor =
                    actorPtr.
                        get();

                if (
                    !actor ||
                    actor ==
                        player ||
                    actor->
                        IsDisabled() ||
                    !actor->
                        Is3DLoaded() ||
                    actor->
                        IsGhost() ||
                    actor->
                        IsDead()
                )
                {
                    continue;
                }

                const RE::NiPoint3 actorPosition =
                    actor->
                        GetPosition();

                float actorHeight =
                    actor->
                        GetHeight();

                if (actorHeight < 21.0f)
                {
                    actorHeight =
                        21.0f;
                }

                if (actorHeight > 840.0f)
                {
                    actorHeight =
                        840.0f;
                }

                float actorWidth =
                    actor->
                        GetBoundRadius() *
                    2.0f;

                if (actorWidth < 21.0f)
                {
                    actorWidth =
                        21.0f;
                }

                if (actorWidth > 420.0f)
                {
                    actorWidth =
                        420.0f;
                }

                const float actorRadius =
                    actorWidth *
                        0.5f +
                    projectileRadius;

                const float minX =
                    actorPosition.x -
                    actorRadius;

                const float maxX =
                    actorPosition.x +
                    actorRadius;

                const float minY =
                    actorPosition.y -
                    actorRadius;

                const float maxY =
                    actorPosition.y +
                    actorRadius;

                const float minZ =
                    actorPosition.z -
                    projectileRadius;

                const float maxZ =
                    actorPosition.z +
                    actorHeight +
                    projectileRadius;

                float tMin =
                    0.0f;

                float tMax =
                    maxDistance;

                auto intersectSlab =
                    [&tMin, &tMax](
                        float rayOrigin,
                        float rayDirection,
                        float slabMin,
                        float slabMax
                    ) -> bool
                    {
                        if (
                            rayDirection >
                                -0.000001f &&
                            rayDirection <
                                0.000001f
                        )
                        {
                            return
                                rayOrigin >= slabMin &&
                                rayOrigin <= slabMax;
                        }

                        const float inverseDirection =
                            1.0f /
                            rayDirection;

                        float t1 =
                            (
                                slabMin -
                                rayOrigin
                            ) *
                            inverseDirection;

                        float t2 =
                            (
                                slabMax -
                                rayOrigin
                            ) *
                            inverseDirection;

                        if (t1 > t2)
                        {
                            const float swap =
                                t1;

                            t1 =
                                t2;

                            t2 =
                                swap;
                        }

                        if (t1 > tMin)
                        {
                            tMin =
                                t1;
                        }

                        if (t2 < tMax)
                        {
                            tMax =
                                t2;
                        }

                        return
                            tMax >=
                            tMin;
                    };

                if (
                    !intersectSlab(
                        origin.x,
                        direction.x,
                        minX,
                        maxX
                    ) ||
                    !intersectSlab(
                        origin.y,
                        direction.y,
                        minY,
                        maxY
                    ) ||
                    !intersectSlab(
                        origin.z,
                        direction.z,
                        minZ,
                        maxZ
                    )
                )
                {
                    continue;
                }

                if (tMax < 0.0f)
                {
                    continue;
                }

                float hitDistance =
                    tMin;

                if (hitDistance < 0.0f)
                {
                    hitDistance =
                        0.0f;
                }

                if (
                    hitDistance >
                        maxDistance ||
                    hitDistance >=
                        nearestActorDistance
                )
                {
                    continue;
                }

                nearestActorDistance =
                    hitDistance;

                nearestActor =
                    actor;
            }
        }

        if (nearestActor)
        {
            result.distance =
                nearestActorDistance;

            result.targetFormID =
                nearestActor->
                    GetFormID();

            result.actorHit =
                true;
        }

        auto* controller =
            player->
                GetCharController();

        if (controller)
        {
            auto* world =
                controller->
                    GetHavokWorld();

            if (
                world &&
                maxDistance >
                    9.0f
            )
            {
                const float worldStartDistance =
                    8.0f;

                const RE::NiPoint3 worldStart =
                    origin +
                    direction *
                        worldStartDistance;

                const RE::NiPoint3 worldEnd =
                    origin +
                    direction *
                        maxDistance;

                const RE::NiPoint3 playerWorld =
                    player->
                        GetPosition();

                RE::hkVector4 playerHavok{};

                controller->
                    GetPosition(
                        playerHavok,
                        false
                    );

                const RE::NiPoint3 startDelta{
                    worldStart.x -
                        playerWorld.x,
                    worldStart.y -
                        playerWorld.y,
                    worldStart.z -
                        playerWorld.z
                };

                const RE::NiPoint3 endDelta{
                    worldEnd.x -
                        playerWorld.x,
                    worldEnd.y -
                        playerWorld.y,
                    worldEnd.z -
                        playerWorld.z
                };

                const float havokScale =
                    RE::bhkWorld::
                        GetWorldScale();

                const RE::hkVector4 havokScaleVector{
                    havokScale
                };

                const RE::hkVector4 startHavok =
                    playerHavok +
                    (
                        RE::hkVector4(
                            startDelta
                        ) *
                        havokScaleVector
                    );

                const RE::hkVector4 endHavok =
                    playerHavok +
                    (
                        RE::hkVector4(
                            endDelta
                        ) *
                        havokScaleVector
                    );

                RE::bhkPickData pick{};

                pick.
                    rayInput.
                    enableShapeCollectionFilter =
                        true;

                pick.
                    rayInput.
                    from =
                        startHavok;

                pick.
                    rayInput.
                    to =
                        endHavok;

                controller->
                    GetCollisionFilterInfo(
                        pick.
                            rayInput.
                            filterInfo
                    );

                if (
                    world->
                        PickObject(
                            pick
                        ) &&
                    pick.
                        rayOutput.
                        rootCollidable
                )
                {
                    auto* blockerRef =
                        RE::TESHavokUtilities::
                            FindCollidableRef(
                                *pick.
                                    rayOutput.
                                    rootCollidable
                            );

                    if (
                        blockerRef !=
                        player
                    )
                    {
                        const float worldDistance =
                            worldStartDistance +
                            pick.
                                rayOutput.
                                hitFraction *
                            (
                                maxDistance -
                                worldStartDistance
                            );

                        if (
                            worldDistance <
                            result.distance
                        )
                        {
                            result.distance =
                                worldDistance;

                            result.worldHit =
                                true;

                            if (
                                blockerRef &&
                                blockerRef ==
                                    nearestActor
                            )
                            {
                                result.actorHit =
                                    true;

                                result.targetFormID =
                                    nearestActor->
                                        GetFormID();
                            }
                            else
                            {
                                result.actorHit =
                                    false;

                                result.targetFormID =
                                    0u;
                            }
                        }
                    }
                }
            }
        }

        if (result.distance < 0.0f)
        {
            result.distance =
                0.0f;
        }

        if (result.distance > maxDistance)
        {
            result.distance =
                maxDistance;
        }

        if (result.actorHit)
        {
            SKSE::log::info(
                "[skydoomskse] REAL DOOM plasma collision armed: ACTOR target={:08X} distance={}",
                result.targetFormID,
                result.distance
            );
        }
        else if (result.worldHit)
        {
            SKSE::log::info(
                "[skydoomskse] REAL DOOM plasma collision armed: WORLD distance={}",
                result.distance
            );
        }

        return result;
    }



    void SpawnSkyDoomBFGVisualV14_2A()

    {

        auto* player =

            RE::PlayerCharacter::

                GetSingleton();



        auto* camera =

            RE::PlayerCamera::

                GetSingleton();



        if (

            !player ||

            !camera ||

            !camera->

                cameraRoot

        )

        {

            return;

        }



        RE::NiPoint3 direction =

            camera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                1.0f,

                0.0f

            };



        if (

            direction.

                Unitize() <=

            0.0f

        )

        {

            return;

        }



        const float scale =

            GetSkyDoomDoomToSkyrimScale(

                player

            );



        const float spawnAdvance =

            64.0f +

            13.0f *

                scale;
        const RE::NiPoint3 origin =
            RE::PlayerCamera::
                GetActiveCameraPosition() +
            direction *
                spawnAdvance;

        const float maxTravelDistance =
            875.0f *
            scale *
            (
                static_cast<float>(
                    SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A
                ) /
                1000.0f
            );

        /*
            MT_BFG and MT_PLASMA are both 25 units/tic,
            radius 13 and height 8 in Doom, so reuse the
            already-proven Skyrim actor + Havok collision path.
        */
        const SkyDoomPlasmaCollisionV12_2B collision =
            FindSkyDoomPlasmaCollisionV12_2B(
                origin,
                direction,
                maxTravelDistance,
                13.0f *
                    scale,
                player
            );



        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        std::lock_guard<std::mutex>

            lock(

                g_skyDoomBFGVisualMutexV14_2A

            );



        SkyDoomBFGVisualV14_2A*

            selected =

                nullptr;



        for (

            auto& bfg :

                g_skyDoomBFGVisualsV14_2A

        )

        {

            if (

                !bfg.active ||

                now <

                    bfg.startMs ||

                now -

                    bfg.startMs >=

                    SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A

            )

            {

                selected =

                    &bfg;



                break;

            }

        }



        if (!selected)

        {

            selected =

                &g_skyDoomBFGVisualsV14_2A[0];



            for (

                auto& bfg :

                    g_skyDoomBFGVisualsV14_2A

            )

            {

                if (

                    bfg.startMs <

                    selected->

                        startMs

                )

                {

                    selected =

                        &bfg;

                }

            }

        }



        selected->

            active =

                true;



        selected->
            origin =
                origin;



        selected->

            direction =

                direction;



        selected->

            doomToSkyrimScale =

                scale;
        selected->
            stopDistance =
                collision.distance;

        selected->
            collisionTargetFormID =
                collision.targetFormID;

        selected->
            collisionActorHit =
                collision.actorHit;

        selected->
            collisionWorldHit =
                collision.worldHit;



        selected->

            startMs =

                now;



        SKSE::log::info(

            "[skydoomskse] REAL DOOM BFG visual launched: scale={} speedSkyrimPerSec={}",

            scale,

            875.0f *

                scale

        );

    }



    void SpawnSkyDoomPlasmaVisualV12_2A()

    {

        auto* player =

            RE::PlayerCharacter::

                GetSingleton();



        auto* camera =

            RE::PlayerCamera::

                GetSingleton();



        if (

            !player ||

            !camera ||

            !camera->

                cameraRoot

        )

        {

            return;

        }



        RE::NiPoint3 direction =

            camera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                1.0f,

                0.0f

            };



        if (

            direction.

                Unitize() <=

            0.0f

        )

        {

            return;

        }



        const float scale =

            GetSkyDoomDoomToSkyrimScale(

                player

            );



        const float spawnAdvance =

            64.0f +

            13.0f *

                scale;
        const RE::NiPoint3 origin =
            RE::PlayerCamera::
                GetActiveCameraPosition() +
            direction *
                spawnAdvance;

        const float maxTravelDistance =
            875.0f *
            scale *
            (
                static_cast<float>(
                    SKYDOOM_PLASMA_VISUAL_LIFETIME_MS
                ) /
                1000.0f
            );

        const SkyDoomPlasmaCollisionV12_2B collision =
            FindSkyDoomPlasmaCollisionV12_2B(
                origin,
                direction,
                maxTravelDistance,
                13.0f *
                    scale,
                player
            );



        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        std::lock_guard<std::mutex>

            lock(

                g_skyDoomPlasmaVisualMutexV12_2A

            );



        SkyDoomPlasmaVisualV12_2A*

            selected =

                nullptr;



        for (

            auto& plasma :

                g_skyDoomPlasmaVisualsV12_2A

        )

        {

            if (

                !plasma.active ||

                now <

                    plasma.startMs ||

                now -

                    plasma.startMs >=

                    SKYDOOM_PLASMA_VISUAL_LIFETIME_MS

            )

            {

                selected =

                    &plasma;



                break;

            }

        }



        if (!selected)

        {

            selected =

                &g_skyDoomPlasmaVisualsV12_2A[0];



            for (

                auto& plasma :

                    g_skyDoomPlasmaVisualsV12_2A

            )

            {

                if (

                    plasma.startMs <

                    selected->

                        startMs

                )

                {

                    selected =

                        &plasma;

                }

            }

        }



        selected->

            active =

                true;



        selected->
            origin =
                origin;



        selected->

            direction =

                direction;



        selected->
            doomToSkyrimScale =
                scale;

        selected->
            startMs =
                now;

        selected->
            stopDistance =
                collision.distance;

        selected->
            collisionTargetFormID =
                collision.targetFormID;

        selected->
            collisionActorHit =
                collision.actorHit;

        selected->
            collisionWorldHit =
                collision.worldHit;



        SKSE::log::info(

            "[skydoomskse] REAL DOOM plasma visual launched: scale={} speedSkyrimPerSec={}",

            scale,

            875.0f *

                scale

        );

    }



    void SpawnSkyDoomRocket()

    {

        auto* player =

            RE::PlayerCharacter::

                GetSingleton();





        auto* camera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !player ||

            !camera ||

            !camera->

                cameraRoot

        )

        {

            return;

        }





        RE::NiPoint3 direction =

            camera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                1.0f,

                0.0f

            };





        if (

            direction.

                Unitize() <=

            0.0f

        )

        {

            return;

        }





        SkyDoomLogicalRocket*

            rocket =

                nullptr;





        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_ACTIVE_ROCKETS;

            ++i

        )

        {

            if (

                !g_skyDoomRockets[i].

                    active

            )

            {

                rocket =

                    &g_skyDoomRockets[i];



                break;

            }

        }





        if (!rocket)

        {

            SKSE::log::warn(

                "[skydoomskse] REAL DOOM rocket launch dropped: active rocket pool full"

            );



            return;

        }





        const float scale =

            GetSkyDoomDoomToSkyrimScale(

                player

            );





        const float

            spawnAdvance =

                64.0f +

                11.0f *

                    scale;





        const auto now =

            static_cast<

                std::uint64_t

            >(

                GetTickCount64()

            );





        rocket->

            active =

                true;





        rocket->

            direction =

                direction;





        rocket->

            doomToSkyrimScale =

                scale;





        rocket->

            position =

                RE::PlayerCamera::

                    GetActiveCameraPosition() +

                direction *

                    spawnAdvance;





        rocket->

            bornMs =

                now;





        rocket->

            lastUpdateMs =

                now;





        SKSE::log::info(

            "[skydoomskse] REAL DOOM rocket launched: scale={} speedSkyrimPerSec={} splashRadiusSkyrim={}",

            scale,

            700.0f *

                scale,

            128.0f *

                scale

        );

    }





    void QueueSkyDoomRocketDirectImpact(

        RE::Actor* target,

        const RE::NiPoint3& impactPosition,

        const RE::NiPoint3& direction,

        float doomToSkyrimScale

    )

    {

        if (!target)

        {

            ApplySkyDoomRocketSplash(

                impactPosition,

                doomToSkyrimScale

            );



            return;

        }





        SkyDoomPendingRocketImpact*

            pending =

                nullptr;





        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_PENDING_ROCKET_IMPACTS;

            ++i

        )

        {

            if (

                !g_skyDoomPendingRocketImpacts[i].

                    active

            )

            {

                pending =

                    &g_skyDoomPendingRocketImpacts[i];



                break;

            }

        }





        if (!pending)

        {

            SKSE::log::warn(

                "[skydoomskse] REAL DOOM rocket direct-hit request dropped: pending pool full - applying splash only"

            );





            ApplySkyDoomRocketSplash(

                impactPosition,

                doomToSkyrimScale

            );



            return;

        }





        if (

            g_skyDoomNextRocketRequestId <=

                0 ||

            g_skyDoomNextRocketRequestId >

                1000000000

        )

        {

            g_skyDoomNextRocketRequestId =

                1;

        }





        const std::int32_t requestId =

            g_skyDoomNextRocketRequestId++;





        pending->

            active =

                true;





        pending->

            requestId =

                requestId;





        pending->

            targetFormId =

                target->

                    GetFormID();





        pending->

            impactPosition =

                impactPosition;





        pending->

            direction =

                direction;





        pending->

            doomToSkyrimScale =

                doomToSkyrimScale;





        pending->

            createdMs =

                static_cast<

                    std::uint64_t

                >(

                    GetTickCount64()

                );





        PushInputEvent(

            SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,

            0,

            requestId

        );





        SKSE::log::info(

            "[skydoomskse] REAL DOOM rocket actor impact: target={:08X} requestId={}",

            target->

                GetFormID(),

            requestId

        );

    }





    // ========================================================

    // SKYDOOM_ROCKET_RAGDOLL_KNOCKBACK_V11_3

    //

    // PRESENTATION / SKYRIM PHYSICS ONLY.

    //

    // Chocolate Doom remains authoritative for:

    //

    //   - rocket ammo

    //   - firing cadence

    //   - direct-hit P_Random damage

    //   - splash damage

    //   - projectile timing

    //

    // Direct Skyrim NPC hits additionally receive:

    //

    //   1. Skyrim's native kActionRagdollInstant action

    //      if the target survived the direct damage.

    //

    //   2. A modest directional Havok velocity after the

    //      ragdoll has entered the scene.

    //

    // Lethal hits do NOT force a second ragdoll action:

    // Skyrim's normal death ragdoll is allowed to happen,

    // then the same velocity is applied to the corpse.

    //

    // This is deliberately direct-hit only for v11.3.

    // Splash victims are untouched.

    // ========================================================





    RE::Actor*

    FindLoadedSkyDoomActorV11_3(

        std::uint32_t formID

    )

    {

        auto* processLists =

            RE::ProcessLists::

                GetSingleton();





        if (!processLists)

        {

            return nullptr;

        }





        for (

            auto& handle :

                processLists->

                    highActorHandles

        )

        {

            auto actorPtr =

                handle.

                    get();





            auto* actor =

                actorPtr.

                    get();





            if (

                actor &&

                actor->

                    GetFormID() ==

                    formID

            )

            {

                return actor;

            }

        }





        return nullptr;

    }





        // ========================================================
    // SKYDOOM_ROCKET_RAGDOLL_RECOVERY_V15_9C1
    //
    // kActionRagdollInstant can leave some surviving actors,
    // particularly large actors such as Giants, permanently
    // face-down.  Preserve the rocket throw, then let Skyrim
    // repair/blend surviving actors back out of ragdoll.
    //
    // Dead actors are explicitly ignored.
    // ========================================================

    constexpr std::uint64_t
        SKYDOOM_ROCKET_RAGDOLL_RECOVERY_DELAY_MS_V15_9C1 =
            1800u;

    constexpr std::uint64_t
        SKYDOOM_ROCKET_RAGDOLL_RECOVERY_RETRY_MS_V15_9C1 =
            350u;

    constexpr std::uint32_t
        SKYDOOM_ROCKET_RAGDOLL_RECOVERY_MAX_ATTEMPTS_V15_9C1 =
            8u;

    constexpr std::uint32_t
        SKYDOOM_MAX_ROCKET_RAGDOLL_RECOVERIES_V15_9C1 =
            32u;


    struct SkyDoomRocketRagdollRecoveryV15_9C1
    {
        bool active = false;

        std::uint32_t formID = 0u;

        std::uint64_t recoverAtMs = 0u;

        std::uint32_t attempts = 0u;
    };


    std::array<
        SkyDoomRocketRagdollRecoveryV15_9C1,
        SKYDOOM_MAX_ROCKET_RAGDOLL_RECOVERIES_V15_9C1>
        g_skyDoomRocketRagdollRecoveriesV15_9C1{};


    void QueueSkyDoomRocketRagdollRecoveryV15_9C1(
        std::uint32_t formID
    )
    {
        /*
            SKYDOOM_NATURAL_SURVIVOR_KNOCKDOWN_V15_9C2

            Skyrim's kActionKnockdown now owns living-actor recovery.
            Do not force PotentiallyFixRagdollState() after a timer:
            that caused Giants to snap upright or remain in a broken
            face-down state.

            Keep the old queue implementation below for rollback/reference,
            but it is intentionally unreachable.
        */
        (void) formID;
        return;

        if (formID == 0u)
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        SkyDoomRocketRagdollRecoveryV15_9C1*
            selected =
                nullptr;

        for (
            auto& recovery :
                g_skyDoomRocketRagdollRecoveriesV15_9C1
        )
        {
            if (
                recovery.active &&
                recovery.formID ==
                    formID
            )
            {
                selected =
                    &recovery;

                break;
            }

            if (
                !selected &&
                !recovery.active
            )
            {
                selected =
                    &recovery;
            }
        }

        if (!selected)
        {
            selected =
                &g_skyDoomRocketRagdollRecoveriesV15_9C1[0];

            for (
                auto& recovery :
                    g_skyDoomRocketRagdollRecoveriesV15_9C1
            )
            {
                if (
                    recovery.recoverAtMs <
                    selected->recoverAtMs
                )
                {
                    selected =
                        &recovery;
                }
            }

            SKSE::log::warn(
                "[skydoomskse] rocket ragdoll recovery pool full - replacing oldest entry"
            );
        }

        selected->active =
            true;

        selected->formID =
            formID;

        selected->attempts =
            0u;

        selected->recoverAtMs =
            now +
            SKYDOOM_ROCKET_RAGDOLL_RECOVERY_DELAY_MS_V15_9C1;

        SKSE::log::info(
            "[skydoomskse] rocket survivor recovery queued: target={:08X} delay={}ms",
            formID,
            SKYDOOM_ROCKET_RAGDOLL_RECOVERY_DELAY_MS_V15_9C1
        );
    }


    void ProcessSkyDoomRocketRagdollRecoveriesV15_9C1()
    {
        if (
            g_state &&
            g_state->skyrim.paused
        )
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        for (
            auto& recovery :
                g_skyDoomRocketRagdollRecoveriesV15_9C1
        )
        {
            if (
                !recovery.active ||
                now <
                    recovery.recoverAtMs
            )
            {
                continue;
            }

            auto* actor =
                FindLoadedSkyDoomActorV11_3(
                    recovery.formID
                );

            if (
                !actor ||
                actor->IsDead() ||
                actor->IsDisabled() ||
                !actor->Is3DLoaded()
            )
            {
                recovery.active =
                    false;

                continue;
            }

            if (!actor->IsInRagdollState())
            {
                recovery.active =
                    false;

                SKSE::log::info(
                    "[skydoomskse] rocket survivor already recovered: target={:08X}",
                    recovery.formID
                );

                continue;
            }

            actor->PotentiallyFixRagdollState();

            recovery.attempts++;

            if (!actor->IsInRagdollState())
            {
                SKSE::log::info(
                    "[skydoomskse] rocket survivor recovered from ragdoll: target={:08X} attempts={}",
                    recovery.formID,
                    recovery.attempts
                );

                recovery.active =
                    false;

                continue;
            }

            if (
                recovery.attempts >=
                SKYDOOM_ROCKET_RAGDOLL_RECOVERY_MAX_ATTEMPTS_V15_9C1
            )
            {
                SKSE::log::warn(
                    "[skydoomskse] rocket survivor ragdoll recovery gave up: target={:08X} attempts={}",
                    recovery.formID,
                    recovery.attempts
                );

                recovery.active =
                    false;

                continue;
            }

            recovery.recoverAtMs =
                now +
                SKYDOOM_ROCKET_RAGDOLL_RECOVERY_RETRY_MS_V15_9C1;
        }
    }

bool

    ApplySkyDoomRocketRagdollVelocityV11_3(

        std::uint32_t formID,

        RE::NiPoint3 direction,

        std::int32_t doomDamage,

        std::uint32_t attempt

    )

    {

        auto* target =

            FindLoadedSkyDoomActorV11_3(

                formID

            );





        if (

            !target ||

            !target->

                Is3DLoaded()

        )

        {

            SKSE::log::info(

                "[skydoomskse] REAL DOOM rocket knockback waiting: target={:08X} attempt={} reason=actor-not-loaded",

                formID,

                attempt

            );



            return false;

        }





        auto* root =

            target->

                Get3D();





        if (!root)

        {

            SKSE::log::info(

                "[skydoomskse] REAL DOOM rocket knockback waiting: target={:08X} attempt={} reason=no-3d",

                formID,

                attempt

            );



            return false;

        }





        /*

            We want the actor thrown AWAY in the rocket's

            horizontal travel direction rather than driven

            down into the floor by a downward-looking shot.



            Preserve a little projectile pitch through the

            vertical contribution, but always retain a modest

            upward component so the result reads as a physical

            rocket blast.

        */



        const float horizontalLength =

            std::sqrt(

                direction.x *

                    direction.x +

                direction.y *

                    direction.y

            );





        float pushX =

            0.0f;





        float pushY =

            0.0f;





        if (

            horizontalLength >

            0.0001f

        )

        {

            pushX =

                direction.x /

                horizontalLength;





            pushY =

                direction.y /

                horizontalLength;

        }





        /*

            These values are intentionally capped.



            The direct DOOM rocket roll ranges from 20..160.



            Low roll:

                ~3.8 Havok units/sec horizontal



            High roll:

                ~5.0 Havok units/sec horizontal



            This should produce a satisfying shove without

            turning every Skyrim NPC into a space programme.

        */



        const float horizontalSpeed =

            std::clamp(

                4.0f +

                    static_cast<float>(

                        doomDamage

                    ) *

                    0.010f,

                4.2f,

                5.6f

            );





        const float baseLift =

            std::clamp(

                1.0f +

                    static_cast<float>(

                        doomDamage

                    ) *

                    0.0033f,

                1.10f,

                1.50f

            );





        const float pitchContribution =

            std::clamp(

                direction.z,

                -0.25f,

                0.50f

            ) *

            1.25f;





        const float verticalSpeed =

            std::max(

                0.65f,

                baseLift +

                    pitchContribution

            );





        RE::hkVector4 velocity{};





        velocity.quad =

            _mm_set_ps(

                0.0f,

                verticalSpeed,

                pushY *

                    horizontalSpeed,

                pushX *

                    horizontalSpeed

            );





        std::uint32_t affectedBodies =

            0u;





        /*

            Use CommonLib's own scenegraph-collision traversal

            pattern.



            A Skyrim ragdoll consists of several movable Havok

            rigid bodies. Give every movable body the same base

            velocity so the whole ragdoll translates together

            rather than one limb being yanked away.

        */



        RE::BSVisit::

            TraverseScenegraphCollision(

                root,

                [&](

                    RE::bhkNiCollisionObject*

                        collision

                ) ->

                    RE::BSVisit::

                        BSVisitControl

                {

                    if (

                        !collision ||

                        !collision->

                            body

                    )

                    {

                        return

                            RE::BSVisit::

                                BSVisitControl::

                                    kContinue;

                    }





                    auto* rigidBody =

                        collision->

                            body->

                            AsBhkRigidBody();





                    if (

                        !rigidBody ||

                        !rigidBody->

                            referencedObject

                    )

                    {

                        return

                            RE::BSVisit::

                                BSVisitControl::

                                    kContinue;

                    }





                    auto* havokBody =

                        static_cast<

                            RE::hkpRigidBody*>(

                                rigidBody->

                                    referencedObject.

                                    get()

                            );





                    if (

                        !havokBody ||

                        havokBody->

                            motion.

                            GetMass() <=

                            0.0f

                    )

                    {

                        return

                            RE::BSVisit::

                                BSVisitControl::

                                    kContinue;

                    }





                    rigidBody->

                        SetLinearVelocity(

                            velocity

                        );





                    ++affectedBodies;





                    return

                        RE::BSVisit::

                            BSVisitControl::

                                kContinue;

                }

            );





        if (

            affectedBodies ==

            0u

        )

        {

            SKSE::log::info(

                "[skydoomskse] REAL DOOM rocket knockback waiting: target={:08X} attempt={} reason=no-movable-ragdoll-bodies",

                formID,

                attempt

            );



            return false;

        }





        SKSE::log::info(

            "[skydoomskse] REAL DOOM rocket knockback APPLIED: target={:08X} damage={} bodies={} horizontalSpeed={} verticalSpeed={} attempt={}",

            formID,

            doomDamage,

            affectedBodies,

            horizontalSpeed,

            verticalSpeed,

            attempt

        );





                /*
            SKYDOOM_QUEUE_ROCKET_SURVIVOR_RECOVERY_V15_9C1

            Lethal hits remain ordinary Skyrim death ragdolls.
            Only living actors get a delayed get-back-up repair.
        */
        if (!target->IsDead())
        {
            QueueSkyDoomRocketRagdollRecoveryV15_9C1(
                formID
            );
        }

return true;

    }





    void

    ScheduleSkyDoomRocketRagdollVelocityV11_3(

        std::uint32_t formID,

        const RE::NiPoint3& direction,

        std::int32_t doomDamage,

        std::uint32_t attempt

    )

    {

        auto* taskInterface =

            SKSE::

                GetTaskInterface();





        if (!taskInterface)

        {

            ApplySkyDoomRocketRagdollVelocityV11_3(

                formID,

                direction,

                doomDamage,

                attempt

            );



            return;

        }





        taskInterface->

            AddTask(

                [

                    formID,

                    direction,

                    doomDamage,

                    attempt

                ]()

                {

                    const bool applied =

                        ApplySkyDoomRocketRagdollVelocityV11_3(

                            formID,

                            direction,

                            doomDamage,

                            attempt

                        );





                    if (

                        !applied &&

                        attempt <

                            3u

                    )

                    {

                        ScheduleSkyDoomRocketRagdollVelocityV11_3(

                            formID,

                            direction,

                            doomDamage,

                            attempt +

                                1u

                        );

                    }

                    else if (

                        !applied

                    )

                    {

                        SKSE::log::info(

                            "[skydoomskse] REAL DOOM rocket knockback gave up: target={:08X} after {} attempts",

                            formID,

                            attempt

                        );

                    }

                }

            );

    }





        // ========================================================
    // SKYDOOM_NATIVE_PUSH_ACTOR_AWAY_V15_9C3
    //
    // Living rocket victims now use Skyrim's own
    // ObjectReference.PushActorAway native function.
    //
    // This deliberately avoids:
    //   - kActionKnockdown / kActionRagdollInstant
    //   - manual ragdoll state repair
    //   - direct SetLinearVelocity on living actor rigid bodies
    //
    // Skyrim owns ragdoll entry, flight, landing and get-up.
    // ========================================================

    class SkyDoomPushActorAwayArgsV15_9C3 :
        public RE::BSScript::IFunctionArguments
    {
    public:
        SkyDoomPushActorAwayArgsV15_9C3(
            RE::Actor* target,
            float force
        ) :
            target_(target),
            force_(force)
        {
        }


        bool operator()(
            RE::BSScrapArray<
                RE::BSScript::Variable>& args
        ) const override
        {
            args.resize(2);

            args[0].Pack(
                target_
            );

            args[1].Pack(
                force_
            );

            return true;
        }


    private:
        RE::Actor* target_ =
            nullptr;

        float force_ =
            0.0f;
    };


    bool PushSkyDoomLivingActorAwayV15_9C3(
        RE::Actor* target,
        std::int32_t doomDamage
    )
    {
        if (
            !target ||
            target->IsDead()
        )
        {
            return false;
        }

        auto* player =
            RE::PlayerCharacter::
                GetSingleton();

        auto* vm =
            RE::BSScript::Internal::
                VirtualMachine::
                    GetSingleton();

        if (
            !player ||
            !vm
        )
        {
            SKSE::log::warn(
                "[skydoomskse] PushActorAway unavailable: player={} vm={}",
                static_cast<void*>(player),
                static_cast<void*>(vm)
            );

            return false;
        }

        auto* policy =
            vm->
                GetObjectHandlePolicy();

        if (!policy)
        {
            SKSE::log::warn(
                "[skydoomskse] PushActorAway unavailable: no object handle policy"
            );

            return false;
        }

        const auto playerHandle =
            policy->
                GetHandleForObject(
                    static_cast<RE::VMTypeID>(
                        player->GetFormType()
                    ),
                    player
                );

        /*
            SKYDOOM_SIZE_TUNED_PUSH_V15_9C4

            v15.9c3 proved Skyrim's native PushActorAway path
            gives the natural knockdown/ragdoll/get-up behaviour
            we want.

            Give ordinary NPC-sized actors a noticeably stronger
            rocket kick while retaining the known-good 15.0 force
            for very large actors such as Giants.

            Mammoths are intentionally not specially handled.
        */
        const float targetHeight =
            target->GetHeight();

        constexpr float
            normalActorPushForce =
                20.0f;

        constexpr float
            largeActorPushForce =
                15.0f;

        constexpr float
            largeActorHeightThreshold =
                200.0f;

        const float pushForce =
            targetHeight >=
                largeActorHeightThreshold
                    ? largeActorPushForce
                    : normalActorPushForce;

        auto* args =
            new SkyDoomPushActorAwayArgsV15_9C3(
                target,
                pushForce
            );

        RE::BSTSmartPointer<
            RE::BSScript::
                IStackCallbackFunctor>
            callback;

        vm->
            DispatchMethodCall(
                playerHandle,
                RE::BSFixedString(
                    "ObjectReference"
                ),
                RE::BSFixedString(
                    "PushActorAway"
                ),
                args,
                callback
            );

        SKSE::log::info(
            "[skydoomskse] native PushActorAway queued: target={:08X} force={} height={} doomDamage={}",
            target->GetFormID(),
            pushForce,
            targetHeight,
            doomDamage
        );

        return true;
    }

void

    QueueSkyDoomRocketKnockbackV11_3(

        RE::Actor* target,

        const RE::NiPoint3& direction,

        std::int32_t doomDamage,

        bool lethalDirectHit

    )

    {

        if (!target)

        {

            return;

        }

        /*
            SKYDOOM_NATIVE_PUSH_ACTOR_AWAY_V15_9C3

            ALL living direct-hit actors use Skyrim's native
            PushActorAway path and return here.

            This includes ordinary NPCs, creatures and Giants.

            The old v11.3 ragdoll/rigid-body velocity machinery
            remains below ONLY for lethal direct hits / corpses.
        */
        if (!lethalDirectHit)
        {
            if (
                !PushSkyDoomLivingActorAwayV15_9C3(
                    target,
                    doomDamage
                )
            )
            {
                SKSE::log::warn(
                    "[skydoomskse] native PushActorAway dispatch failed: target={:08X}",
                    target->GetFormID()
                );
            }

            return;
        }





        const std::uint32_t formID =

            target->

                GetFormID();





        bool ragdollActionResult =

            false;





        bool requestedRagdoll =

            false;





        /*

            A living target needs Skyrim to transition from its

            character controller / animation graph into ragdoll.



            For a lethal direct hit, Skyrim is already doing its

            normal death transition, so do not interfere with

            that. We only add velocity afterwards.

        */



        if (

            !lethalDirectHit &&

            !target->

                IsInRagdollState()

        )

        {

            requestedRagdoll =

                true;





            ragdollActionResult =

                RE::SourceActionMap::

                    DoAction(

                        target,

                        RE::DEFAULT_OBJECT::

                            kActionKnockdown

                    );

        }





        SKSE::log::info(

            "[skydoomskse] REAL DOOM rocket knockback QUEUED: target={:08X} damage={} lethal={} requestedRagdoll={} ragdollActionResult={} alreadyRagdoll={}",

            formID,

            doomDamage,

            lethalDirectHit,

            requestedRagdoll,

            ragdollActionResult,

            target->

                IsInRagdollState()

        );





        /*

            Do the velocity work through the SKSE task queue.



            This gives Skyrim's native ragdoll action / death

            transition a chance to expose the ragdoll's Havok

            rigid bodies before we touch them.



            If they are not ready yet, the helper retries up to

            three task passes.

        */



        ScheduleSkyDoomRocketRagdollVelocityV11_3(

            formID,

            direction,

            doomDamage,

            1u

        );

    }





    constexpr std::uint32_t

        SKYDOOM_MAX_PENDING_PLASMA_IMPACTS_V13 =

            64u;



    struct SkyDoomPendingPlasmaImpactV13

    {

        bool active =

            false;



        std::int32_t requestId =

            0;



        std::uint32_t targetFormId =

            0u;



        RE::NiPoint3 impactPosition{};



        RE::NiPoint3 direction{};



        std::uint64_t createdMs =

            0;

    };



    std::array<

        SkyDoomPendingPlasmaImpactV13,

        SKYDOOM_MAX_PENDING_PLASMA_IMPACTS_V13>

        g_skyDoomPendingPlasmaImpactsV13{};



    struct SkyDoomPlasmaImpactTriggerV13

    {

        bool actorHit =

            false;



        bool worldHit =

            false;



        std::uint32_t targetFormId =

            0u;



        RE::NiPoint3 position{};



        RE::NiPoint3 direction{};



        float doomToSkyrimScale =

            1.0f;

    };



    bool QueueSkyDoomPlasmaDirectImpactV13(

        std::uint32_t targetFormId,

        const RE::NiPoint3& impactPosition,

        const RE::NiPoint3& direction

    )

    {

        if (targetFormId == 0u)

        {

            return false;

        }



        SkyDoomPendingPlasmaImpactV13*

            pending =

                nullptr;



        for (

            auto& candidate :

                g_skyDoomPendingPlasmaImpactsV13

        )

        {

            if (!candidate.active)

            {

                pending =

                    &candidate;



                break;

            }

        }



        if (!pending)

        {

            SKSE::log::warn(

                "[skydoomskse] REAL DOOM plasma damage request dropped: pending pool full"

            );



            return false;

        }



        if (

            g_skyDoomNextRocketRequestId <=

                0 ||

            g_skyDoomNextRocketRequestId >

                1000000000

        )

        {

            g_skyDoomNextRocketRequestId =

                1;

        }



        const std::int32_t requestId =

            g_skyDoomNextRocketRequestId++;



        pending->active =

            true;



        pending->requestId =

            requestId;



        pending->targetFormId =

            targetFormId;



        pending->impactPosition =

            impactPosition;



        pending->direction =

            direction;



        pending->createdMs =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        /*

            Reuse the proven rocket damage-request transport.

            code identifies this request as PLASMA; the event

            response is tagged SKYDOOM_COMBAT_WEAPON_PLASMA.

        */

        PushInputEvent(

            SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,

            SKYDOOM_COMBAT_WEAPON_PLASMA,

            requestId

        );



        SKSE::log::info(

            "[skydoomskse] REAL DOOM plasma actor impact: target={:08X} requestId={}",

            targetFormId,

            requestId

        );



        return true;

    }



    // SKYDOOM_BFG_DAMAGE_SPRAY_V14_3
    bool QueueSkyDoomBFGDamageRequestV14_3(
        std::uint32_t targetFormId,
        const RE::NiPoint3& hitPosition,
        const RE::NiPoint3& direction,
        bool spray
    );

    void ProcessSkyDoomBFGSpraysV14_3(
        bool doomFresh
    );

    void ResolveSkyDoomBFGDamageRollV14_3(
        std::int32_t requestId,
        std::int32_t doomDamage,
        std::int32_t mode
    );

    struct SkyDoomBFGImpactTriggerV14_2B
    {
        bool actorHit =
            false;

        bool worldHit =
            false;

        std::uint32_t targetFormId =
            0u;

        RE::NiPoint3 position{};

        RE::NiPoint3 direction{};

        float doomToSkyrimScale =
            1.0f;
    };

    void ProcessSkyDoomBFGImpactsV14_2B(
        bool doomFresh
    )
    {
        if (
            !doomFresh ||
            !g_state ||
            !g_state->
                skyrim.
                in_game
        )
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        std::array<
            SkyDoomBFGImpactTriggerV14_2B,
            SKYDOOM_MAX_ACTIVE_BFG_VISUALS_V14_2A>
            triggers{};

        std::uint32_t triggerCount =
            0u;

        {
            std::lock_guard<std::mutex>
                lock(
                    g_skyDoomBFGVisualMutexV14_2A
                );

            for (
                auto& bfg :
                    g_skyDoomBFGVisualsV14_2A
            )
            {
                if (
                    !bfg.active ||
                    now <
                        bfg.startMs
                )
                {
                    continue;
                }

                const std::uint64_t elapsed =
                    now -
                    bfg.startMs;

                const float speed =
                    875.0f *
                    bfg.
                        doomToSkyrimScale;

                const float travelDistance =
                    speed *
                    (
                        static_cast<float>(
                            elapsed
                        ) /
                        1000.0f
                    );

                if (
                    (
                        bfg.collisionActorHit ||
                        bfg.collisionWorldHit
                    ) &&
                    travelDistance >=
                        bfg.stopDistance
                )
                {
                    if (
                        triggerCount <
                        SKYDOOM_MAX_ACTIVE_BFG_VISUALS_V14_2A
                    )
                    {
                        auto& trigger =
                            triggers[
                                triggerCount++
                            ];

                        trigger.actorHit =
                            bfg.collisionActorHit;

                        trigger.worldHit =
                            bfg.collisionWorldHit;

                        trigger.targetFormId =
                            bfg.collisionTargetFormID;

                        trigger.position =
                            bfg.origin +
                            bfg.direction *
                                bfg.stopDistance;

                        trigger.direction =
                            bfg.direction;

                        trigger.doomToSkyrimScale =
                            bfg.doomToSkyrimScale;
                    }

                    bfg.active =
                        false;

                    continue;
                }

                if (
                    elapsed >=
                    SKYDOOM_BFG_VISUAL_LIFETIME_MS_V14_2A
                )
                {
                    bfg.active =
                        false;
                }
            }
        }

        for (
            std::uint32_t i = 0u;
            i <
                triggerCount;
            ++i
        )
        {
            const auto& trigger =
                triggers[i];

            StartSkyDoomBFGImpactVisualV14_2B(
                trigger.position,
                trigger.doomToSkyrimScale,
                trigger.direction
            );

            /*
                The hidden Chocolate Doom MT_BFG is suppressed,
                so explicitly request its genuine sfx_rxplod
                deathsound when Skyrim reaches the real impact.
            */
            PushInputEvent(
                SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,
                SKYDOOM_COMBAT_WEAPON_BFG,
                0
            );

            if (trigger.actorHit)
            {
                QueueSkyDoomBFGDamageRequestV14_3(
                    trigger.targetFormId,
                    trigger.position,
                    trigger.direction,
                    false
                );

                SKSE::log::info(
                    "[skydoomskse] REAL DOOM BFG ACTOR impact: target={:08X}",
                    trigger.targetFormId
                );
            }
            else if (trigger.worldHit)
            {
                SKSE::log::info(
                    "[skydoomskse] REAL DOOM BFG WORLD impact"
                );
            }
        }
    }

    void ProcessSkyDoomPlasmaImpactsV13(

        bool doomFresh

    )

    {

        if (

            !doomFresh ||

            !g_state ||

            !g_state->

                skyrim.

                in_game

        )

        {

            return;

        }



        const auto now =

            static_cast<std::uint64_t>(

                GetTickCount64()

            );



        for (

            auto& pending :

                g_skyDoomPendingPlasmaImpactsV13

        )

        {

            if (

                pending.active &&

                now >=

                    pending.createdMs &&

                now -

                    pending.createdMs >

                    2000u

            )

            {

                SKSE::log::warn(

                    "[skydoomskse] REAL DOOM plasma damage response timeout: requestId={}",

                    pending.requestId

                );



                pending.active =

                    false;

            }

        }



        std::array<

            SkyDoomPlasmaImpactTriggerV13,

            SKYDOOM_MAX_ACTIVE_PLASMA_VISUALS>

            triggers{};



        std::uint32_t triggerCount =

            0u;



        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomPlasmaVisualMutexV12_2A

                );



            for (

                auto& plasma :

                    g_skyDoomPlasmaVisualsV12_2A

            )

            {

                if (

                    !plasma.active ||

                    now <

                        plasma.startMs

                )

                {

                    continue;

                }



                const std::uint64_t elapsed =

                    now -

                    plasma.startMs;



                const float speed =

                    875.0f *

                    plasma.

                        doomToSkyrimScale;



                const float travelDistance =

                    speed *

                    (

                        static_cast<float>(

                            elapsed

                        ) /

                        1000.0f

                    );



                if (

                    (

                        plasma.collisionActorHit ||

                        plasma.collisionWorldHit

                    ) &&

                    travelDistance >=

                        plasma.stopDistance

                )

                {

                    if (

                        triggerCount <

                        SKYDOOM_MAX_ACTIVE_PLASMA_VISUALS

                    )

                    {

                        auto& trigger =

                            triggers[

                                triggerCount++

                            ];



                        trigger.actorHit =

                            plasma.collisionActorHit;



                        trigger.worldHit =

                            plasma.collisionWorldHit;



                        trigger.targetFormId =

                            plasma.collisionTargetFormID;



                        trigger.position =

                            plasma.origin +

                            plasma.direction *

                                plasma.stopDistance;



                        trigger.direction =

                            plasma.direction;



                        trigger.doomToSkyrimScale =

                            plasma.doomToSkyrimScale;

                    }



                    /*

                        This makes the collision a one-shot event

                        and also removes the travelling sprite.

                    */

                    plasma.active =

                        false;



                    continue;

                }



                if (

                    elapsed >=

                    SKYDOOM_PLASMA_VISUAL_LIFETIME_MS

                )

                {

                    plasma.active =

                        false;

                }

            }

        }



        for (

            std::uint32_t i = 0u;

            i <

                triggerCount;

            ++i

        )

        {

            const auto& trigger =

                triggers[i];



            StartSkyDoomPlasmaImpactVisualV13(

                trigger.position,

                trigger.doomToSkyrimScale

            );



            if (trigger.actorHit)

            {

                QueueSkyDoomPlasmaDirectImpactV13(

                    trigger.targetFormId,

                    trigger.position,

                    trigger.direction

                );

            }

            else if (trigger.worldHit)

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM plasma WORLD impact"

                );

            }

        }

    }



    void ResolveSkyDoomPlasmaDamageRollV13(

        std::int32_t requestId,

        std::int32_t doomDamage

    )

    {

        SkyDoomPendingPlasmaImpactV13*

            pending =

                nullptr;



        for (

            auto& candidate :

                g_skyDoomPendingPlasmaImpactsV13

        )

        {

            if (

                candidate.active &&

                candidate.requestId ==

                    requestId

            )

            {

                pending =

                    &candidate;



                break;

            }

        }



        if (!pending)

        {

            SKSE::log::warn(

                "[skydoomskse] REAL DOOM plasma damage response has no pending impact: requestId={} damage={}",

                requestId,

                doomDamage

            );



            return;

        }



        auto* player =

            RE::PlayerCharacter::

                GetSingleton();



        auto* target =

            FindSkyDoomActorByFormId(

                pending->

                    targetFormId

            );



        if (

            player &&

            target &&

            !target->

                IsDead() &&

            !target->

                IsDisabled() &&

            target->

                Is3DLoaded()

        )

        {

            auto* actorValueOwner =

                target->

                    AsActorValueOwner();



            if (actorValueOwner)

            {

                const float healthBefore =

                    actorValueOwner->

                        GetActorValue(

                            RE::ActorValue::

                                kHealth

                        );



                if (healthBefore > 0.0f)

                {

                    const bool usedNativeHitData =

                        ApplySkyDoomNativePistolHit(

                            player,

                            target,

                            static_cast<float>(

                                doomDamage

                            ),

                            pending->

                                impactPosition,

                            pending->

                                direction

                        );



                    if (!usedNativeHitData)

                    {

                        target->

                            DoDamage(

                                static_cast<float>(

                                    doomDamage

                                ),

                                player,

                                true

                            );

                    }



                    const float healthAfter =

                        actorValueOwner->

                            GetActorValue(

                                RE::ActorValue::

                                    kHealth

                            );



                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM plasma DIRECT HIT: target={:08X} requestId={} damage={} health={} -> {} path={}",

                        target->

                            GetFormID(),

                        requestId,

                        doomDamage,

                        healthBefore,

                        healthAfter,

                        usedNativeHitData ?

                            "HitData" :

                            "DoDamage"

                    );

                }

            }

        }



        pending->active =

            false;

    }



    // ========================================================
    // SKYDOOM_BFG_DAMAGE_SPRAY_V14_3
    //
    // Direct missile hit:
    //   Chocolate Doom rolls ((P_Random()%8)+1) * 100.
    //
    // Spray:
    //   40 rays across 90 degrees.
    //   -45 degrees + (2.25 degrees * ray index)
    //   1024 Doom-unit range.
    //   Each successful target ray asks Chocolate Doom for
    //   15 independent ((P_Random()&7)+1) rolls.
    // ========================================================

    constexpr std::uint32_t
        SKYDOOM_MAX_PENDING_BFG_DAMAGE_V14_3 =
            128u;

    struct SkyDoomPendingBFGDamageV14_3
    {
        bool active =
            false;

        bool spray =
            false;

        std::int32_t requestId =
            0;

        std::uint32_t targetFormId =
            0u;

        RE::NiPoint3 hitPosition{};

        RE::NiPoint3 direction{};

        std::uint64_t createdMs =
            0;
    };

    std::array<
        SkyDoomPendingBFGDamageV14_3,
        SKYDOOM_MAX_PENDING_BFG_DAMAGE_V14_3>
        g_skyDoomPendingBFGDamageV14_3{};

    bool QueueSkyDoomBFGDamageRequestV14_3(
        std::uint32_t targetFormId,
        const RE::NiPoint3& hitPosition,
        const RE::NiPoint3& direction,
        bool spray
    )
    {
        if (targetFormId == 0u)
        {
            return false;
        }

        SkyDoomPendingBFGDamageV14_3*
            pending =
                nullptr;

        for (
            auto& candidate :
                g_skyDoomPendingBFGDamageV14_3
        )
        {
            if (!candidate.active)
            {
                pending =
                    &candidate;

                break;
            }
        }

        if (!pending)
        {
            SKSE::log::warn(
                "[skydoomskse] REAL DOOM BFG damage request dropped: pending pool full"
            );

            return false;
        }

        if (
            g_skyDoomNextRocketRequestId <=
                0 ||
            g_skyDoomNextRocketRequestId >
                1000000000
        )
        {
            g_skyDoomNextRocketRequestId =
                1;
        }

        const std::int32_t requestId =
            g_skyDoomNextRocketRequestId++;

        pending->active =
            true;

        pending->spray =
            spray;

        pending->requestId =
            requestId;

        pending->targetFormId =
            targetFormId;

        pending->hitPosition =
            hitPosition;

        pending->direction =
            direction;

        pending->createdMs =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        /*
            Same proven input transport as rocket/plasma.

            BFG value encoding:
              0          = impact sound only
              +requestId = direct missile RNG request
              -requestId = spray RNG request
        */
        PushInputEvent(
            SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,
            SKYDOOM_COMBAT_WEAPON_BFG,
            spray ?
                -requestId :
                requestId
        );

        SKSE::log::info(
            "[skydoomskse] REAL DOOM BFG {} request: target={:08X} requestId={}",
            spray ?
                "SPRAY" :
                "DIRECT",
            targetFormId,
            requestId
        );

        return true;
    }

    void ResolveSkyDoomBFGDamageRollV14_3(
        std::int32_t requestId,
        std::int32_t doomDamage,
        std::int32_t mode
    )
    {
        SkyDoomPendingBFGDamageV14_3*
            pending =
                nullptr;

        for (
            auto& candidate :
                g_skyDoomPendingBFGDamageV14_3
        )
        {
            if (
                candidate.active &&
                candidate.requestId ==
                    requestId
            )
            {
                pending =
                    &candidate;

                break;
            }
        }

        if (!pending)
        {
            SKSE::log::warn(
                "[skydoomskse] REAL DOOM BFG damage response has no pending request: requestId={} damage={} mode={}",
                requestId,
                doomDamage,
                mode
            );

            return;
        }

        const bool responseIsSpray =
            mode ==
                2;

        if (
            responseIsSpray !=
                pending->spray
        )
        {
            SKSE::log::warn(
                "[skydoomskse] REAL DOOM BFG response mode mismatch: requestId={} pendingSpray={} mode={}",
                requestId,
                pending->spray,
                mode
            );

            pending->active =
                false;

            return;
        }

        auto* player =
            RE::PlayerCharacter::
                GetSingleton();

        auto* target =
            FindSkyDoomActorByFormId(
                pending->
                    targetFormId
            );

        if (
            player &&
            target &&
            !target->
                IsDead() &&
            !target->
                IsDisabled() &&
            target->
                Is3DLoaded()
        )
        {
            auto* actorValueOwner =
                target->
                    AsActorValueOwner();

            if (actorValueOwner)
            {
                const float healthBefore =
                    actorValueOwner->
                        GetActorValue(
                            RE::ActorValue::
                                kHealth
                        );

                if (healthBefore > 0.0f)
                {
                    const bool usedNativeHitData =
                        ApplySkyDoomNativePistolHit(
                            player,
                            target,
                            static_cast<float>(
                                doomDamage
                            ),
                            pending->
                                hitPosition,
                            pending->
                                direction
                        );

                    if (!usedNativeHitData)
                    {
                        target->
                            DoDamage(
                                static_cast<float>(
                                    doomDamage
                                ),
                                player,
                                true
                            );
                    }

                    const float healthAfter =
                        actorValueOwner->
                            GetActorValue(
                                RE::ActorValue::
                                    kHealth
                            );

                    SKSE::log::info(
                        "[skydoomskse] REAL DOOM BFG {} HIT: target={:08X} requestId={} damage={} health={} -> {} path={}",
                        pending->spray ?
                            "SPRAY" :
                            "DIRECT",
                        target->
                            GetFormID(),
                        requestId,
                        doomDamage,
                        healthBefore,
                        healthAfter,
                        usedNativeHitData ?
                            "HitData" :
                            "DoDamage"
                    );
                }
            }
        }

        pending->active =
            false;
    }

    void ExecuteSkyDoomBFGSprayV14_3(
        const RE::NiPoint3& frozenAttackDirection,
        float doomToSkyrimScale
    )
    {
        auto* player =
            RE::PlayerCharacter::
                GetSingleton();

        auto* camera =
            RE::PlayerCamera::
                GetSingleton();

        if (
            !player ||
            !camera ||
            !camera->
                cameraRoot
        )
        {
            return;
        }

        RE::NiPoint3 baseDirection =
            frozenAttackDirection;

        if (
            baseDirection.
                Unitize() <=
            0.0f
        )
        {
            return;
        }

        const RE::NiPoint3 origin =
            RE::PlayerCamera::
                GetActiveCameraPosition();

        const float maxDistance =
            1024.0f *
            doomToSkyrimScale;

        constexpr float
            degreesToRadians =
                0.01745329251994329577f;

        std::uint32_t successfulRays =
            0u;

        for (
            std::uint32_t rayIndex = 0u;
            rayIndex < 40u;
            ++rayIndex
        )
        {
            /*
                Exact A_BFGSpray horizontal fan:
                  -45 degrees
                  + (90/40 * i)
                for i = 0..39.

                Therefore the last ray is +42.75 degrees.
            */
            const float angleDegrees =
                -45.0f +
                2.25f *
                static_cast<float>(
                    rayIndex
                );

            const float angleRadians =
                angleDegrees *
                degreesToRadians;

            const float c =
                std::cos(
                    angleRadians
                );

            const float s =
                std::sin(
                    angleRadians
                );

            RE::NiPoint3 rayDirection{
                baseDirection.x *
                    c -
                baseDirection.y *
                    s,
                baseDirection.x *
                    s +
                baseDirection.y *
                    c,
                baseDirection.z
            };

            if (
                rayDirection.
                    Unitize() <=
                0.0f
            )
            {
                continue;
            }

            /*
                Radius zero makes the proven projectile collision
                helper behave as a line ray against Skyrim actor
                bounds, while still letting nearer Havok geometry
                block the actor.
            */
            const SkyDoomPlasmaCollisionV12_2B collision =
                FindSkyDoomPlasmaCollisionV12_2B(
                    origin,
                    rayDirection,
                    maxDistance,
                    0.0f,
                    player
                );

            if (
                !collision.actorHit ||
                collision.targetFormID ==
                    0u
            )
            {
                continue;
            }

            auto* target =
                FindSkyDoomActorByFormId(
                    collision.
                        targetFormID
                );

            if (
                !target ||
                target->
                    IsDead() ||
                target->
                    IsDisabled() ||
                !target->
                    Is3DLoaded()
            )
            {
                continue;
            }

            const RE::NiPoint3 damagePosition =
                origin +
                rayDirection *
                    collision.distance;

            RE::NiPoint3 visualPosition =
                target->
                    GetPosition();

            float targetHeight =
                target->
                    GetHeight();

            if (targetHeight < 21.0f)
            {
                targetHeight =
                    21.0f;
            }

            if (targetHeight > 840.0f)
            {
                targetHeight =
                    840.0f;
            }

            /*
                Vanilla A_BFGSpray spawns MT_EXTRABFG at
                target z + target height/4.
            */
            visualPosition.z +=
                targetHeight *
                0.25f;

            StartSkyDoomBFGExtraVisualV14_3(
                visualPosition,
                doomToSkyrimScale
            );

            if (
                QueueSkyDoomBFGDamageRequestV14_3(
                    collision.targetFormID,
                    damagePosition,
                    rayDirection,
                    true
                )
            )
            {
                ++successfulRays;
            }
        }

        SKSE::log::info(
            "[skydoomskse] REAL DOOM A_BFGSpray: successful target rays={} of 40 rangeSkyrim={}",
            successfulRays,
            maxDistance
        );
    }

    void ProcessSkyDoomBFGSpraysV14_3(
        bool doomFresh
    )
    {
        if (
            !doomFresh ||
            !g_state ||
            !g_state->
                skyrim.
                in_game
        )
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        for (
            auto& pending :
                g_skyDoomPendingBFGDamageV14_3
        )
        {
            if (
                pending.active &&
                now >=
                    pending.createdMs &&
                now -
                    pending.createdMs >
                    3000u
            )
            {
                SKSE::log::warn(
                    "[skydoomskse] REAL DOOM BFG damage response timeout: requestId={} spray={}",
                    pending.requestId,
                    pending.spray
                );

                pending.active =
                    false;
            }
        }

        struct SprayTrigger
        {
            RE::NiPoint3 attackDirection{};

            float doomToSkyrimScale =
                1.0f;
        };

        std::array<
            SprayTrigger,
            SKYDOOM_MAX_BFG_IMPACTS_V14_2B>
            triggers{};

        std::uint32_t triggerCount =
            0u;

        {
            std::lock_guard<std::mutex>
                lock(
                    g_skyDoomBFGImpactMutexV14_2B
                );

            for (
                auto& impact :
                    g_skyDoomBFGImpactsV14_2B
            )
            {
                if (
                    !impact.active ||
                    impact.sprayTriggered ||
                    now <
                        impact.startMs
                )
                {
                    continue;
                }

                const std::uint64_t elapsed =
                    now -
                    impact.startMs;

                /*
                    A_BFGSpray is called when S_BFGLAND enters
                    BFE1C: after A + B = 16 Doom tics ~= 457 ms.
                */
                if (
                    elapsed <
                    SKYDOOM_BFG_IMPACT_B_END_MS_V14_2B
                )
                {
                    continue;
                }

                impact.sprayTriggered =
                    true;

                if (
                    triggerCount <
                    SKYDOOM_MAX_BFG_IMPACTS_V14_2B
                )
                {
                    auto& trigger =
                        triggers[
                            triggerCount++
                        ];

                    trigger.attackDirection =
                        impact.attackDirection;

                    trigger.doomToSkyrimScale =
                        impact.doomToSkyrimScale;
                }
            }
        }

        for (
            std::uint32_t i = 0u;
            i < triggerCount;
            ++i
        )
        {
            ExecuteSkyDoomBFGSprayV14_3(
                triggers[i].
                    attackDirection,
                triggers[i].
                    doomToSkyrimScale
            );
        }
    }

    // ========================================================
    // SKYDOOM_PHYSICAL_PICKUPS_V15
    //
    // Stage 1 proof:
    //   * watch loaded high-process Skyrim actors
    //   * detect ALIVE -> DEAD once
    //   * spawn a genuine DOOM MEDIA0 medikit billboard
    //   * walking over it asks Chocolate Doom to grant the item
    //   * Chocolate Doom remains authoritative for acceptance
    //
    // This first proof intentionally reacts to ANY newly-dead
    // loaded non-player actor. Once the physical loop is proven,
    // the death source will be tightened to SkyDoom/player kills.
    // ========================================================

    constexpr std::uint32_t
        SKYDOOM_MAX_TRACKED_DEATH_ACTORS_V15 =
            512u;

    constexpr std::uint32_t
        SKYDOOM_MAX_ACTIVE_PICKUPS_V15 =
            64u;

    struct SkyDoomTrackedDeathActorV15
    {
        bool active =
            false;

        bool wasDead =
            false;

        std::uint32_t formId =
            0u;

        std::uint64_t lastSeenMs =
            0u;
    };

    struct SkyDoomPhysicalPickupV15
    {
        bool active =
            false;

        bool awaitingResult =
            false;

        bool blockedByHealth =
            false;

        std::int32_t requestId =
            0;

        std::int32_t blockedHealth =
            0;

        std::uint16_t pickupType =
            SKYDOOM_PICKUP_MEDIKIT;

        RE::NiPoint3 groundPosition{};

        float doomToSkyrimScale =
            1.0f;

        std::uint64_t spawnMs =
            0u;

        std::uint64_t requestMs =
            0u;
    };

    std::array<
        SkyDoomTrackedDeathActorV15,
        SKYDOOM_MAX_TRACKED_DEATH_ACTORS_V15>
        g_skyDoomTrackedDeathActorsV15{};

    std::mutex
        g_skyDoomPhysicalPickupMutexV15;

    std::array<
        SkyDoomPhysicalPickupV15,
        SKYDOOM_MAX_ACTIVE_PICKUPS_V15>
        g_skyDoomPhysicalPickupsV15{};

    std::int32_t
        g_skyDoomNextPickupRequestIdV15 =
            1;

    // ========================================================
    // SKYDOOM_RESOURCE_DROPS_V15_3
    // ========================================================

    std::uint32_t
        g_skyDoomPickupRandomStateV15_3 =
            0xA341316Cu;

    const char* SkyDoomPickupNameV15_3(
        std::uint16_t pickupType
    )
    {
        switch (pickupType)
        {
            case SKYDOOM_PICKUP_MEDIKIT:
                return "MEDIKIT";

            case SKYDOOM_PICKUP_ARMOR:
                return "GREEN ARMOR";

            case SKYDOOM_PICKUP_BULLETS:
                return "BULLETS";

            case SKYDOOM_PICKUP_SHELLS:
                return "SHELLS";

            case SKYDOOM_PICKUP_ROCKET:
                return "ROCKET";

            case SKYDOOM_PICKUP_CELLS:
                return "CELLS";

            case SKYDOOM_PICKUP_ARMOR_BONUS:
                return "ARMOR BONUS";

            case SKYDOOM_PICKUP_STIMPACK:
                return "STIMPACK";

            case SKYDOOM_PICKUP_BLUE_ARMOR:
                return "BLUE ARMOR";

            case SKYDOOM_PICKUP_BULLET_BOX:
                return "BULLET BOX";

            case SKYDOOM_PICKUP_SHELL_BOX:
                return "SHELL BOX";

            case SKYDOOM_PICKUP_ROCKET_BOX:
                return "ROCKET BOX";

            case SKYDOOM_PICKUP_CELL_PACK:
                return "CELL PACK";

            case SKYDOOM_PICKUP_HEALTH_BONUS:
                return "HEALTH BONUS";

            case SKYDOOM_PICKUP_SOULSPHERE:
                return "SOULSPHERE";

            case SKYDOOM_PICKUP_BACKPACK:
                return "BACKPACK";

            case SKYDOOM_PICKUP_MEGASPHERE:
                return "MEGASPHERE";

            default:
                return "UNKNOWN";
        }
    }









    std::int32_t SkyDoomPickupResourceValueV15_3(
        std::uint16_t pickupType
    )
    {
        if (!g_state)
        {
            return 0;
        }

        switch (pickupType)
        {
            case SKYDOOM_PICKUP_MEDIKIT:
            case SKYDOOM_PICKUP_STIMPACK:
            case SKYDOOM_PICKUP_HEALTH_BONUS:
            case SKYDOOM_PICKUP_SOULSPHERE:
            case SKYDOOM_PICKUP_MEGASPHERE:
                return g_state->
                    doom.
                    health;

            case SKYDOOM_PICKUP_ARMOR:
            case SKYDOOM_PICKUP_ARMOR_BONUS:
            case SKYDOOM_PICKUP_BLUE_ARMOR:
                return g_state->
                    doom.
                    armor;

            case SKYDOOM_PICKUP_BULLETS:
            case SKYDOOM_PICKUP_BULLET_BOX:
                return g_state->
                    doom.
                    ammo_bullets;

            case SKYDOOM_PICKUP_SHELLS:
            case SKYDOOM_PICKUP_SHELL_BOX:
                return g_state->
                    doom.
                    ammo_shells;

            case SKYDOOM_PICKUP_ROCKET:
            case SKYDOOM_PICKUP_ROCKET_BOX:
                return g_state->
                    doom.
                    ammo_rockets;

            case SKYDOOM_PICKUP_CELLS:
            case SKYDOOM_PICKUP_CELL_PACK:
                return g_state->
                    doom.
                    ammo_cells;

            case SKYDOOM_PICKUP_BACKPACK:
                return g_state->
                    doom.
                    has_backpack != 0u ?
                        1 :
                        0;

            default:
                return 0;
        }
    }









    std::uint32_t SkyDoomPickupNeedWeightV15_3(
        std::int32_t current,
        std::int32_t maximum
    )
    {
        if (
            maximum <= 0 ||
            current >= maximum
        )
        {
            return 0u;
        }

        if (current < 0)
        {
            current = 0;
        }

        const std::int32_t deficit =
            maximum -
            current;

        const std::uint32_t deficitPercent =
            static_cast<std::uint32_t>(
                (
                    deficit *
                    100
                ) /
                maximum
            );

        std::uint32_t weight =
            10u +
            deficitPercent *
                2u;

        /*
            Still random, but strongly favour genuinely scarce
            resources.  This is intentionally a SkyDoom rule;
            actual resource grant amounts/caps remain Doom-native.
        */
        if (
            current * 4 <=
            maximum
        )
        {
            weight +=
                150u;
        }
        else if (
            current * 2 <=
            maximum
        )
        {
            weight +=
                75u;
        }

        return weight;
    }

    std::uint32_t SkyDoomNextPickupRandomV15_3(
        std::uint32_t entropy
    )
    {
        std::uint32_t x =
            g_skyDoomPickupRandomStateV15_3;

        x ^=
            entropy +
            static_cast<std::uint32_t>(
                GetTickCount64()
            ) +
            0x9E3779B9u;

        if (x == 0u)
        {
            x =
                0xA341316Cu;
        }

        x ^=
            x <<
            13;

        x ^=
            x >>
            17;

        x ^=
            x <<
            5;

        if (x == 0u)
        {
            x =
                0xC8013EA4u;
        }

        g_skyDoomPickupRandomStateV15_3 =
            x;

        return x;
    }

    std::uint16_t ChooseSkyDoomResourcePickupV15_3(
        std::uint32_t entropy
    )
    {
        if (!g_state)
        {
            return SKYDOOM_PICKUP_NONE;
        }

        struct Candidate
        {
            std::uint16_t type;
            std::uint32_t weight;
        };

        const std::int32_t health =
            g_state->
                doom.
                health;

        /*
            Normal healing stays capped at 100:
              70% Stimpack / 30% Medikit within that category.
        */
        const std::uint32_t totalBaseHealthWeight =
            SkyDoomPickupNeedWeightV15_3(
                health,
                100
            );

        const std::uint32_t medikitWeight =
            (
                totalBaseHealthWeight *
                3u
            ) /
            10u;

        const std::uint32_t stimpackWeight =
            totalBaseHealthWeight -
            medikitWeight;

        /*
            Health Bonus and Soulsphere can matter above 100.
            These are provisional weights pending the balance pass.
        */
        const std::uint32_t totalOverhealNeedWeight =
            SkyDoomPickupNeedWeightV15_3(
                health,
                200
            );

        std::uint32_t healthBonusWeight =
            0u;

        std::uint32_t soulsphereWeight =
            0u;

        if (health < 200)
        {
            healthBonusWeight =
                totalOverhealNeedWeight /
                8u;

            if (healthBonusWeight == 0u)
            {
                healthBonusWeight =
                    1u;
            }

            soulsphereWeight =
                totalOverhealNeedWeight /
                60u;

            if (soulsphereWeight == 0u)
            {
                soulsphereWeight =
                    1u;
            }
        }

        const std::int32_t armor =
            g_state->
                doom.
                armor;

        std::uint32_t greenArmorWeight =
            0u;

        std::uint32_t blueArmorWeight =
            0u;

        std::uint32_t armorBonusWeight =
            0u;

        if (armor < 100)
        {
            const std::uint32_t totalArmorWeight =
                SkyDoomPickupNeedWeightV15_3(
                    armor,
                    100
                );

            greenArmorWeight =
                totalArmorWeight /
                10u;

            blueArmorWeight =
                totalArmorWeight /
                50u;

            armorBonusWeight =
                totalArmorWeight -
                greenArmorWeight -
                blueArmorWeight;
        }
        else if (armor < 200)
        {
            const std::uint32_t totalArmorWeight =
                SkyDoomPickupNeedWeightV15_3(
                    armor,
                    200
                ) /
                4u;

            blueArmorWeight =
                totalArmorWeight /
                20u;

            armorBonusWeight =
                totalArmorWeight -
                blueArmorWeight;
        }

        /*
            Read Doom's live limits.  Fall back to vanilla base values only
            during the tiny startup window before Chocolate Doom publishes.
        */
        const std::int32_t maxBullets =
            g_state->doom.maxammo_bullets > 0 ?
                g_state->doom.maxammo_bullets :
                200;

        const std::int32_t maxShells =
            g_state->doom.maxammo_shells > 0 ?
                g_state->doom.maxammo_shells :
                50;

        const std::int32_t maxRockets =
            g_state->doom.maxammo_rockets > 0 ?
                g_state->doom.maxammo_rockets :
                50;

        const std::int32_t maxCells =
            g_state->doom.maxammo_cells > 0 ?
                g_state->doom.maxammo_cells :
                300;

        const std::uint32_t totalBulletWeight =
            SkyDoomPickupNeedWeightV15_3(
                g_state->doom.ammo_bullets,
                maxBullets
            );

        const std::uint32_t bulletBoxWeight =
            totalBulletWeight /
                5u;

        const std::uint32_t bulletWeight =
            totalBulletWeight -
                bulletBoxWeight;

        const std::uint32_t totalShellWeight =
            SkyDoomPickupNeedWeightV15_3(
                g_state->doom.ammo_shells,
                maxShells
            );

        const std::uint32_t shellBoxWeight =
            totalShellWeight /
                5u;

        const std::uint32_t shellWeight =
            totalShellWeight -
                shellBoxWeight;

        const std::uint32_t totalRocketWeight =
            SkyDoomPickupNeedWeightV15_3(
                g_state->doom.ammo_rockets,
                maxRockets
            );

        const std::uint32_t rocketBoxWeight =
            totalRocketWeight /
                5u;

        const std::uint32_t rocketWeight =
            totalRocketWeight -
                rocketBoxWeight;

        const std::uint32_t totalCellWeight =
            SkyDoomPickupNeedWeightV15_3(
                g_state->doom.ammo_cells,
                maxCells
            );

        const std::uint32_t cellPackWeight =
            totalCellWeight /
                5u;

        const std::uint32_t cellWeight =
            totalCellWeight -
                cellPackWeight;

        const bool hasBackpack =
            g_state->
                doom.
                has_backpack !=
                    0u;

        const bool anyAmmoMissing =
            g_state->doom.ammo_bullets < maxBullets ||
            g_state->doom.ammo_shells < maxShells ||
            g_state->doom.ammo_rockets < maxRockets ||
            g_state->doom.ammo_cells < maxCells;

        /*
            First Backpack is valuable even with full ammo because it doubles
            capacity. Later Backpacks remain eligible as authentic ammo bundles.
            Provisional rarity only.
        */
        const std::uint32_t backpackWeight =
            !hasBackpack ?
                8u :
                (
                    anyAmmoMissing ?
                        2u :
                        0u
                );

        /*
            Doom II/commercial only. Do not create an item Chocolate Doom
            would have to reject forever in Doom I mode.
        */
        const std::uint32_t megasphereWeight =
            (
                g_state->doom.commercial_mode != 0u &&
                (
                    health < 200 ||
                    armor < 200
                )
            ) ?
                2u :
                0u;

        const std::array<Candidate, 17>
            candidates{{
                {
                    SKYDOOM_PICKUP_STIMPACK,
                    stimpackWeight
                },
                {
                    SKYDOOM_PICKUP_MEDIKIT,
                    medikitWeight
                },
                {
                    SKYDOOM_PICKUP_HEALTH_BONUS,
                    healthBonusWeight
                },
                {
                    SKYDOOM_PICKUP_SOULSPHERE,
                    soulsphereWeight
                },
                {
                    SKYDOOM_PICKUP_ARMOR_BONUS,
                    armorBonusWeight
                },
                {
                    SKYDOOM_PICKUP_ARMOR,
                    greenArmorWeight
                },
                {
                    SKYDOOM_PICKUP_BLUE_ARMOR,
                    blueArmorWeight
                },
                {
                    SKYDOOM_PICKUP_BULLETS,
                    bulletWeight
                },
                {
                    SKYDOOM_PICKUP_BULLET_BOX,
                    bulletBoxWeight
                },
                {
                    SKYDOOM_PICKUP_SHELLS,
                    shellWeight
                },
                {
                    SKYDOOM_PICKUP_SHELL_BOX,
                    shellBoxWeight
                },
                {
                    SKYDOOM_PICKUP_ROCKET,
                    rocketWeight
                },
                {
                    SKYDOOM_PICKUP_ROCKET_BOX,
                    rocketBoxWeight
                },
                {
                    SKYDOOM_PICKUP_CELLS,
                    cellWeight
                },
                {
                    SKYDOOM_PICKUP_CELL_PACK,
                    cellPackWeight
                },
                {
                    SKYDOOM_PICKUP_BACKPACK,
                    backpackWeight
                },
                {
                    SKYDOOM_PICKUP_MEGASPHERE,
                    megasphereWeight
                }
            }};

        std::uint32_t totalWeight =
            0u;

        for (
            const auto& candidate :
                candidates
        )
        {
            totalWeight +=
                candidate.weight;
        }

        if (totalWeight == 0u)
        {
            return SKYDOOM_PICKUP_NONE;
        }

        std::uint32_t roll =
            SkyDoomNextPickupRandomV15_3(
                entropy
            ) %
            totalWeight;

        for (
            const auto& candidate :
                candidates
        )
        {
            if (
                roll <
                candidate.weight
            )
            {
                return candidate.type;
            }

            roll -=
                candidate.weight;
        }

        return SKYDOOM_PICKUP_STIMPACK;
    }









    void SpawnSkyDoomResourcePickupV15_3(
        std::uint16_t pickupType,
        const RE::NiPoint3& groundPosition,
        float doomToSkyrimScale
    )
    {
        if (
            pickupType <
                SKYDOOM_PICKUP_MEDIKIT ||
            pickupType > SKYDOOM_PICKUP_MEGASPHERE
        )
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        std::lock_guard<std::mutex>
            lock(
                g_skyDoomPhysicalPickupMutexV15
            );

        SkyDoomPhysicalPickupV15*
            selected =
                nullptr;

        for (
            auto& pickup :
                g_skyDoomPhysicalPickupsV15
        )
        {
            if (!pickup.active)
            {
                selected =
                    &pickup;

                break;
            }
        }

        if (!selected)
        {
            selected =
                &g_skyDoomPhysicalPickupsV15[0];

            for (
                auto& pickup :
                    g_skyDoomPhysicalPickupsV15
            )
            {
                if (
                    pickup.spawnMs <
                    selected->
                        spawnMs
                )
                {
                    selected =
                        &pickup;
                }
            }

            SKSE::log::warn(
                "[skydoomskse] physical pickup pool full - replacing oldest pickup"
            );
        }

        selected->active =
            true;

        selected->awaitingResult =
            false;

        selected->blockedByHealth =
            false;

        selected->requestId =
            0;

        selected->blockedHealth =
            0;

        selected->pickupType =
            pickupType;

        selected->groundPosition =
            groundPosition;

        selected->doomToSkyrimScale =
            doomToSkyrimScale;

        selected->spawnMs =
            now;

        selected->requestMs =
            0u;

        SKSE::log::info(
            "[skydoomskse] PHYSICAL DOOM PICKUP spawned: {} position=({}, {}, {}) scale={}",
            SkyDoomPickupNameV15_3(
                pickupType
            ),
            groundPosition.x,
            groundPosition.y,
            groundPosition.z,
            doomToSkyrimScale
        );
    }

    void SpawnSkyDoomMedikitPickupV15(
        const RE::NiPoint3& groundPosition,
        float doomToSkyrimScale
    )
    {
        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        std::lock_guard<std::mutex>
            lock(
                g_skyDoomPhysicalPickupMutexV15
            );

        SkyDoomPhysicalPickupV15*
            selected =
                nullptr;

        for (
            auto& pickup :
                g_skyDoomPhysicalPickupsV15
        )
        {
            if (!pickup.active)
            {
                selected =
                    &pickup;

                break;
            }
        }

        if (!selected)
        {
            selected =
                &g_skyDoomPhysicalPickupsV15[0];

            for (
                auto& pickup :
                    g_skyDoomPhysicalPickupsV15
            )
            {
                if (
                    pickup.spawnMs <
                    selected->
                        spawnMs
                )
                {
                    selected =
                        &pickup;
                }
            }

            SKSE::log::warn(
                "[skydoomskse] physical pickup pool full - replacing oldest pickup"
            );
        }

        selected->active =
            true;

        selected->awaitingResult =
            false;

        selected->blockedByHealth =
            false;

        selected->requestId =
            0;

        selected->blockedHealth =
            0;

        selected->pickupType =
            SKYDOOM_PICKUP_MEDIKIT;

        selected->groundPosition =
            groundPosition;

        selected->doomToSkyrimScale =
            doomToSkyrimScale;

        selected->spawnMs =
            now;

        selected->requestMs =
            0u;

        SKSE::log::info(
            "[skydoomskse] PHYSICAL DOOM PICKUP spawned: MEDIKIT position=({}, {}, {}) scale={}",
            groundPosition.x,
            groundPosition.y,
            groundPosition.z,
            doomToSkyrimScale
        );
    }

    void ResolveSkyDoomPickupResultV15(
        std::int32_t requestId,
        std::int32_t accepted,
        std::int32_t resourceAfter
    )
    {
        std::lock_guard<std::mutex>
            lock(
                g_skyDoomPhysicalPickupMutexV15
            );

        for (
            auto& pickup :
                g_skyDoomPhysicalPickupsV15
        )
        {
            if (
                !pickup.active ||
                !pickup.awaitingResult ||
                pickup.requestId !=
                    requestId
            )
            {
                continue;
            }

            pickup.awaitingResult =
                false;

            pickup.requestId =
                0;

            if (accepted != 0)
            {
                SKSE::log::info(
                    "[skydoomskse] PHYSICAL DOOM PICKUP collected: {} requestId={} resourceAfter={}",
                    SkyDoomPickupNameV15_3(
                        pickup.pickupType
                    ),
                    requestId,
                    resourceAfter
                );

                pickup.active =
                    false;
            }
            else
            {
                /*
                    Keep a rejected item in the world.  Re-arm it
                    only after the corresponding resource falls,
                    preventing a full-resource request every frame.
                */
                pickup.blockedByHealth =
                    true;

                pickup.blockedHealth =
                    resourceAfter;

                SKSE::log::info(
                    "[skydoomskse] PHYSICAL DOOM PICKUP rejected/left in world: {} requestId={} resourceAfter={}",
                    SkyDoomPickupNameV15_3(
                        pickup.pickupType
                    ),
                    requestId,
                    resourceAfter
                );
            }

            return;
        }

        SKSE::log::warn(
            "[skydoomskse] pickup result has no pending world pickup: requestId={} accepted={} resourceAfter={}",
            requestId,
            accepted,
            resourceAfter
        );
    }

    void ProcessSkyDoomPhysicalPickupsV15(
        bool doomFresh
    )
    {
        if (
            !doomFresh ||
            !g_state ||
            !g_state->
                doom.
                running ||
            !g_state->
                doom.
                in_level ||
            !g_state->
                skyrim.
                in_game ||
            g_state->
                skyrim.
                paused
        )
        {
            return;
        }

        auto* player =
            RE::PlayerCharacter::
                GetSingleton();

        auto* processLists =
            RE::ProcessLists::
                GetSingleton();

        if (
            !player ||
            !processLists
        )
        {
            return;
        }

        const auto now =
            static_cast<std::uint64_t>(
                GetTickCount64()
            );

        const float scale =
            GetSkyDoomDoomToSkyrimScale(
                player
            );

        /*
            Death transition scan.

            New actors are INITIALISED to their current dead state,
            which prevents pre-existing corpses from generating
            pickups merely because a cell was loaded.
        */
        for (
            auto& handle :
                processLists->
                    highActorHandles
        )
        {
            auto actorPtr =
                handle.
                    get();

            auto* actor =
                actorPtr.
                    get();

            if (
                !actor ||
                actor ==
                    player ||
                actor->
                    IsDisabled() ||
                !actor->
                    Is3DLoaded() ||
                actor->
                    IsGhost()
            )
            {
                continue;
            }

            const std::uint32_t formId =
                actor->
                    GetFormID();

            if (formId == 0u)
            {
                continue;
            }

            const bool isDead =
                actor->
                    IsDead();

            SkyDoomTrackedDeathActorV15*
                tracked =
                    nullptr;

            SkyDoomTrackedDeathActorV15*
                oldest =
                    &g_skyDoomTrackedDeathActorsV15[0];

            for (
                auto& candidate :
                    g_skyDoomTrackedDeathActorsV15
            )
            {
                if (
                    candidate.active &&
                    candidate.formId ==
                        formId
                )
                {
                    tracked =
                        &candidate;

                    break;
                }

                if (!candidate.active)
                {
                    oldest =
                        &candidate;
                }
                else if (
                    oldest->active &&
                    candidate.lastSeenMs <
                        oldest->lastSeenMs
                )
                {
                    oldest =
                        &candidate;
                }
            }

            if (!tracked)
            {
                tracked =
                    oldest;

                tracked->active =
                    true;

                tracked->formId =
                    formId;

                tracked->wasDead =
                    isDead;

                tracked->lastSeenMs =
                    now;

                continue;
            }

            tracked->lastSeenMs =
                now;

            if (
                !tracked->wasDead &&
                isDead
            )
            {
                const std::uint16_t
                    pickupType =
                        ChooseSkyDoomResourcePickupV15_3(
                            formId
                        );

                if (
                    pickupType !=
                    SKYDOOM_PICKUP_NONE
                )
                {
                    SpawnSkyDoomResourcePickupV15_3(
                        pickupType,
                        actor->
                            GetPosition(),
                        scale
                    );
                }
                else
                {
                    SKSE::log::info(
                        "[skydoomskse] PHYSICAL DOOM PICKUP skipped: all tracked Doom resources are full"
                    );
                }
            }

            tracked->wasDead =
                isDead;
        }

        /*
            Proximity collection.

            Doom pickup radius is intentionally a little forgiving
            in Skyrim space. A short 500 ms arming delay ensures a
            close-range kill still gives the sprite time to appear.
        */
        const RE::NiPoint3 playerPosition =
            player->
                GetPosition();

        std::lock_guard<std::mutex>
            lock(
                g_skyDoomPhysicalPickupMutexV15
            );

        for (
            auto& pickup :
                g_skyDoomPhysicalPickupsV15
        )
        {
            if (!pickup.active)
            {
                continue;
            }

            if (
                pickup.awaitingResult
            )
            {
                if (
                    now >=
                        pickup.requestMs &&
                    now -
                        pickup.requestMs >
                        2500u
                )
                {
                    SKSE::log::warn(
                        "[skydoomskse] pickup request timed out: requestId={}",
                        pickup.requestId
                    );

                    pickup.awaitingResult =
                        false;

                    pickup.requestId =
                        0;
                }

                continue;
            }

            if (
                pickup.blockedByHealth
            )
            {
                const std::int32_t
                    currentResource =
                        SkyDoomPickupResourceValueV15_3(
                            pickup.pickupType
                        );

                if (
                    currentResource <
                    pickup.blockedHealth
                )
                {
                    pickup.blockedByHealth =
                        false;
                }
                else
                {
                    continue;
                }
            }

            if (
                now <
                    pickup.spawnMs ||
                now -
                    pickup.spawnMs <
                    500u
            )
            {
                continue;
            }

            const float dx =
                playerPosition.x -
                pickup.
                    groundPosition.x;

            const float dy =
                playerPosition.y -
                pickup.
                    groundPosition.y;

            const float dz =
                playerPosition.z -
                pickup.
                    groundPosition.z;

            const float collectRadius =
                42.0f *
                pickup.
                    doomToSkyrimScale;

            const float verticalAllowance =
                64.0f *
                pickup.
                    doomToSkyrimScale;

            if (
                dx * dx +
                dy * dy >
                    collectRadius *
                    collectRadius ||
                std::fabs(
                    dz
                ) >
                    verticalAllowance
            )
            {
                continue;
            }

            if (
                g_skyDoomNextPickupRequestIdV15 <=
                    0 ||
                g_skyDoomNextPickupRequestIdV15 >
                    1000000000
            )
            {
                g_skyDoomNextPickupRequestIdV15 =
                    1;
            }

            const std::int32_t requestId =
                g_skyDoomNextPickupRequestIdV15++;

            pickup.awaitingResult =
                true;

            pickup.requestId =
                requestId;

            pickup.requestMs =
                now;

            PushInputEvent(
                SKYDOOM_INPUT_EVENT_PICKUP,
                pickup.pickupType,
                requestId
            );

            SKSE::log::info(
                "[skydoomskse] PHYSICAL DOOM PICKUP touch: {} requestId={} currentResource={}",
                SkyDoomPickupNameV15_3(
                    pickup.pickupType
                ),
                requestId,
                SkyDoomPickupResourceValueV15_3(
                    pickup.pickupType
                )
            );
        }
    }

    // SKYDOOM_PICKUP_RENDER_ORDER_FIX_V15_R4
    // Renderer intentionally lives after pickup state/processing declarations.

    // SKYDOOM_PICKUP_POSITION_FIX_V15_R5
    void DrawSkyDoomPhysicalPickupsV15()

    {

        if (

            !g_state ||

            !g_state->

                doom.

                running ||

            !g_state->

                doom.

                in_level ||

            !g_state->

                skyrim.

                in_game ||

            g_state->

                skyrim.

                paused

        )

        {

            return;

        }





        if (

            !g_context ||

            !g_backBufferRTV ||

            !g_vertexBuffer ||

            !g_vertexShader ||

            !g_pixelShader ||

            !g_inputLayout ||

            !g_pointSampler ||

            !g_alphaBlend ||

            !g_depthDisabled ||

            !g_rasterizer ||

            g_backBufferWidth ==

                0 ||

            g_backBufferHeight ==

                0

        )

        {

            return;

        }





        if (

            !EnsureSkyDoomRocketExplosionTextures()

        )

        {

            return;

        }





        auto* worldCamera =

            RE::Main::

                WorldRootCamera();





        auto* playerCamera =

            RE::PlayerCamera::

                GetSingleton();





        if (

            !worldCamera ||

            !playerCamera ||

            !playerCamera->

                cameraRoot

        )

        {

            return;

        }





        std::array<

            SkyDoomPhysicalPickupV15,

            SKYDOOM_MAX_ACTIVE_PICKUPS_V15>

            pickups{};





        {

            std::lock_guard<std::mutex>

                lock(

                    g_skyDoomPhysicalPickupMutexV15

                );





            pickups =

                g_skyDoomPhysicalPickupsV15;

        }





        





        bool haveExplosion =

            false;





        for (

            const auto& pickup :

                pickups

        )

        {

            if (

                pickup.active

            )

            {

                haveExplosion =

                    true;



                break;

            }

        }





        if (!haveExplosion)

        {

            return;

        }





        ID3D11RenderTargetView*

            renderTarget =

                g_backBufferRTV.

                    Get();





        g_context->

            OMSetRenderTargets(

                1,

                &renderTarget,

                nullptr

            );





        D3D11_VIEWPORT viewport{};





        viewport.TopLeftX =

            0.0f;





        viewport.TopLeftY =

            0.0f;





        viewport.Width =

            static_cast<float>(

                g_backBufferWidth

            );





        viewport.Height =

            static_cast<float>(

                g_backBufferHeight

            );





        viewport.MinDepth =

            0.0f;





        viewport.MaxDepth =

            1.0f;





        g_context->

            RSSetViewports(

                1,

                &viewport

            );





        const UINT stride =

            sizeof(

                OverlayVertex

            );





        const UINT offset =

            0;





        ID3D11Buffer*

            vertexBuffer =

                g_vertexBuffer.

                    Get();





        g_context->

            IASetInputLayout(

                g_inputLayout.

                    Get()

            );





        g_context->

            IASetVertexBuffers(

                0,

                1,

                &vertexBuffer,

                &stride,

                &offset

            );





        g_context->

            IASetPrimitiveTopology(

                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST

            );





        g_context->

            VSSetShader(

                g_vertexShader.

                    Get(),

                nullptr,

                0

            );





        g_context->

            PSSetShader(

                g_pixelShader.

                    Get(),

                nullptr,

                0

            );





        ID3D11SamplerState*

            sampler =

                g_pointSampler.

                    Get();





        g_context->

            PSSetSamplers(

                0,

                1,

                &sampler

            );





        const float blendFactor[

            4

        ]{

            0.0f,

            0.0f,

            0.0f,

            0.0f

        };





        g_context->

            OMSetBlendState(

                g_alphaBlend.

                    Get(),

                blendFactor,

                0xFFFFFFFFu

            );





        g_context->

            OMSetDepthStencilState(

                g_depthDisabled.

                    Get(),

                0

            );





        g_context->

            RSSetState(

                g_rasterizer.

                    Get()

            );





        RE::NiPoint3 cameraUp =

            playerCamera->

                cameraRoot->

                world.

                rotate *

            RE::NiPoint3{

                0.0f,

                0.0f,

                1.0f

            };





        if (

            cameraUp.

                Unitize() <=

            0.0f

        )

        {

            cameraUp =

                RE::NiPoint3{

                    0.0f,

                    0.0f,

                    1.0f

                };

        }





        for (

            const auto& pickup :

                pickups

        )

        {

            if (!pickup.active)
            {
                continue;
            }

            // SKYDOOM_PICKUP_GROUND_OCCLUSION_V15_1
            //
            // Anchor the BOTTOM of the Doom sprite directly to
            // groundPosition.  The original proof projected the
            // centre and reconstructed the bottom from camera-up,
            // which could visibly float as the view angle changed.
            //
            // Because these overlay sprites intentionally use the
            // depth-disabled render state, do a real Skyrim/Havok
            // camera -> pickup visibility ray before drawing.
            std::uint32_t frameIndex =
                22u;

            switch (pickup.pickupType)
            {
                case SKYDOOM_PICKUP_MEDIKIT:
                    frameIndex =
                        22u;
                    break;

                case SKYDOOM_PICKUP_ARMOR:
                    frameIndex =
                        23u;
                    break;

                case SKYDOOM_PICKUP_BULLETS:
                    frameIndex =
                        24u;
                    break;

                case SKYDOOM_PICKUP_SHELLS:
                    frameIndex =
                        25u;
                    break;

                case SKYDOOM_PICKUP_ROCKET:
                    frameIndex =
                        26u;
                    break;

                case SKYDOOM_PICKUP_CELLS:
                    frameIndex =
                        27u;
                    break;

                case SKYDOOM_PICKUP_STIMPACK:
                    frameIndex =
                        32u;
                    break;

                case SKYDOOM_PICKUP_BLUE_ARMOR:
                    frameIndex =
                        33u;
                    break;

                case SKYDOOM_PICKUP_BULLET_BOX:
                    frameIndex =
                        34u;
                    break;

                case SKYDOOM_PICKUP_SHELL_BOX:
                    frameIndex =
                        35u;
                    break;

                case SKYDOOM_PICKUP_ROCKET_BOX:
                    frameIndex =
                        36u;
                    break;

                case SKYDOOM_PICKUP_CELL_PACK:
                    frameIndex =
                        37u;
                    break;

                case SKYDOOM_PICKUP_ARMOR_BONUS:
                {
                    const std::uint64_t elapsedMs =
                        static_cast<std::uint64_t>(
                            GetTickCount64()
                        ) -
                        pickup.spawnMs;

                    const std::uint64_t doomTics =
                        (
                            elapsedMs *
                            35u
                        ) /
                        1000u;

                    const std::uint32_t phase =
                        static_cast<std::uint32_t>(
                            (
                                doomTics /
                                6u
                            ) %
                            6u
                        );

                    static constexpr
                        std::array<
                            std::uint32_t,
                            6>
                        armorBonusFrames{
                            28u,
                            29u,
                            30u,
                            31u,
                            30u,
                            29u
                        };

                    frameIndex =
                        armorBonusFrames[
                            phase
                        ];

                    break;
                }

                case SKYDOOM_PICKUP_HEALTH_BONUS:
                {
                    const std::uint64_t elapsedMs =
                        static_cast<std::uint64_t>(
                            GetTickCount64()
                        ) -
                        pickup.spawnMs;

                    const std::uint64_t doomTics =
                        (
                            elapsedMs *
                            35u
                        ) /
                        1000u;

                    const std::uint32_t phase =
                        static_cast<std::uint32_t>(
                            (
                                doomTics /
                                6u
                            ) %
                            6u
                        );

                    static constexpr
                        std::array<
                            std::uint32_t,
                            6>
                        healthBonusFrames{
                            38u,
                            39u,
                            40u,
                            41u,
                            40u,
                            39u
                        };

                    frameIndex =
                        healthBonusFrames[
                            phase
                        ];

                    break;
                }

                case SKYDOOM_PICKUP_SOULSPHERE:
                {
                    const std::uint64_t elapsedMs =
                        static_cast<std::uint64_t>(
                            GetTickCount64()
                        ) -
                        pickup.spawnMs;

                    const std::uint64_t doomTics =
                        (
                            elapsedMs *
                            35u
                        ) /
                        1000u;

                    const std::uint32_t phase =
                        static_cast<std::uint32_t>(
                            (
                                doomTics /
                                6u
                            ) %
                            6u
                        );

                    static constexpr
                        std::array<
                            std::uint32_t,
                            6>
                        soulsphereFrames{
                            42u,
                            43u,
                            44u,
                            45u,
                            44u,
                            43u
                        };

                    frameIndex =
                        soulsphereFrames[
                            phase
                        ];

                    break;
                }

                case SKYDOOM_PICKUP_BACKPACK:
                    frameIndex =
                        46u;
                    break;

                case SKYDOOM_PICKUP_MEGASPHERE:
                {
                    const std::uint64_t elapsedMs =
                        static_cast<std::uint64_t>(
                            GetTickCount64()
                        ) -
                        pickup.spawnMs;

                    const std::uint64_t doomTics =
                        (
                            elapsedMs *
                            35u
                        ) /
                        1000u;

                    const std::uint32_t phase =
                        static_cast<std::uint32_t>(
                            (
                                doomTics /
                                6u
                            ) %
                            4u
                        );

                    static constexpr
                        std::array<
                            std::uint32_t,
                            4>
                        megasphereFrames{
                            47u,
                            48u,
                            49u,
                            50u
                        };

                    frameIndex =
                        megasphereFrames[
                            phase
                        ];

                    break;
                }

                default:
                    continue;
            }

            const float fullWorldHeight =
                static_cast<float>(
                    g_skyDoomRocketExplosionHeights[
                        frameIndex
                    ]
                ) *
                pickup.
                    doomToSkyrimScale;

            const RE::NiPoint3 groundWorld =
                pickup.
                    groundPosition;

            RE::NiPoint3 topWorld =
                groundWorld;

            topWorld.z +=
                fullWorldHeight;

            RE::NiPoint3 visibilityTarget =
                groundWorld;

            visibilityTarget.z +=
                fullWorldHeight *
                0.5f;

            const RE::NiPoint3 cameraWorld =
                playerCamera->
                    cameraRoot->
                    world.
                    translate;

            // SKYDOOM_PICKUP_BEHIND_CAMERA_CULL_V15_2
            //
            // Skyrim's world->screen helper can still return a
            // drawable projection for points behind the camera.
            // Use the exact +Y camera-forward convention already
            // proven by SkyDoom's plasma/BFG projectile code and
            // reject pickups behind the camera plane before any
            // overlay projection occurs.
            RE::NiPoint3 cameraForward =
                playerCamera->
                    cameraRoot->
                    world.
                    rotate *
                RE::NiPoint3{
                    0.0f,
                    1.0f,
                    0.0f
                };

            if (
                cameraForward.
                    Unitize() <=
                0.0f
            )
            {
                continue;
            }

            const RE::NiPoint3 toPickup{
                visibilityTarget.x -
                    cameraWorld.x,
                visibilityTarget.y -
                    cameraWorld.y,
                visibilityTarget.z -
                    cameraWorld.z
            };

            const float pickupForwardDot =
                toPickup.x *
                    cameraForward.x +
                toPickup.y *
                    cameraForward.y +
                toPickup.z *
                    cameraForward.z;

            if (
                pickupForwardDot <=
                    0.0f
            )
            {
                continue;
            }

            RE::TESObjectREFR*
                blockerRef =
                    nullptr;

            float blockerFraction =
                1.0f;

            if (
                SkyDoomWorldPick(
                    cameraWorld,
                    visibilityTarget,
                    blockerRef,
                    blockerFraction
                ) &&
                blockerFraction <
                    0.985f
            )
            {
                /*
                    A corpse at the pickup position can legitimately
                    be the first Havok hit.  Do not let that corpse
                    hide its own drop.  Static geometry and living
                    actors still occlude the pickup.
                */
                auto* blockerActor =
                    FindSkyDoomActorByReference(
                        blockerRef
                    );

                if (
                    !blockerActor ||
                    !blockerActor->
                        IsDead()
                )
                {
                    continue;
                }
            }

            float groundScreenX =
                0.0f;

            float groundScreenY =
                0.0f;

            float groundScreenZ =
                0.0f;

            if (
                !worldCamera->
                    WorldPtToScreenPt3(
                        groundWorld,
                        groundScreenX,
                        groundScreenY,
                        groundScreenZ,
                        0.00001f
                    )
            )
            {
                continue;
            }

            if (
                groundScreenX <
                    -0.35f ||
                groundScreenX >
                    1.35f ||
                groundScreenY <
                    -0.35f ||
                groundScreenY >
                    1.35f
            )
            {
                continue;
            }

            float topX =
                0.0f;

            float topY =
                0.0f;

            float topZ =
                0.0f;

            if (
                !worldCamera->
                    WorldPtToScreenPt3(
                        topWorld,
                        topX,
                        topY,
                        topZ,
                        0.00001f
                    )
            )
            {
                continue;
            }

            const float groundPixelX =
                groundScreenX *
                static_cast<float>(
                    g_backBufferWidth
                );

            const float groundPixelY =
                (
                    1.0f -
                    groundScreenY
                ) *
                static_cast<float>(
                    g_backBufferHeight
                );

            const float projectedTopPixelX =
                topX *
                static_cast<float>(
                    g_backBufferWidth
                );

            const float projectedTopPixelY =
                (
                    1.0f -
                    topY
                ) *
                static_cast<float>(
                    g_backBufferHeight
                );

            const float projectedDX =
                projectedTopPixelX -
                groundPixelX;

            const float projectedDY =
                projectedTopPixelY -
                groundPixelY;

            float fullHeightPixels =
                std::sqrt(
                    projectedDX *
                        projectedDX +
                    projectedDY *
                        projectedDY
                );

            if (
                fullHeightPixels <
                    8.0f
            )
            {
                fullHeightPixels =
                    8.0f;
            }

            if (
                fullHeightPixels >
                    320.0f
            )
            {
                fullHeightPixels =
                    320.0f;
            }

            const float aspect =
                static_cast<float>(
                    g_skyDoomRocketExplosionWidths[
                        frameIndex
                    ]
                ) /
                static_cast<float>(
                    g_skyDoomRocketExplosionHeights[
                        frameIndex
                    ]
                );

            const float fullWidthPixels =
                fullHeightPixels *
                aspect;

            /*
                Keep the base fixed to the projected ground point.
                Any min/max size clamp therefore grows upward rather
                than lifting the sprite away from the floor.
            */
            const float centrePixelX =
                groundPixelX;

            const float bottomPixel =
                groundPixelY;

            const float topPixel =
                bottomPixel -
                fullHeightPixels;

            const float leftPixel =
                centrePixelX -
                fullWidthPixels *
                0.5f;

            const float rightPixel =
                centrePixelX +
                fullWidthPixels *
                0.5f;

            const float left =

                (

                    leftPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float right =

                (

                    rightPixel /

                    static_cast<float>(

                        g_backBufferWidth

                    )

                ) *

                    2.0f -

                1.0f;





            const float top =

                1.0f -

                (

                    topPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const float bottom =

                1.0f -

                (

                    bottomPixel /

                    static_cast<float>(

                        g_backBufferHeight

                    )

                ) *

                    2.0f;





            const OverlayVertex vertices[

                6

            ]{

                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                },

                {

                    left,

                    bottom,

                    0.0f,

                    0.0f,

                    1.0f

                },



                {

                    left,

                    top,

                    0.0f,

                    0.0f,

                    0.0f

                },

                {

                    right,

                    top,

                    0.0f,

                    1.0f,

                    0.0f

                },

                {

                    right,

                    bottom,

                    0.0f,

                    1.0f,

                    1.0f

                }

            };





            D3D11_MAPPED_SUBRESOURCE mapped{};





            if (

                FAILED(

                    g_context->

                        Map(

                            g_vertexBuffer.

                                Get(),

                            0,

                            D3D11_MAP_WRITE_DISCARD,

                            0,

                            &mapped

                        )

                )

            )

            {

                continue;

            }





            std::memcpy(

                mapped.pData,

                vertices,

                sizeof(

                    vertices

                )

            );





            g_context->

                Unmap(

                    g_vertexBuffer.

                        Get(),

                    0

                );





            ID3D11ShaderResourceView*

                spriteSRV =

                    g_skyDoomRocketExplosionSRVs[

                        frameIndex

                    ].

                    Get();





            g_context->

                PSSetShaderResources(

                    0,

                    1,

                    &spriteSRV

                );





            g_context->

                Draw(

                    6,

                    0

                );

        }

    }



    void ResolveSkyDoomRocketDamageRoll(

        std::int32_t requestId,

        std::int32_t doomDamage

    )

    {

        SkyDoomPendingRocketImpact*

            pending =

                nullptr;





        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_PENDING_ROCKET_IMPACTS;

            ++i

        )

        {

            if (

                g_skyDoomPendingRocketImpacts[i].

                    active &&

                g_skyDoomPendingRocketImpacts[i].

                    requestId ==

                    requestId

            )

            {

                pending =

                    &g_skyDoomPendingRocketImpacts[i];



                break;

            }

        }





        if (!pending)

        {

            SKSE::log::warn(

                "[skydoomskse] REAL DOOM rocket damage response has no pending impact: requestId={} damage={}",

                requestId,

                doomDamage

            );



            return;

        }





        auto* player =

            RE::PlayerCharacter::

                GetSingleton();





        auto* target =

            FindSkyDoomActorByFormId(

                pending->

                    targetFormId

            );





        if (

            player &&

            target &&

            !target->

                IsDead() &&

            !target->

                IsDisabled() &&

            target->

                Is3DLoaded()

        )

        {

            auto* actorValueOwner =

                target->

                    AsActorValueOwner();





            if (actorValueOwner)

            {

                const float healthBefore =

                    actorValueOwner->

                        GetActorValue(

                            RE::ActorValue::

                                kHealth

                        );





                if (healthBefore > 0.0f)

                {

                    const bool

                        usedNativeHitData =

                            ApplySkyDoomNativePistolHit(

                                player,

                                target,

                                static_cast<float>(

                                    doomDamage

                                ),

                                pending->

                                    impactPosition,

                                pending->

                                    direction

                            );





                    if (!usedNativeHitData)

                    {

                        target->

                            DoDamage(

                                static_cast<float>(

                                    doomDamage

                                ),

                                player,

                                true

                            );

                    }





                    const float healthAfter =

                        actorValueOwner->

                            GetActorValue(

                                RE::ActorValue::

                                    kHealth

                            );





                                        QueueSkyDoomRocketKnockbackV11_3(

                        target,

                        pending->

                            direction,

                        doomDamage,

                        healthAfter <=

                            0.0f

                    );



SKSE::log::info(

                        "[skydoomskse] REAL DOOM rocket DIRECT HIT: target={:08X} requestId={} damage={} health={} -> {} path={}",

                        target->

                            GetFormID(),

                        requestId,

                        doomDamage,

                        healthBefore,

                        healthAfter,

                        usedNativeHitData ?

                            "HitData" :

                            "DoDamage"

                    );

                }

            }

        }





        /*

            DOOM applies the direct missile damage first.



            The explosion state then performs A_Explode and its

            radius attack.



            If the direct hit killed the target, the splash loop

            naturally skips its now-dead Actor.

        */



        ApplySkyDoomRocketSplash(

            pending->

                impactPosition,

            pending->

                doomToSkyrimScale

        );





        pending->

            active =

                false;

    }





    void ProcessSkyDoomRockets(

        bool doomFresh

    )

    {

        if (!g_state)

        {

            ClearSkyDoomRocketState();



            return;

        }





        const auto now =

            static_cast<

                std::uint64_t

            >(

                GetTickCount64()

            );





        const bool active =

            doomFresh &&

            g_state->

                doom.

                running &&

            g_state->

                doom.

                in_level &&

            g_state->

                skyrim.

                in_game &&

            !g_state->

                skyrim.

                paused;





        if (!active)

        {

            ClearSkyDoomRocketState();



            return;

        }





        /*

            If a direct-hit RNG response somehow disappears,

            never leave the impact pending forever.



            We still perform the deterministic DOOM splash after

            two seconds, but deliberately omit unproven direct

            damage.

        */



        for (

            std::uint32_t i = 0;

            i <

                SKYDOOM_MAX_PENDING_ROCKET_IMPACTS;

            ++i

        )

        {

            auto& pending =

                g_skyDoomPendingRocketImpacts[i];





            if (

                pending.active &&

                now -

                    pending.createdMs >

                    2000u

            )

            {

                SKSE::log::warn(

                    "[skydoomskse] REAL DOOM rocket damage response timeout: requestId={} - applying splash only",

                    pending.requestId

                );





                ApplySkyDoomRocketSplash(

                    pending.

                        impactPosition,

                    pending.

                        doomToSkyrimScale

                );





                pending.active =

                    false;

            }

        }





        auto* player =

            RE::PlayerCharacter::

                GetSingleton();





        auto* processLists =

            RE::ProcessLists::

                GetSingleton();





        if (

            !player ||

            !processLists

        )

        {

            return;

        }





        for (

            std::uint32_t rocketIndex = 0;

            rocketIndex <

                SKYDOOM_MAX_ACTIVE_ROCKETS;

            ++rocketIndex

        )

        {

            auto& rocket =

                g_skyDoomRockets[

                    rocketIndex

                ];





            if (!rocket.active)

            {

                continue;

            }





            if (

                now -

                    rocket.bornMs >

                    8000u

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM rocket expired after 8 seconds"

                );





                rocket.active =

                    false;



                continue;

            }





            if (

                now <=

                rocket.lastUpdateMs

            )

            {

                continue;

            }





            float dt =

                static_cast<float>(

                    now -

                    rocket.lastUpdateMs

                ) /

                1000.0f;





            rocket.lastUpdateMs =

                now;





            /*

                Never allow one delayed Skyrim task to create an

                enormous tunnelling segment.

            */



            if (dt > 0.10f)

            {

                dt =

                    0.10f;

            }





            if (dt <= 0.0f)

            {

                continue;

            }





            const float speed =

                700.0f *

                rocket.

                    doomToSkyrimScale;





            const float segmentLength =

                speed *

                dt;





            if (segmentLength <= 0.0f)

            {

                continue;

            }





            const RE::NiPoint3

                segmentStart =

                    rocket.

                        position;





            const RE::NiPoint3

                segmentEnd =

                    segmentStart +

                    rocket.

                        direction *

                    segmentLength;





            const float rocketRadius =

                11.0f *

                rocket.

                    doomToSkyrimScale;





            const float rocketHalfHeight =

                4.0f *

                rocket.

                    doomToSkyrimScale;





            RE::Actor*

                nearestActor =

                    nullptr;





            float nearestActorDistance =

                segmentLength +

                1.0f;





            for (

                auto& handle :

                    processLists->

                        highActorHandles

            )

            {

                auto actorPtr =

                    handle.

                        get();





                auto* actor =

                    actorPtr.

                        get();





                if (

                    !actor ||

                    actor ==

                        player ||

                    actor->

                        IsDisabled() ||

                    !actor->

                        Is3DLoaded() ||

                    actor->

                        IsGhost() ||

                    actor->

                        IsDead()

                )

                {

                    continue;

                }





                float hitDistance =

                    0.0f;





                if (

                    !SkyDoomSegmentHitsActor(

                        segmentStart,

                        rocket.

                            direction,

                        segmentLength,

                        actor,

                        rocketRadius,

                        rocketHalfHeight,

                        hitDistance

                    )

                )

                {

                    continue;

                }





                if (

                    hitDistance <

                    nearestActorDistance

                )

                {

                    nearestActorDistance =

                        hitDistance;





                    nearestActor =

                        actor;

                }

            }





            RE::TESObjectREFR*

                blockerRef =

                    nullptr;





            float geometryFraction =

                1.0f;





            const bool worldHit =

                SkyDoomWorldPick(

                    segmentStart,

                    segmentEnd,

                    blockerRef,

                    geometryFraction

                );





            float worldHitDistance =

                segmentLength +

                1.0f;





            if (worldHit)

            {

                worldHitDistance =

                    geometryFraction *

                    segmentLength;

            }





            /*

                If Havok's first collision is itself an Actor,

                treat it as the direct target.



                The mathematical AABB path remains as the fallback

                because it has already proved reliable for our

                bullet weapons.

            */



            RE::Actor*

                pickedActor =

                    nullptr;





            if (

                worldHit &&

                blockerRef &&

                blockerRef !=

                    player

            )

            {

                pickedActor =

                    FindSkyDoomActorByReference(

                        blockerRef

                    );





                if (

                    pickedActor &&

                    (

                        pickedActor->

                            IsDead() ||

                        pickedActor->

                            IsDisabled() ||

                        !pickedActor->

                            Is3DLoaded()

                    )

                )

                {

                    pickedActor =

                        nullptr;

                }

            }





            if (pickedActor)

            {

                nearestActor =

                    pickedActor;





                nearestActorDistance =

                    worldHitDistance;

            }





            bool actorWins =

                false;





            if (nearestActor)

            {

                if (!worldHit)

                {

                    actorWins =

                        true;

                }

                else if (

                    pickedActor ==

                    nearestActor

                )

                {

                    actorWins =

                        true;

                }

                else if (

                    nearestActorDistance <=

                    worldHitDistance +

                        2.0f

                )

                {

                    actorWins =

                        true;

                }

            }





            if (actorWins)

            {

                float impactDistance =

                    nearestActorDistance -

                    2.0f;





                if (impactDistance < 0.0f)

                {

                    impactDistance =

                        0.0f;

                }





                const RE::NiPoint3

                    impactPosition =

                        segmentStart +

                        rocket.

                            direction *

                        impactDistance;





                rocket.active =

                    false;





                StartSkyDoomRocketExplosionVisual(

                    impactPosition,

                    rocket.

                        doomToSkyrimScale

                );



                // SKYDOOM_ROCKET_EXPLOSION_SOUND_NOTIFY_V13_1

                PushInputEvent(

                    SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,

                    SKYDOOM_COMBAT_WEAPON_ROCKET,

                    0

                );



                QueueSkyDoomRocketDirectImpact(

                    nearestActor,

                    impactPosition,

                    rocket.

                        direction,

                    rocket.

                        doomToSkyrimScale

                );





                continue;

            }





            if (worldHit)

            {

                /*

                    The player's own controller should not normally

                    be reachable because launch begins forward of

                    the camera. If it is the first Havok result,

                    ignore this one small step rather than making

                    the rocket explode inside its owner.

                */



                if (

                    blockerRef ==

                    player

                )

                {

                    rocket.position =

                        segmentEnd;



                    continue;

                }





                float impactDistance =

                    worldHitDistance -

                    2.0f;





                if (impactDistance < 0.0f)

                {

                    impactDistance =

                        0.0f;

                }





                const RE::NiPoint3

                    impactPosition =

                        segmentStart +

                        rocket.

                            direction *

                        impactDistance;





                rocket.active =

                    false;

                StartSkyDoomRocketExplosionVisual(
                    impactPosition,
                    rocket.
                        doomToSkyrimScale
                );



                // SKYDOOM_ROCKET_EXPLOSION_SOUND_NOTIFY_V13_1

                PushInputEvent(

                    SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE,

                    SKYDOOM_COMBAT_WEAPON_ROCKET,

                    0

                );







                if (blockerRef)

                {

                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM rocket WORLD impact: blocker={:08X} distanceThisStep={}",

                        blockerRef->

                            GetFormID(),

                        worldHitDistance

                    );

                }

                else

                {

                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM rocket WORLD impact: blocker=STATIC distanceThisStep={}",

                        worldHitDistance

                    );

                }





                ApplySkyDoomRocketSplash(

                    impactPosition,

                    rocket.

                        doomToSkyrimScale

                );





                continue;

            }





            rocket.position =

                segmentEnd;

        }

    



        /*

            SKYDOOM_VISIBLE_ROCKET_SPRITE_V11_1



            Publish only after the authoritative v11 collision

            simulation has finished this update.

        */



        PublishSkyDoomRocketRenderSnapshots();



}





    void ProcessDoomShotgunCombatBridge(

        bool doomFresh

    )

    {

        /*

            SKYDOOM_COMBAT_EVENT_CONSUMER_V10

            SKYDOOM_COMBAT_EVENT_CONSUMER_V11



            This remains the ONE consumer of the Chocolate Doom ->

            Skyrim combat ring.



            Current events:



                shotgun pellet

                chaingun bullet

                fist attack

                chainsaw attack

                rocket fired

                rocket direct-hit RNG response

        */



        if (!g_state)

        {

            return;

        }





        auto& ring =

            g_state->

                combat;





        if (

            !doomFresh ||

            !g_state->doom.running ||

            !g_state->doom.in_level ||

            !g_state->skyrim.in_game ||

            g_state->skyrim.paused

        )

        {

            ring.tail =

                ring.head;



            return;

        }





        static std::uint32_t

            lastDropped =

                0;





        if (

            ring.dropped !=

            lastDropped

        )

        {

            SKSE::log::warn(

                "[skydoomskse] DOOM combat ring dropped events: {} -> {}",

                lastDropped,

                ring.dropped

            );





            lastDropped =

                ring.dropped;

        }





        for (

            std::uint32_t processed = 0;

            processed < 128u;

            ++processed

        )

        {

            const std::uint32_t head =

                ring.head;





            const std::uint32_t tail =

                ring.tail;





            if (tail == head)

            {

                break;

            }





            MemoryBarrier();





            const SkyDoomCombatEvent event =

                ring.events[

                    tail &

                    SKYDOOM_COMBAT_RING_MASK

                ];





            ring.tail =

                tail + 1u;





            // ------------------------------------------------

            // ------------------------------------------------

            // SKYDOOM_REAL_BFG_V14 - STAGE 1 FIRE CONFIRMATION

            // ------------------------------------------------



            if (

                event.type ==

                    SKYDOOM_COMBAT_EVENT_BFG_FIRED &&

                event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_BFG

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM BFG fire event"

                );

                SpawnSkyDoomBFGVisualV14_2A();

                continue;

            }



            // REAL DOOM PLASMA VISUAL LAUNCH - V12.2A

            // ------------------------------------------------



            if (

                event.type ==

                    SKYDOOM_COMBAT_EVENT_PLASMA_FIRED &&

                event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_PLASMA

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM plasma fire event"

                );



                SpawnSkyDoomPlasmaVisualV12_2A();



                continue;

            }



            // ------------------------------------------------

            // REAL DOOM ROCKET LAUNCH

            // ------------------------------------------------



            if (

                event.type ==

                    SKYDOOM_COMBAT_EVENT_ROCKET_FIRED &&

                event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_ROCKET

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM rocket fire event"

                );





                SpawnSkyDoomRocket();



                continue;

            }





            // ------------------------------------------------

            // ------------------------------------------------

            // REAL DOOM PLASMA DIRECT-HIT RNG RESPONSE - V13

            // ------------------------------------------------



            if (

                event.type ==

                    SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE &&

                event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_PLASMA

            )

            {

                if (

                    event.damage <

                        5 ||

                    event.damage >

                        40 ||

                    (

                        event.damage %

                        5

                    ) !=

                        0 ||

                    event.angle_offset <=

                        0

                )

                {

                    SKSE::log::warn(

                        "[skydoomskse] Invalid REAL DOOM plasma damage response: requestId={} damage={}",

                        event.angle_offset,

                        event.damage

                    );



                    continue;

                }



                SKSE::log::info(

                    "[skydoomskse] REAL DOOM plasma damage roll: requestId={} damage={}",

                    event.angle_offset,

                    event.damage

                );



                ResolveSkyDoomPlasmaDamageRollV13(

                    event.angle_offset,

                    event.damage

                );



                continue;

            }



            // ------------------------------------------------
            // REAL DOOM BFG DIRECT + A_BFGSpray RNG - V14.3
            // ------------------------------------------------

            if (
                event.type ==
                    SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE &&
                event.weapon ==
                    SKYDOOM_COMBAT_WEAPON_BFG
            )
            {
                const bool direct =
                    event.slope_offset ==
                        1;

                const bool spray =
                    event.slope_offset ==
                        2;

                const bool directDamageValid =
                    direct &&
                    event.damage >=
                        100 &&
                    event.damage <=
                        800 &&
                    (
                        event.damage %
                        100
                    ) ==
                        0;

                const bool sprayDamageValid =
                    spray &&
                    event.damage >=
                        15 &&
                    event.damage <=
                        120;

                if (
                    event.angle_offset <=
                        0 ||
                    (
                        !directDamageValid &&
                        !sprayDamageValid
                    )
                )
                {
                    SKSE::log::warn(
                        "[skydoomskse] Invalid REAL DOOM BFG damage response: requestId={} damage={} mode={}",
                        event.angle_offset,
                        event.damage,
                        event.slope_offset
                    );

                    continue;
                }

                ResolveSkyDoomBFGDamageRollV14_3(
                    event.angle_offset,
                    event.damage,
                    event.slope_offset
                );

                continue;
            }

                        // ------------------------------------------------
            // PHYSICAL DOOM PICKUP RESULT - V15.3
            // ------------------------------------------------

            if (
                event.type ==
                    SKYDOOM_COMBAT_EVENT_PICKUP_RESULT &&
                event.weapon >=
                    SKYDOOM_PICKUP_MEDIKIT &&
                event.weapon <= SKYDOOM_PICKUP_MEGASPHERE
            )
            {
                if (
                    event.angle_offset <=
                        0 ||
                    (
                        event.damage !=
                            0 &&
                        event.damage !=
                            1
                    )
                )
                {
                    SKSE::log::warn(
                        "[skydoomskse] Invalid pickup result: type={} requestId={} accepted={} resourceAfter={}",
                        event.weapon,
                        event.angle_offset,
                        event.damage,
                        event.slope_offset
                    );

                    continue;
                }

                ResolveSkyDoomPickupResultV15(
                    event.angle_offset,
                    event.damage,
                    event.slope_offset
                );

                continue;
            }



            // REAL DOOM ROCKET DIRECT-HIT RNG RESPONSE

            // ------------------------------------------------



            if (

                event.type ==

                    SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE &&

                event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_ROCKET

            )

            {

                if (

                    event.damage < 20 ||

                    event.damage > 160 ||

                    (event.damage % 20) != 0 ||

                    event.angle_offset <= 0

                )

                {

                    SKSE::log::warn(

                        "[skydoomskse] Invalid REAL DOOM rocket damage response: requestId={} damage={}",

                        event.angle_offset,

                        event.damage

                    );



                    continue;

                }





                SKSE::log::info(

                    "[skydoomskse] REAL DOOM rocket damage roll: requestId={} damage={}",

                    event.angle_offset,

                    event.damage

                );





                ResolveSkyDoomRocketDamageRoll(

                    event.angle_offset,

                    event.damage

                );





                continue;

            }





            // ------------------------------------------------

            // REAL DOOM MELEE

            // ------------------------------------------------



            if (

                event.type ==

                SKYDOOM_COMBAT_EVENT_MELEE

            )

            {

                if (

                    event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_FIST

                )

                {

                    if (

                        event.damage < 2 ||

                        event.damage > 200 ||

                        (event.damage & 1) != 0

                    )

                    {

                        SKSE::log::warn(

                            "[skydoomskse] Invalid REAL DOOM fist event: damage={} angleOffset={}",

                            event.damage,

                            event.angle_offset

                        );



                        continue;

                    }





                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM fist attack: damage={} angleOffset={}",

                        event.damage,

                        event.angle_offset

                    );





                    ApplyDoomMeleeDamageToCrosshairActor(

                        event.damage,

                        event.angle_offset

                    );





                    continue;

                }





                if (

                    event.weapon ==

                    SKYDOOM_COMBAT_WEAPON_CHAINSAW

                )

                {

                    if (

                        event.damage < 2 ||

                        event.damage > 20 ||

                        (event.damage & 1) != 0

                    )

                    {

                        SKSE::log::warn(

                            "[skydoomskse] Invalid REAL DOOM chainsaw event: damage={} angleOffset={}",

                            event.damage,

                            event.angle_offset

                        );



                        continue;

                    }





                    SKSE::log::info(

                        "[skydoomskse] REAL DOOM chainsaw attack: damage={} angleOffset={}",

                        event.damage,

                        event.angle_offset

                    );





                    ApplyDoomMeleeDamageToCrosshairActor(

                        event.damage,

                        event.angle_offset

                    );





                    continue;

                }





                continue;

            }





            // ------------------------------------------------

            // EXISTING REAL DOOM BULLET EVENTS

            // ------------------------------------------------



            if (

                event.type !=

                SKYDOOM_COMBAT_EVENT_PELLET

            )

            {

                continue;

            }





            if (

                event.damage != 5 &&

                event.damage != 10 &&

                event.damage != 15

            )

            {

                SKSE::log::warn(

                    "[skydoomskse] Invalid DOOM bullet event: weapon={} damage={} angleOffset={}",

                    event.weapon,

                    event.damage,

                    event.angle_offset

                );



                continue;

            }





            if (

                event.weapon ==

                SKYDOOM_COMBAT_WEAPON_SHOTGUN

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM shotgun pellet: damage={} angleOffset={} slopeOffset={}",

                    event.damage,

                    event.angle_offset,

                    event.slope_offset

                );





                ApplyDoomShotgunPelletDamageToCrosshairActor(

                    event.damage,

                    event.angle_offset

                );





                continue;

            }





            if (

                event.weapon ==

                SKYDOOM_COMBAT_WEAPON_CHAINGUN

            )

            {

                SKSE::log::info(

                    "[skydoomskse] REAL DOOM chaingun bullet: damage={} angleOffset={} slopeOffset={}",

                    event.damage,

                    event.angle_offset,

                    event.slope_offset

                );





                ApplyDoomChaingunBulletDamageToCrosshairActor(

                    event.damage,

                    event.angle_offset

                );





                continue;

            }

        }

    }





// SKYDOOM PLAYER HEALTH + RESPAWN BRIDGE

    // ========================================================



    // SKYDOOM_SKYRIM_INCOMING_DAMAGE_V7

    // SKYDOOM_RESPAWN_BRIDGE_V8



    void ProcessSkyrimIncomingDamageBridge(

        bool doomFresh,

        RE::PlayerCharacter* player

    )

    {

        /*

            NORMAL PLAY:



                Skyrim HP loss

                    ->

                restore Skyrim host HP

                    ->

                send raw integer damage to Chocolate Doom

                    ->

                real P_DamageMobj

                    ->

                real DOOM health / armor / HUD





            DEATH:



                DOOM health reaches zero

                    ->

                kill Skyrim host





            RESPAWN / SAVE RELOAD:



                observe the Skyrim death/load transition

                    ->

                when Skyrim becomes alive again

                    ->

                send SKYDOOM_INPUT_EVENT_RESPAWN

                    ->

                Chocolate Doom enters PST_REBORN

                    ->

                real DOOM rebirth resets HUD/player state

        */





        static bool

            haveHostHealthSnapshot =

                false;





        static float

            previousHostHealth =

                0.0f;





        static float

            fractionalDamage =

                0.0f;





        static bool

            doomDeathIssued =

                false;





        /*

            These deliberately survive menu/loading transitions.



            resetDamageSensor() must NOT clear them.

        */



        static bool

            awaitingSkyrimRespawn =

                false;





        static bool

            sawHostDead =

                false;





        static bool

            sawPostDeathInactive =

                false;





        static bool

            waitingForDoomReborn =

                false;





        static std::int32_t

            observedDoomHealth =

                -1;





        static std::int32_t

            observedDoomArmor =

                -1;





        auto resetDamageSensor =

            [&]()

            {

                haveHostHealthSnapshot =

                    false;





                previousHostHealth =

                    0.0f;





                fractionalDamage =

                    0.0f;





                observedDoomHealth =

                    -1;





                observedDoomArmor =

                    -1;

            };





        if (

            !g_state ||

            !player

        )

        {

            resetDamageSensor();



            return;

        }





        const bool bridgeActive =

            doomFresh &&

            g_state->

                doom.

                running &&

            g_state->

                doom.

                in_level &&

            g_state->

                skyrim.

                in_game &&

            !g_state->

                skyrim.

                paused;





        /*

            A Skyrim death normally introduces a death screen /

            loading period.



            Remember that transition even though the ordinary health

            sensor is inactive during it.

        */



        if (!bridgeActive)

        {

            if (awaitingSkyrimRespawn)

            {

                if (

                    player->

                        IsDead()

                )

                {

                    sawHostDead =

                        true;

                }





                if (

                    !g_state->

                        skyrim.

                        in_game ||

                    g_state->

                        skyrim.

                        paused

                )

                {

                    sawPostDeathInactive =

                        true;

                }

            }





            resetDamageSensor();



            return;

        }





        /*

            After we have requested a real DOOM rebirth, DO NOT kill

            Skyrim again merely because the shared DOOM health still

            reads zero for another update or two.



            Wait for Chocolate Doom to publish its reborn health.

        */



        if (waitingForDoomReborn)

        {

            if (

                g_state->

                    doom.

                    health >

                0

            )

            {

                SKSE::log::info(

                    "[skydoomskse] DOOM reborn confirmed: health={} armor={}",

                    g_state->doom.health,

                    g_state->doom.armor

                );





                waitingForDoomReborn =

                    false;





                doomDeathIssued =

                    false;





                resetDamageSensor();

            }





            return;

        }





        /*

            We killed Skyrim because DOOM died.



            Wait until Skyrim has genuinely transitioned through

            death/loading and is alive again before resetting DOOM.



            sawHostDead covers a normal observable Skyrim death.



            sawPostDeathInactive covers the case where loading begins

            before our 50ms update loop samples IsDead().

        */



        if (awaitingSkyrimRespawn)

        {

            if (

                player->

                    IsDead()

            )

            {

                sawHostDead =

                    true;



                return;

            }





            /*

                If something else has already revived DOOM, simply

                return to normal operation.

            */



            if (

                g_state->

                    doom.

                    health >

                0

            )

            {

                SKSE::log::info(

                    "[skydoomskse] DOOM was already alive after Skyrim respawn: health={} armor={}",

                    g_state->doom.health,

                    g_state->doom.armor

                );





                awaitingSkyrimRespawn =

                    false;





                sawHostDead =

                    false;





                sawPostDeathInactive =

                    false;





                doomDeathIssued =

                    false;





                resetDamageSensor();



                return;

            }





            if (

                sawHostDead ||

                sawPostDeathInactive

            )

            {

                SKSE::log::info(

                    "[skydoomskse] Skyrim respawn/load detected - requesting real DOOM reborn"

                );





                PushInputEvent(

                    SKYDOOM_INPUT_EVENT_RESPAWN,

                    0,

                    1

                );





                awaitingSkyrimRespawn =

                    false;





                sawHostDead =

                    false;





                sawPostDeathInactive =

                    false;





                doomDeathIssued =

                    false;





                waitingForDoomReborn =

                    true;





                resetDamageSensor();



                return;

            }





            /*

                We have issued the host death but have not yet seen

                Skyrim actually enter death/loading.



                Do nothing here. In particular, do NOT issue another

                lethal Skyrim hit.

            */



            return;

        }





        auto* actorValueOwner =

            player->

                AsActorValueOwner();





        if (!actorValueOwner)

        {

            resetDamageSensor();



            return;

        }





        /*

            Runtime proof that Chocolate Doom's actual vitals changed.

        */



        if (

            g_state->

                doom.

                health !=

                    observedDoomHealth ||

            g_state->

                doom.

                armor !=

                    observedDoomArmor

        )

        {

            if (

                observedDoomHealth >=

                0

            )

            {

                SKSE::log::info(

                    "[skydoomskse] DOOM vitals changed: health={} -> {} armor={} -> {}",

                    observedDoomHealth,

                    g_state->doom.health,

                    observedDoomArmor,

                    g_state->doom.armor

                );

            }





            observedDoomHealth =

                g_state->

                    doom.

                    health;





            observedDoomArmor =

                g_state->

                    doom.

                    armor;

        }





        /*

            Chocolate Doom decides death.



            Once its health reaches zero, kill the Skyrim host exactly

            once and enter the respawn-wait state.

        */



        if (

            g_state->

                doom.

                health <=

            0

        )

        {

            if (

                player->

                    IsDead()

            )

            {

                sawHostDead =

                    true;





                awaitingSkyrimRespawn =

                    true;





                return;

            }





            if (!doomDeathIssued)

            {

                doomDeathIssued =

                    true;





                awaitingSkyrimRespawn =

                    true;





                sawHostDead =

                    false;





                sawPostDeathInactive =

                    false;





                SKSE::log::info(

                    "[skydoomskse] DOOM health reached zero - killing Skyrim host player"

                );





                player->

                    DoDamage(

                        1000000.0f,

                        nullptr,

                        true

                    );

            }





            return;

        }





        doomDeathIssued =

            false;





        const float currentHostHealth =

            actorValueOwner->

                GetActorValue(

                    RE::ActorValue::

                        kHealth

                );





        /*

            First healthy sample establishes Skyrim's host-health

            sensor baseline.

        */



        if (!haveHostHealthSnapshot)

        {

            previousHostHealth =

                currentHostHealth;





            haveHostHealthSnapshot =

                true;





            SKSE::log::info(

                "[skydoomskse] Skyrim->DOOM health bridge armed: hostHealth={} doomHealth={} doomArmor={}",

                currentHostHealth,

                g_state->doom.health,

                g_state->doom.armor

            );





            return;

        }





        /*

            Positive Skyrim health changes become the new sensor

            baseline.



            DOOM healing itself can be bridged separately later.

        */



        if (

            currentHostHealth >

            previousHostHealth +

                0.01f

        )

        {

            previousHostHealth =

                currentHostHealth;



            return;

        }





        if (

            currentHostHealth >=

            previousHostHealth -

                0.01f

        )

        {

            previousHostHealth =

                currentHostHealth;



            return;

        }





        const float hostLoss =

            previousHostHealth -

            currentHostHealth;





        /*

            Repair the Skyrim host damage.



            The user's local CommonLib exposes the two-argument form:



                RestoreActorValue(ActorValue, float)

        */



        actorValueOwner->

            RestoreActorValue(

                RE::ActorValue::

                    kHealth,

                hostLoss

            );





        const float restoredHostHealth =

            actorValueOwner->

                GetActorValue(

                    RE::ActorValue::

                        kHealth

                );





        /*

            Preserve fractional Skyrim damage rather than silently

            dropping it.

        */



        const float accumulatedDamage =

            hostLoss +

            fractionalDamage;





        std::int32_t doomDamage =

            static_cast<std::int32_t>(

                std::floor(

                    accumulatedDamage

                )

            );





        fractionalDamage =

            accumulatedDamage -

            static_cast<float>(

                doomDamage

            );





        if (doomDamage > 100000)

        {

            doomDamage =

                100000;





            fractionalDamage =

                0.0f;

        }





        if (doomDamage > 0)

        {

            PushInputEvent(

                SKYDOOM_INPUT_EVENT_DAMAGE,

                0,

                doomDamage

            );





            SKSE::log::info(

                "[skydoomskse] SKYRIM->DOOM incoming damage: hostLoss={} doomRawDamage={} hostHealth={} -> {} doomHealth={} doomArmor={}",

                hostLoss,

                doomDamage,

                currentHostHealth,

                restoredHostHealth,

                g_state->doom.health,

                g_state->doom.armor

            );

        }





        previousHostHealth =

            restoredHostHealth;

    }











	void UpdateSharedState()
	{
		if (!g_state) {
			return;
		}

		const auto now =
			static_cast<
				std::uint64_t>(
				GetTickCount64());

		g_state->skyrim.running =
			1;

		g_state->skyrim.pid =
			GetCurrentProcessId();

		g_state->skyrim.heartbeat_ms =
			now;

		g_state->skyrim.update_counter++;

		auto* ui =
			RE::UI::
				GetSingleton();

		const bool paused =
			ui &&
			ui->GameIsPaused();

		g_state->skyrim.paused =
			paused ?
				1u :
				0u;

		auto* player =
			RE::PlayerCharacter::
				GetSingleton();

		if (player) {
			const auto position =
				player->GetPosition();

			g_state->skyrim.player_x =
				position.x;

			g_state->skyrim.player_y =
				position.y;

			g_state->skyrim.player_z =
				position.z;

			g_state->skyrim.player_yaw =
				player->GetAngleZ();

			g_state->skyrim.player_pitch =
				player->GetAngleX();

			g_state->skyrim.in_game =
				(player->GetParentCell() &&
					!paused) ?
					1u :
					0u;
		} else {
			g_state->skyrim.in_game =
				0;
		}

		const bool doomFresh =
			DoomHeartbeatIsFresh();



        // SKYDOOM_PROCESS_INCOMING_DAMAGE_V7

        ProcessSkyrimIncomingDamageBridge(

            doomFresh,

            player

        );

        // SKYDOOM_PROCESS_PISTOL_COMBAT_V4
        ProcessDoomPistolCombatBridge(
            doomFresh
        );



        // SKYDOOM_PROCESS_SHOTGUN_COMBAT_V6

        ProcessDoomShotgunCombatBridge(

            doomFresh

        );



        // SKYDOOM_PROCESS_ROCKET_PROJECTILES_V11

        ProcessSkyDoomRockets(

            doomFresh

        );

        // SKYDOOM_PROCESS_ROCKET_RAGDOLL_RECOVERY_V15_9C1
        ProcessSkyDoomRocketRagdollRecoveriesV15_9C1();

        // SKYDOOM_PROCESS_PLASMA_IMPACTS_V13
        ProcessSkyDoomPlasmaImpactsV13(
            doomFresh
        );
        // SKYDOOM_PROCESS_BFG_IMPACTS_V14_2B
        ProcessSkyDoomBFGImpactsV14_2B(
            doomFresh
        );
        // SKYDOOM_PROCESS_BFG_SPRAY_V14_3
        ProcessSkyDoomBFGSpraysV14_3(
            doomFresh
        );
        // SKYDOOM_PROCESS_PHYSICAL_PICKUPS_V15
        ProcessSkyDoomPhysicalPickupsV15(
            doomFresh
        );

        // SKYDOOM_APPLY_PRESENTATION_V4
        ApplySkyDoomPresentation(
            doomFresh &&
            g_state->skyrim.in_game
        );

		if (
			doomFresh &&
			!g_reportedDoomConnected) {
			g_reportedDoomConnected =
				true;

			logger::info(
				"Hidden DOOM guest connected. PID={}",
				g_state->doom.pid);

			if (
				auto* console =
					RE::ConsoleLog::
						GetSingleton()) {
				console->Print(
					"[skydoomskse] Hidden DOOM guest connected!");
			}
		} else if (
			!doomFresh) {
			g_reportedDoomConnected =
				false;
		}
	}

	void StartUpdateThread()
	{
		if (
			g_running.exchange(
				true)) {
			return;
		}

		g_updateThread =
			std::jthread(
				[](
					std::stop_token
						stopToken) {
					while (
						!stopToken.stop_requested() &&
						g_running.load()) {
						std::this_thread::
							sleep_for(
								std::chrono::
									milliseconds(
										50));

						if (
							g_taskPending.exchange(
								true)) {
							continue;
						}

						auto* taskInterface =
							SKSE::
								GetTaskInterface();

						if (!taskInterface) {
							g_taskPending =
								false;

							continue;
						}

						taskInterface->AddTask(
							[]() {
								UpdateSharedState();

								g_taskPending =
									false;
							});
					}
				});

		logger::info(
			"SkyDoom Skyrim state loop started");
	}

	// ========================================================
	// STARTUP
	// ========================================================

	void OnDataLoaded()
	{
		logger::info(
			"SkyDoom Visual Overlay v4 DataLoaded");

		if (
			!InitialiseSkyDoomPortablePaths()) {
			logger::error(
				"SkyDoom public beta prerequisite discovery failed");

			return;
		}

		if (
			!InitSharedMemory()) {
			logger::error(
				"Failed to initialise SkyDoom v4 shared memory");

			return;
		}

		if (
			!RegisterInputSink()) {
			logger::error(
				"Failed to register SkyDoom input sink");

			return;
		}

		if (
			!InstallRenderHook()) {
			logger::error(
				"Failed to install SkyDoom native D3D11 rendering hook");
		}

		UpdateSharedState();

		StartUpdateThread();

		if (
			!LaunchDoomGuest()) {
			logger::error(
				"Failed to launch hidden DOOM guest");
		}

		const auto* plugin =
			SKSE::PluginDeclaration::
				GetSingleton();

		const auto name =
			plugin ?
				plugin->GetName() :
				"SkyDoomSKSE";

		if (
			auto* console =
				RE::ConsoleLog::
					GetSingleton()) {
			console->Print(
				"[%s] SkyDoom Visual Overlay v4 loaded successfully!",
				std::string(
					name)
					.c_str());

			if (
				g_renderHookInstalled) {
				console->Print(
					"[skydoomskse] Native DOOM weapon renderer installed!");
			}
		}

		logger::info(
			"SkyDoom Visual Overlay v4 startup complete");
	}
}

SKSEPluginLoad(
	const SKSE::LoadInterface*
		a_skse)
{
	SKSE::Init(
		a_skse);

	SetupLog();

	const auto* plugin =
		SKSE::PluginDeclaration::
			GetSingleton();

	if (!plugin) {
		return false;
	}

	logger::info(
		"{} v{} loaded",
		plugin->GetName(),
		plugin->GetVersion());

	const auto* messaging =
		SKSE::
			GetMessagingInterface();

	if (!messaging) {
		return false;
	}

	if (
		!messaging->RegisterListener(
			[](
				SKSE::
					MessagingInterface::
						Message*
							a_msg) {
				switch (
					a_msg->type) {
				case SKSE::
					MessagingInterface::
						kDataLoaded:

					OnDataLoaded();

					break;

				default:

					break;
				}
			})) {
		return false;
	}

	return true;
}


