#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/NaniteResources.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Math/Bounds.h"
#include "Templates/Sorting.h"
#include "GraphPartitioner.h"
#include "MeshSimplify.h"
#include "meshoptimizer.h"
#include <cmath>

namespace
{
struct FVertex
{
    FVector3f P, N;
    FVector2f UV[2];
    bool operator==(const FVertex& B) const { return FMemory::Memcmp(this, &B, sizeof(*this)) == 0; }
    friend uint32 GetTypeHash(const FVertex& V) { return FCrc::MemCrc32(&V, sizeof(V)); }
};
static_assert(sizeof(FVertex) == 40);
struct FMesh
{
    TArray<FVertex> V;
    TArray<uint32> I;
    TArray<uint8> Material;
    int32 Faces() const { return I.Num()/3; }
    void Append(const FMesh& Other)
    {
        const uint32 Base = V.Num();
        V.Append(Other.V);
        for (uint32 Id : Other.I) I.Add(Base + Id);
        Material.Append(Other.Material);
    }
    void Compact()
    {
        TMap<FVertex,uint32> Used;
        TArray<FVertex> Ordered;
        for (uint32& Id : I)
        {
            const auto& Vertex = V[Id];
            if (const auto* Old = Used.Find(Vertex)) Id = *Old;
            else { Id = Ordered.Add(Vertex); Used.Add(Vertex,Id); }
        }
        V = MoveTemp(Ordered);
    }
};

bool ReadMesh(const FString& Path, FMesh& Mesh)
{
    TUniquePtr<FArchive> F(IFileManager::Get().CreateFileReader(*Path));
    if (!F) return false;
    uint32 V, I;
    *F << V << I;
    if (!V || !I || I%3 || F->TotalSize()!=8+40ll*V+4ll*I) return false;
    Mesh.V.SetNumUninitialized(V); Mesh.I.SetNumUninitialized(I);
    F->Serialize(Mesh.V.GetData(),40ll*V); F->Serialize(Mesh.I.GetData(),4ll*I);
    for (uint32 Index : Mesh.I) if (Index>=V) return false;
    const FString Slots=FPaths::ChangeExtension(Path,TEXT("mat"));
    if (FPaths::FileExists(Slots) && (!FFileHelper::LoadFileToArray(Mesh.Material,*Slots) || Mesh.Material.Num()!=Mesh.Faces())) return false;
    return !F->IsError();
}

bool WriteLevel(const FString& Path, const TArray<FMesh>& Pieces)
{
    FMesh Mesh;
    for (const auto& Part : Pieces) Mesh.Append(Part);
    Mesh.Compact();
    TUniquePtr<FArchive> F(IFileManager::Get().CreateFileWriter(*Path));
    if (!F || Mesh.I.IsEmpty()) return false;
    int32 V = Mesh.V.Num(), I = Mesh.I.Num();
    *F << V << I;
    F->Serialize(Mesh.V.GetData(),40ll*V); F->Serialize(Mesh.I.GetData(),4ll*I);
    F->Close();
    return !F->IsError() && (Mesh.Material.IsEmpty() ||
        FFileHelper::SaveArrayToFile(Mesh.Material,*FPaths::ChangeExtension(Path,TEXT("mat"))));
}

// Find shared geometric edges across UV wedges. Unconnected twigs get locality
// links from Epic's partitioner. Only edges crossing two groups are locked.
struct FConnections
{
    TArray<TMap<int32,int32>> Neighbors;
    TArray<FVector3f> Centers;
    TMap<FVector3f,uint32> Positions;
    TArray<FVector3f> Points;
    TMap<uint64,int32> Owner;
    struct FCross { uint64 Edge; int32 A,B; };
    TArray<FCross> Crossings;
    explicit FConnections(int32 N) { Neighbors.SetNum(N); Centers.SetNumZeroed(N); }
    uint32 Point(const FVector3f& V)
    {
        if (auto* Id = Positions.Find(V)) return *Id;
        uint32 Id = Points.Add(V); Positions.Add(V,Id); return Id;
    }
    void Edge(uint32 A,uint32 B,int32 Id)
    {
        if (A==B) return;
        uint64 Key = (uint64(FMath::Min(A,B))<<32) | FMath::Max(A,B);
        if (auto* Prev = Owner.Find(Key))
        {
            if (*Prev!=Id)
            {
                ++Neighbors[*Prev].FindOrAdd(Id); ++Neighbors[Id].FindOrAdd(*Prev);
                Crossings.Add({Key,*Prev,Id});
            }
        }
        else Owner.Add(Key,Id);
    }
    void Add(const FMesh& M,int32 Cluster,bool ByFace)
    {
        TArray<uint32> Ids;
        for (const auto& V : M.V) Ids.Add(Point(V.P));
        FBounds3f Bounds;
        for (int32 F=0; F<M.Faces(); ++F)
        {
            uint32 A=M.I[3*F],B=M.I[3*F+1],C=M.I[3*F+2];
            int32 Id=ByFace?F:Cluster;
            Edge(Ids[A],Ids[B],Id); Edge(Ids[B],Ids[C],Id); Edge(Ids[C],Ids[A],Id);
            if (ByFace) Centers[F]=(M.V[A].P+M.V[B].P+M.V[C].P)/3.f;
            else { Bounds+=M.V[A].P; Bounds+=M.V[B].P; Bounds+=M.V[C].P; }
        }
        if (!ByFace) Centers[Cluster]=FVector3f((Bounds.Min+Bounds.Max)*0.5f);
    }
    TArray<TArray<int32>> Groups(int32 Min,int32 Max,int32 Weight)
    {
        int32 N=Centers.Num();
        TArray<TArray<int32>> Out;
        if (N<=Max) { auto& Ids=Out.AddDefaulted_GetRef(); for (int32 I=0;I<N;++I) Ids.Add(I); return Out; }
        FBounds3f Bounds;
        FDisjointSet Islands(N);
        for (int32 I=0; I<N; ++I)
        {
            Bounds+=Centers[I];
            for (const auto& P:Neighbors[I]) if(P.Key<I) Islands.UnionSequential(I,P.Key);
        }
        FGraphPartitioner Partition(N,Min,Max);
        auto Center=[&](uint32 Id) { return Centers[Id]; };
        Partition.BuildLocalityLinks(Islands,Bounds,{},Center);
        auto* Graph=Partition.NewGraph(N*3);
        for (int32 I=0;I<N;++I)
        {
            Graph->AdjacencyOffset[I]=Graph->Adjacency.Num();
            int32 Id=Partition.Indexes[I];
            Neighbors[Id].KeySort([](int32 A,int32 B){return A<B;});
            for (const auto& P:Neighbors[Id]) Partition.AddAdjacency(Graph,P.Key,P.Value*Weight+(Weight==16?4:0));
            Partition.AddLocalityLinks(Graph,Id,1);
        }
        Graph->AdjacencyOffset[N]=Graph->Adjacency.Num();
        Partition.PartitionStrict(Graph,false);
        for (auto Range:Partition.Ranges)
        {
            auto& Group=Out.AddDefaulted_GetRef();
            for (uint32 I=Range.Begin;I<Range.End;++I) Group.Add(Partition.Indexes[I]);
        }
        return Out;
    }
};

TArray<FMesh> Split(const FMesh& Mesh)
{
    FConnections Graph(Mesh.Faces());
    Graph.Add(Mesh,0,true);
    TArray<FMesh> Out;
    for (const auto& Faces:Graph.Groups(124,128,260))
    {
        auto& M=Out.AddDefaulted_GetRef();
        TMap<uint32,uint32> Remap;
        for (int32 Face:Faces)
        {
            if (!Mesh.Material.IsEmpty()) M.Material.Add(Mesh.Material[Face]);
            for (int32 Corner=0;Corner<3;++Corner)
            {
                const uint32 Id=Mesh.I[3*Face+Corner];
                if (const auto* Local=Remap.Find(Id)) M.I.Add(*Local);
                else { const uint32 New=M.V.Add(Mesh.V[Id]); Remap.Add(Id,New); M.I.Add(New); }
            }
        }
    }
    return Out;
}

FMesh Reduce(FMesh Mesh,const TSet<FVector3f>& Locked,int32 Target,bool Epic)
{
    Mesh.Compact();
    if (Target>=Mesh.Faces()) return Mesh;
    if (!Epic)
    {
        // Meshoptimizer returns vertex indices without face labels. Reduce the
        // slots separately inside this group and preserve every material range.
        if (!Mesh.Material.IsEmpty())
        {
            FMesh Joined;
            for (uint8 Slot : {uint8(0),uint8(1)})
            {
                FMesh Part; Part.V=Mesh.V;
                for (int32 F=0;F<Mesh.Faces();++F) if (Mesh.Material[F]==Slot) Part.I.Append(&Mesh.I[3*F],3);
                if (Part.I.IsEmpty()) continue;
                const int32 Goal=FMath::Max(1,FMath::RoundToInt(float(Target)*Part.Faces()/Mesh.Faces()));
                Part=Reduce(MoveTemp(Part),Locked,Goal,false);
                Part.Material.Init(Slot,Part.Faces());
                Joined.Append(Part);
            }
            return Joined;
        }
        TArray<uint8> Locks;
        for (const auto& V:Mesh.V) Locks.Add(Locked.Contains(V.P)?meshopt_SimplifyVertex_Lock:0);
        const float Weights[]={.003f,.003f,.003f,.01f,.01f,.01f,.01f};
        const int32 Count=meshopt_simplifyWithUpdate(Mesh.I.GetData(),Mesh.I.Num(),&Mesh.V[0].P.X,Mesh.V.Num(),sizeof(FVertex),
            &Mesh.V[0].N.X,sizeof(FVertex),Weights,7,Locks.GetData(),Target*3,1.f,meshopt_SimplifyPermissive|meshopt_SimplifyPrune,nullptr);
        if (Count) Mesh.I.SetNum(Count);
        Mesh.Compact();
        for (auto& V:Mesh.V) V.N.Normalize();
        return Mesh;
    }
    FMesh Work=Mesh;
    float Area=0, UVArea[2]={};
    TArray<int32> Mats;
    for (int32 F=0;F<Mesh.Faces();++F)
    {
        const auto& A=Mesh.V[Mesh.I[F*3]],&B=Mesh.V[Mesh.I[F*3+1]],&C=Mesh.V[Mesh.I[F*3+2]];
        Area+=0.5f*FVector3f::CrossProduct(B.P-A.P,C.P-A.P).Size();
        int32 Flags=Mesh.Material.IsEmpty()?0:Mesh.Material[F];
        for (int32 U=0;U<2;++U)
        {
            float Signed=(B.UV[U]-A.UV[U])^(C.UV[U]-A.UV[U]);
            UVArea[U]+=0.5f*FMath::Abs(Signed);
            if (Signed>=0) Flags|=1<<(24+U);
        }
        Mats.Add(Flags);
    }
    int Exponent=0;
    std::frexp(FMath::Max(1.e-10f,FMath::Sqrt(Area/Mesh.Faces())),&Exponent);
    const float Scale=std::ldexp(1.f,-1-Exponent);
    for (auto& V:Work.V) V.P*=Scale;
    float Weights[]={1,1,1,0,0,0,0};
    for (int32 U=0;U<2;++U) Weights[3+U*2]=Weights[4+U*2]=1.f/(128*FMath::Max(1.e-5f,FMath::Sqrt(UVArea[U]/Mesh.Faces())));
    FMeshSimplifier S(&Work.V[0].P.X,Work.V.Num(),Work.I.GetData(),Work.I.Num(),Mats.GetData(),7);
    for (const auto& P:Locked) S.LockPosition(P*Scale);
    S.SetAttributeWeights(Weights); S.SetEdgeWeight(2.f);
    S.SetCorrectAttributes([](float* A){reinterpret_cast<FVector3f*>(A)->Normalize();});
    S.Simplify(Work.V.Num(),Target,0,0,0,MAX_flt);
    S.PreserveSurfaceArea(); S.Compact();
    Work.V.SetNum(S.GetRemainingNumVerts()); Work.I.SetNum(3*S.GetRemainingNumTris());
    if (!Mesh.Material.IsEmpty())
    {
        Work.Material.Reset();
        for (int32 F=0;F<Work.Faces();++F) Work.Material.Add(uint8(Mats[F]&0xffffff));
    }
    for (auto& V:Work.V) V.P/=Scale;
    return Work.I.IsEmpty()?Mesh:Work;
}

// A batch compares reducers on identical input groups before either arm's
// next repartition. The fully recursive arm also receives its own prior cut.
void Run(const TArray<FString>& Args)
{
    if (Args.Num()!=4) { UE_LOG(LogTemp,Error,TEXT("oak.Bake input.mesh outputDir epic|custom ratio")); return; }
    const bool Epic=Args[2]==TEXT("epic");
    const float Ratio=FCString::Atof(*Args[3]);
    if ((!Epic && Args[2]!=TEXT("custom")) || Ratio<.3f || Ratio>.9f) return;
    FMesh Input;
    if (!ReadMesh(Args[0],Input)) return;
    IFileManager::Get().MakeDirectory(*Args[1],true);
    auto Children=Split(Input);
    for (int32 Level=1;Level<=10 && Children.Num()>1;++Level)
    {
        FConnections Graph(Children.Num());
        for (int32 I=0;I<Children.Num();++I) Graph.Add(Children[I],I,false);
        auto Groups=Graph.Groups(8,32,16);
        TArray<int32> GroupId;
        GroupId.SetNumUninitialized(Children.Num());
        for (int32 G=0;G<Groups.Num();++G) for (int32 I:Groups[G]) GroupId[I]=G;
        TArray<TSet<FVector3f>> Locks; Locks.SetNum(Groups.Num());
        for (const auto& Cross:Graph.Crossings)
            if (GroupId[Cross.A]!=GroupId[Cross.B])
                for (int32 Id:{GroupId[Cross.A],GroupId[Cross.B]})
                {
                    Locks[Id].Add(Graph.Points[uint32(Cross.Edge>>32)]);
                    Locks[Id].Add(Graph.Points[uint32(Cross.Edge)]);
                }
        TArray<FMesh> Next, Paired;
        for (int32 G=0;G<Groups.Num();++G)
        {
            FMesh Pool;
            for (int32 Id:Groups[G]) Pool.Append(Children[Id]);
            const int32 Target=FMath::Max(1,FMath::RoundToInt(Pool.Faces()*Ratio));
            FMesh M=Reduce(Pool,Locks[G],Target,Epic);
            if (Epic) Paired.Add(Reduce(Pool,Locks[G],Target,false));
            Next.Append(Split(M));
        }
        FString Stem=Args[1]/FString::Printf(TEXT("level%02d"),Level);
        if (!WriteLevel(Stem+TEXT(".mesh"),Next) || (Epic && !WriteLevel(Stem+TEXT("-paired.mesh"),Paired))) return;
        Children=MoveTemp(Next);
    }
    FFileHelper::SaveStringToFile(TEXT("complete"),*(Args[1]/TEXT("done.txt")));
}
}

class FOakAssemblyBenchModule final : public IModuleInterface
{
    IConsoleObject* Command = nullptr;
    IConsoleObject* LayoutCommand = nullptr;
    void StartupModule() override
    {
        Command=IConsoleManager::Get().RegisterConsoleCommand(TEXT("oak.Bake"),TEXT("Bake cross-spray triangle replacements"),
            FConsoleCommandWithArgsDelegate::CreateStatic(Run),ECVF_Default);
        LayoutCommand=IConsoleManager::Get().RegisterConsoleCommand(TEXT("oak.ExportLayout"),TEXT("Export resident hierarchy offsets for a GPU readback"),
            FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
            {
                if (Args.Num()!=2) return;
                UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*Args[0]);
                if (!Mesh || !Mesh->GetRenderData() || !Mesh->GetRenderData()->NaniteResourcesPtr) return;
                FlushRenderingCommands();
                const auto& R=*Mesh->GetRenderData()->NaniteResourcesPtr;
                if (R.HierarchyOffset==MAX_uint32 || !R.NumHierarchyNodes || R.HierarchyRootOffsets.IsEmpty()) return;
                FString Text=FString::Printf(TEXT("%u %u %u %u %u"), R.HierarchyOffset,R.NumHierarchyNodes,
                    R.AssemblyTransformOffset,R.NumAssemblyTransforms,R.HierarchyRootOffsets[0]);
                FFileHelper::SaveStringToFile(Text,*Args[1]);
            }),ECVF_Default);
    }
    void ShutdownModule() override
    {
        if (Command) IConsoleManager::Get().UnregisterConsoleObject(Command);
        if (LayoutCommand) IConsoleManager::Get().UnregisterConsoleObject(LayoutCommand);
    }
};
IMPLEMENT_MODULE(FOakAssemblyBenchModule,OakAssemblyBench)
