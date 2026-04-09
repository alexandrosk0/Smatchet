using System;
using System.IO;
using UnrealBuildTool;

public class SmatchetImGuiPlugin : ModuleRules
{
    public SmatchetImGuiPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Surface dead-code-adjacent issues in plugin .cpp/.h without changing engine modules.
        // (IWYU enforcement is optional: turn on when you want to pay down monolithic includes.)
        ShadowVariableWarningLevel = WarningLevel.Warning;
        UnsafeTypeCastWarningLevel = WarningLevel.Warning;
        bEnableUndefinedIdentifierWarnings = true;
        bEnforceIWYU = false;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "RHI",
                "RenderCore",
                "Projects"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "ApplicationCore",
                "InputCore"
            });

        string platformName = Target.Platform.ToString();
        bool isWin64 = Target.Platform == UnrealTargetPlatform.Win64;
        bool isPs5 = platformName.Equals("PS5", StringComparison.OrdinalIgnoreCase);
        bool isXbox = platformName.IndexOf("Xbox", StringComparison.OrdinalIgnoreCase) >= 0;

        if (isWin64)
        {
            // Helps MSVC drop unreferenced internal linkage / inline candidates (link-time hygiene).
            bVcRemoveUnreferencedComdat = true;

            // D3D12RHI headers require access to its internal AgilitySDK include setup.
            // Use a public dependency so include paths resolve during our compilation.
            PublicDependencyModuleNames.Add("D3D12RHI");

            // UE's D3D12RHI public headers include <d3dx12.h>.
            // In some build environments, that header is not picked up unless we
            // add the AgilitySDK d3dx12 include directory explicitly.
            var agilityIncludeRoot = Path.Combine(
                Target.RelativeEnginePath,
                "Source",
                "ThirdParty",
                "Windows",
                "AgilitySDK",
                "1.616.1",
                "Include");
            PublicSystemIncludePaths.Add(agilityIncludeRoot);

            var agilityD3DX12IncludeDir = Path.Combine(agilityIncludeRoot, "d3dx12");
            PublicSystemIncludePaths.Add(agilityD3DX12IncludeDir);

            // libcurl (Schannel backend) depends on these Windows crypto/import libs.
            PublicAdditionalLibraries.Add("crypt32.lib");
            PublicAdditionalLibraries.Add("cryptnet.lib");
        }

        string PluginDir = ModuleDirectory;
        string ThirdPartyDir = Path.GetFullPath(Path.Combine(PluginDir, "..", "..", "ThirdParty"));
        string SmatchetIncludeDir = Path.Combine(ThirdPartyDir, "Smatchet", "include");
        PublicIncludePaths.Add(SmatchetIncludeDir);

        // Expected output layout from a companion CMake packaging step.
        // Example:
        // ThirdParty/Smatchet/lib/<Platform>/Debug/SmatchetImGuiHost_<Backend>.lib
        string ConfigFolder = (Target.Configuration == UnrealTargetConfiguration.Debug && Target.bDebugBuildsActuallyUseDebugCRT)
            ? "Debug"
            : "Development";
        string backendSuffix;
        string libPlatformFolder;
        if (isWin64)
        {
            backendSuffix = "DX12";
            libPlatformFolder = "Win64";
        }
        else if (isPs5)
        {
            backendSuffix = "PS5";
            libPlatformFolder = "PS5";
        }
        else if (isXbox)
        {
            backendSuffix = "XBOX";
            libPlatformFolder = "Xbox";
        }
        else
        {
            throw new BuildException($"SmatchetImGuiPlugin: Unsupported platform '{platformName}'.");
        }
        string LibDir = Path.Combine(ThirdPartyDir, "Smatchet", "lib", libPlatformFolder, ConfigFolder);

        void AddExistingLib(string targetName)
        {
            // Naming differs between toolchains:
            // - MSVC:  SmatchetImGuiHost_DX12.lib
            // - MinGW: libSmatchetImGuiHost_DX12.a
            string msvcLibNoPrefix = Path.Combine(LibDir, targetName + ".lib");
            string msvcLibWithPrefix = Path.Combine(LibDir, "lib" + targetName + ".lib");
            string mingwANoPrefix = Path.Combine(LibDir, targetName + ".a");
            string mingwAWithPrefix = Path.Combine(LibDir, "lib" + targetName + ".a");

            if (File.Exists(msvcLibNoPrefix))
            {
                PublicAdditionalLibraries.Add(msvcLibNoPrefix);
                return;
            }
            if (File.Exists(msvcLibWithPrefix))
            {
                PublicAdditionalLibraries.Add(msvcLibWithPrefix);
                return;
            }
            if (File.Exists(mingwAWithPrefix))
            {
                PublicAdditionalLibraries.Add(mingwAWithPrefix);
                return;
            }
            if (File.Exists(mingwANoPrefix))
            {
                PublicAdditionalLibraries.Add(mingwANoPrefix);
                return;
            }

            throw new BuildException($"SmatchetImGuiPlugin: Missing native library for '{targetName}'. Expected '{msvcLibNoPrefix}', '{msvcLibWithPrefix}', '{mingwAWithPrefix}', or '{mingwANoPrefix}'.");
        }

        AddExistingLib("SmatchetImGuiHost_" + backendSuffix);
        AddExistingLib("SmatchetPlugin_Mcp_" + backendSuffix);
        AddExistingLib("SmatchetPlugin_LuaConsole_" + backendSuffix);
        AddExistingLib("SmatchetCore_" + backendSuffix);
        AddExistingLib("ImGuiLib_" + backendSuffix);

        // Dependencies required by SmatchetCore_DX12 (static libs don't automatically carry link-time deps).
        AddExistingLib("cpr");
        AddExistingLib("SQLiteCpp");
        AddExistingLib("sqlite3");
        AddExistingLib("Smatchet_Lua_Internal");
        AddExistingLib("libcurl");
    }
}
