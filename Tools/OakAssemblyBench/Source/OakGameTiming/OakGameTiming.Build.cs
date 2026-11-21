using UnrealBuildTool;

public class OakGameTiming : ModuleRules
{
    public OakGameTiming(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RHI", "RenderCore" });
    }
}
