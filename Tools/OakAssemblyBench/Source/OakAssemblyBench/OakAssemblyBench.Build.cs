using System.IO;
using UnrealBuildTool;

public class OakAssemblyBench : ModuleRules
{
    public OakAssemblyBench(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        PrivateDependencyModuleNames.AddRange(new[] {"Core","CoreUObject","Engine","RenderCore","QuadricMeshReduction","NaniteUtilities","MeshUtilitiesCommon"});
        PrivateIncludePaths.Add(Path.Combine(EngineDirectory,"Source/Developer/NaniteBuilder/Private"));
        AddEngineThirdPartyPrivateStaticDependencies(Target,"metis");
        string repo = System.Environment.GetEnvironmentVariable("OAK_ENGINE_REPO") ?? Path.GetFullPath(Path.Combine(PluginDirectory,"../../../.."));
        PrivateIncludePaths.Add(Path.Combine(repo,"third_party/meshoptimizer"));
    }
}
