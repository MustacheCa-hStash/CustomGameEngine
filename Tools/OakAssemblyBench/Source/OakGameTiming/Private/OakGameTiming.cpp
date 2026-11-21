#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/MaterialInterface.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "UnrealClient.h"
#include "EngineModule.h"
#include "GlobalShader.h"
#include "Misc/SecureHash.h"
#include "RendererInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

class FOakGameTiming : public IModuleInterface
{
    FDelegateHandle BeginHandle, EndHandle;
    TArray<AStaticMeshActor*> Trees;
    ACameraActor* Camera=nullptr;
    AStaticMeshActor* Ground=nullptr;
    FString Folder, Stem, Wall;
    int32 Job=0, Frame=0;
    bool RasterProfile=false;
    FDelegateHandle DepthHandle;
    bool CaptureDepth=false; // Accessed only on the render thread.
    TUniquePtr<FRHIGPUTextureReadback> DepthReadback;
    double Start=0;
    static constexpr int32 Frames=61;

    void ReadSceneDepth(FPostOpaqueRenderParameters& View)
    {
        if (!CaptureDepth || View.ViewportRect.Size()!=FIntPoint(3840,2160)) return;
        CaptureDepth=false;
        if (!DepthReadback) DepthReadback=MakeUnique<FRHIGPUTextureReadback>(TEXT("OakDepthReadback"));
        auto& Graph=*View.GraphBuilder;
        const auto Desc=FRDGTextureDesc::Create2D(View.ViewportRect.Size(),PF_R32_FLOAT,
            FClearValueBinding::None,TexCreate_RenderTargetable|TexCreate_ShaderResource);
        auto* Depth=Graph.CreateTexture(Desc,TEXT("OakValidationDepth"));
        FRDGDrawTextureInfo Copy;
        Copy.SourcePosition=View.ViewportRect.Min;
        Copy.Size=View.ViewportRect.Size();
        AddDrawTexturePass(Graph,GetGlobalShaderMap(GMaxRHIFeatureLevel),View.DepthTexture,Depth,Copy);
        AddEnqueueCopyPass(Graph,DepthReadback.Get(),Depth);
    }

    void SaveSceneDepth(const FString& Path)
    {
        if (CaptureDepth || !DepthReadback || !DepthReadback->IsReady())
            UE_LOG(LogTemp,Fatal,TEXT("Oak validation depth unavailable"));
        int32 Pitch=0;
        const uint32* Data=static_cast<const uint32*>(DepthReadback->Lock(Pitch));
        TArray<uint8> Mask;
        Mask.SetNumZeroed(3840*2160/8);
        FSHA1 Hash;
        for (int32 Y=0; Y<2160; ++Y) {
            const uint32* Row=Data+Y*Pitch;
            Hash.Update(reinterpret_cast<const uint8*>(Row),3840*sizeof(uint32));
            for (int32 X=0; X<3840; ++X)
                if (Row[X]) Mask[(Y*3840+X)/8] |= uint8(1u<<(X%8));
        }
        DepthReadback->Unlock();
        Hash.Final();
        FSHAHash Digest;
        Hash.GetHash(Digest.Hash);
        if (!FFileHelper::SaveArrayToFile(Mask,*(Path+TEXT(".mask"))) ||
            !FFileHelper::SaveStringToFile(Digest.ToString(),*(Path+TEXT(".sha1")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            UE_LOG(LogTemp,Fatal,TEXT("Could not write oak validation depth"));
    }

    void Setup(UWorld* World)
    {
        Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/
            (RasterProfile ? TEXT("../oak-raster-profile/game") : TEXT("../oak-gpu-traversal/game")));
        IFileManager::Get().MakeDirectory(*Folder,true);
        if (RasterProfile)
            DepthHandle=GetRendererModule().RegisterPostOpaqueRenderDelegate(
                FPostOpaqueRenderDelegate::CreateRaw(this,&FOakGameTiming::ReadSceneDepth));
        for (const TCHAR* Setting : {TEXT("t.MaxFPS 0"),TEXT("r.VSync 0"),TEXT("r.ScreenPercentage 100"),
             TEXT("r.DynamicRes.OperationMode 0"),TEXT("r.AntiAliasingMethod 0"),TEXT("r.MotionBlurQuality 0"),
             TEXT("r.Lumen.DiffuseIndirect.Allow 0"),TEXT("r.Lumen.Reflections.Allow 0"),TEXT("r.Fog 0"),
             TEXT("r.Nanite.Culling.Frustum 1"),TEXT("r.Nanite.Culling.HZB 0"),TEXT("r.Nanite.MaxPixelsPerEdge 1"),
             TEXT("r.GPUCsvStatsEnabled 1"),TEXT("r.OneFrameThreadLag 0"),TEXT("r.Shadow.Virtual.Cache 1")})
            GEngine->Exec(World,Setting);
        auto* Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/NaniteOakBench/Tree_Optimized_Triangles.Tree_Optimized_Triangles"));
        check(Mesh);
        constexpr double Spacing=1901.67193;
        for (int32 i=0;i<4;++i) {
            const bool Center=i==3;
            auto* Tree=World->SpawnActor<AStaticMeshActor>(FVector(Spacing*(i%2-1),Spacing*(i/2-1),0),
                FRotator(0,Center?0:-FMath::RadiansToDegrees(1.25*i),0));
            Tree->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            Tree->GetStaticMeshComponent()->SetStaticMesh(Mesh);
            Tree->SetActorScale3D(FVector(Center?1.:.92+.04*((i*7)%5)));
            Trees.Add(Tree);
        }
        Ground=World->SpawnActor<AStaticMeshActor>(FVector(0,0,-50),FRotator::ZeroRotator);
        Ground->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        Ground->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        Ground->SetActorScale3D(FVector(500,500,1));
        auto* Key=World->SpawnActor<ADirectionalLight>(FVector::ZeroVector,FRotator(-39,-42,0));
        Key->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Key->GetLightComponent()->SetIntensity(6);
        auto* Fill=World->SpawnActor<ADirectionalLight>(FVector::ZeroVector,FRotator(-35,138,0));
        Fill->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Fill->GetLightComponent()->SetIntensity(1.6f);
        Fill->GetLightComponent()->SetCastShadows(false);
        Camera=World->SpawnActor<ACameraActor>();
        auto* Lens=Camera->GetCameraComponent();
        Lens->SetFieldOfView(FMath::RadiansToDegrees(2*FMath::Atan(FMath::Tan(PI/8)*16./9.)));
        Lens->SetAspectRatio(16.f/9);
        World->GetFirstPlayerController()->SetViewTarget(Camera);
    }

    void Begin()
    {
        if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport) return;
        UWorld* World=GEngine->GameViewport->GetWorld();
        if (!World || !World->HasBegunPlay() || !World->GetFirstPlayerController()) return;
        if (!Camera) Setup(World);
        if (Job>=24) return;
        const int32 Repeat=Job/6, Case=Repeat%2==0 ? Job%6 : 5-Job%6;
        // Each pose sees full/flat/flat/full or flat/full/full/flat, with two captures per arm.
        const bool Flat=RasterProfile && ((Repeat==1 || Repeat==2) != bool(Case%2));
        const int32 Count=Case<3?1:4;
        constexpr int32 Distances[]{24,41,100};
        const int32 D=Distances[Case%3];
        if (Frame==0) {
            if (RasterProfile) {
                for (auto* Tree : Trees) {
                    auto* Component=Tree->GetStaticMeshComponent();
                    for (int32 Slot=0; Slot<Component->GetNumMaterials(); ++Slot) {
                        UMaterialInterface* Material=nullptr;
                        if (Flat) {
                            const FString Path=FString::Printf(TEXT("/Game/NaniteOakBench/RasterFlatSlot%d.RasterFlatSlot%d"),Slot,Slot);
                            Material=LoadObject<UMaterialInterface>(nullptr,*Path);
                            auto* Original=Component->GetStaticMesh()->GetMaterial(Slot);
                            if (!Material || !Original || Material->IsTwoSided()!=Original->IsTwoSided() ||
                                Original->GetBlendMode()!=BLEND_Opaque)
                                UE_LOG(LogTemp,Fatal,TEXT("OakRaster invalid flat override for slot %d"),Slot);
                        }
                        Component->SetMaterial(Slot,Material);
                    }
                }
            }
            for (int32 i=0;i<4;++i) Trees[i]->SetActorHiddenInGame(i!=3 && Count==1);
            const FVector Focus(-.98806394,-6.50237372,731.54865397);
            const double P=FMath::DegreesToRadians(D==100?-2.235:-5.46), Y=FMath::DegreesToRadians(132.);
            const FVector Eye=Focus+100*D*FVector(FMath::Cos(P)*FMath::Sin(Y),FMath::Cos(P)*FMath::Cos(Y),FMath::Sin(P));
            Camera->SetActorLocationAndRotation(Eye,(Focus-Eye).Rotation());
            World->GetFirstPlayerController()->SetViewTarget(Camera);
            Stem=FString::Printf(TEXT("%s-%d-%d-%d"),Repeat<2 ? TEXT("fenced") : TEXT("pipelined"),Count,D,Repeat%2);
            if (RasterProfile) Stem=FString::Printf(TEXT("%s-%d-%d-%d"),Flat?TEXT("flat"):TEXT("full"),Count,D,Repeat/2);
            UE_LOG(LogTemp,Display,TEXT("OakGame %s viewport %s"),*Stem,*GEngine->GameViewport->Viewport->GetSizeXY().ToString());
            Wall=TEXT("active_frame_ms\n");
        }
        if (RasterProfile && Frame==34) Ground->SetActorHiddenInGame(true);
        if (RasterProfile && Frame==45) {
            ENQUEUE_RENDER_COMMAND(RequestOakDepth)([this](FRHICommandListImmediate&){CaptureDepth=true;});
        }
        if (RasterProfile && Frame==48) {
            const FString Path=Folder/Stem;
            ENQUEUE_RENDER_COMMAND(SaveOakDepth)([this,Path](FRHICommandListImmediate&){SaveSceneDepth(Path);});
        }
        if (RasterProfile && Frame==50) Ground->SetActorHiddenInGame(false);
        const bool Stats=RasterProfile || Repeat==0;
        if (Frame==70 && Stats) GEngine->Exec(World,TEXT("NaniteStats Primary"));
        if (Frame==77 && Stats) FScreenshotRequest::RequestScreenshot(Folder/(Stem+TEXT("-stats.png")),false,false);
        if (Frame==82 && Stats) GEngine->Exec(World,TEXT("NaniteStats Off"));
        if (Frame==100) FCsvProfiler::Get()->BeginCapture(Frames,Folder,Stem+TEXT(".csv"));
        Start=FPlatformTime::Seconds();
    }

    void End()
    {
        if (!Camera || Job>=24) return;
        // Serialize each sampled game frame as the D3D capture does. Exclude file writes and warm-up.
        if (RasterProfile || Job<12) {
            ENQUEUE_RENDER_COMMAND(FenceOakFrame)([](FRHICommandListImmediate& Cmd){Cmd.SubmitAndBlockUntilGPUIdle();});
            FlushRenderingCommands();
        }
        const double Elapsed=(FPlatformTime::Seconds()-Start)*1000;
        if (Frame>=100 && Frame<100+Frames) Wall+=FString::Printf(TEXT("%.6f\n"),Elapsed);
        ++Frame;
        if (Frame>=185 && !FCsvProfiler::Get()->IsWritingFile()) {
            if (RasterProfile || Job<12) FFileHelper::SaveStringToFile(Wall,*(Folder/(Stem+TEXT("-wall.csv"))));
            Frame=0;
            if (++Job==24) {
                UE_LOG(LogTemp,Display,TEXT("OakGame COMPLETE"));
                FPlatformMisc::RequestExit(false);
            }
        }
    }

public:
    void StartupModule() override
    {
        if (!FParse::Param(FCommandLine::Get(),TEXT("OakGameTiming"))) return;
        RasterProfile=FParse::Param(FCommandLine::Get(),TEXT("OakRasterProfile"));
        BeginHandle=FCoreDelegates::OnBeginFrame.AddRaw(this,&FOakGameTiming::Begin);
        EndHandle=FCoreDelegates::OnEndFrame.AddRaw(this,&FOakGameTiming::End);
    }
    void ShutdownModule() override
    {
        if (DepthHandle.IsValid()) GetRendererModule().RemovePostOpaqueRenderDelegate(DepthHandle);
        FCoreDelegates::OnBeginFrame.Remove(BeginHandle);
        FCoreDelegates::OnEndFrame.Remove(EndHandle);
    }
};

IMPLEMENT_MODULE(FOakGameTiming,OakGameTiming)
