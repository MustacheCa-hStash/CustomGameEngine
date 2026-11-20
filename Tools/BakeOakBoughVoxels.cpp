#include "VoxelBake.h"
#include "VoxelCoverageOptimizer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <queue>

namespace fs = std::filesystem;
template<class T> void read(std::istream& in, T* p, size_t n = 1)
{
    in.read(reinterpret_cast<char*>(p), sizeof(T) * n);
}
template<class T> void write(std::ostream& out, const T* p, size_t n = 1)
{
    out.write(reinterpret_cast<const char*>(p), sizeof(T) * n);
}

void sample(fs::path path, unsigned slot, double edge, std::ostream& out)
{
    std::ifstream in(path,std::ios::binary);
    in.exceptions(std::ios::failbit | std::ios::badbit);
    uint32_t nv, ni; read(in,&nv); read(in,&ni);
    if (ni % 3 || fs::file_size(path) != 8ull+nv*sizeof(Vertex)+ni*4ull) throw std::runtime_error("Invalid mesh");
    MeshData mesh;
    mesh.vertices.resize(nv); mesh.indices.resize(ni);
    read(in,mesh.vertices.data(),nv); read(in,mesh.indices.data(),ni);
    std::ifstream materials(path.replace_extension(".mat"),std::ios::binary);
    materials.exceptions(std::ios::failbit | std::ios::badbit);
    std::vector<uint8_t> slots(ni/3); read(materials,slots.data(),slots.size());
    size_t kept=0;
    for (size_t i=0;i<slots.size();++i)
        if (slots[i]==slot)
            for (unsigned j=0;j<3;++j) mesh.indices[kept++]=mesh.indices[3*i+j];
    mesh.indices.resize(kept);
    const auto bake=sampleVoxelCoverage(mesh,edge,8);
    for (const auto& cell:bake.cells)
    {
        write(out,cell.coordinate.data(),3);
        const float coverage=static_cast<float>(bake.meanCoverage(cell));
        write(out,&coverage);
    }
}
// Lowest coverage leaves first. Transfer to 26 live neighbors by estimated
// projected area of the source normal and combine overlapping probabilities.
void redistribute(const fs::path& path, std::ostream& out)
{
    struct Cell { int p[3];float coverage,n[3]; };
    static_assert(sizeof(Cell)==28);
    std::ifstream in(path,std::ios::binary);in.exceptions(std::ios::failbit | std::ios::badbit);
    std::vector<Cell> cells(fs::file_size(path)/sizeof(Cell));read(in,cells.data(),cells.size());
    const auto key=[](int x,int y,int z) {
        return (uint64_t(x)&0x1fffff) | ((uint64_t(y)&0x1fffff)<<21) | ((uint64_t(z)&0x1fffff)<<42);
    };
    using Entry=std::pair<float,size_t>;
    std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> heap;
    std::unordered_map<uint64_t,size_t> live;
    double sum=0;
    for(size_t i=0;i<cells.size();++i)
    {
        const auto& c=cells[i]; if(c.coverage<=0) continue;
        heap.emplace(c.coverage,i);live.emplace(key(c.p[0],c.p[1],c.p[2]),i);sum+=c.coverage;
    }
    while(live.size()>sum && !heap.empty())
    {
        auto [coverage,i]=heap.top();heap.pop();auto& c=cells[i];
        if(c.coverage!=coverage) continue;
        live.erase(key(c.p[0],c.p[1],c.p[2]));c.coverage=-1;
        Entry neighbors[26];size_t count=0;float total=0;
        for(int z=-1;z<=1;++z) for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
        {
            auto found=live.find(key(c.p[0]+x,c.p[1]+y,c.p[2]+z));
            if(found==live.end()) continue;
            float weight=std::abs(c.n[0]*x+c.n[1]*y+c.n[2]*z)/std::sqrt(float(x*x+y*y+z*z));
            if(weight>0) {neighbors[count++]={weight,found->second};total+=weight;}
        }
        for(size_t j=0;j<count;++j)
        {
            auto [weight,k]=neighbors[j];auto& dst=cells[k].coverage;
            dst+=(1-dst)*coverage*(weight/total);heap.emplace(dst,k);
        }
    }
    for(const auto& c:cells) write(out,&c.coverage);
}
void fit(const fs::path& path, std::ostream& out)
{
    std::ifstream in(path,std::ios::binary);
    in.exceptions(std::ios::failbit | std::ios::badbit);
    uint32_t nb,budget,nv;read(in,&nb);read(in,&budget);read(in,&nv);
    VoxelFoliage eligible{}, seed{};
    for (unsigned i=0;i<nb;++i)
    {
        VoxelBrick brick; read(in,brick.coordinate.data(),3);read(in,&brick.occupancy);
        eligible.bricks.push_back(brick); read(in,&brick.occupancy);
        if (brick.occupancy) seed.bricks.push_back(brick);
    }
    std::vector<VoxelCoverageView> views(nv);
    for (auto& view:views)
    {
        uint32_t np,nr;read(in,&np);read(in,&nr);
        view.samplesPerPixel=1; view.sourceCoverage.resize(np); read(in,view.sourceCoverage.data(),np);
        view.cellRays.resize(64ull*nb);
        for (unsigned i=0;i<nr;++i)
        {
            uint32_t cell,pixel; read(in,&cell);read(in,&pixel);
            if (pixel>=np) throw std::runtime_error("Invalid training pixel");
            view.cellRays.at(cell).push_back(pixel);
        }
    }
    auto result=fitVoxelCoverage(eligible,seed,views,budget);
    for (auto& brick:result.foliage.bricks) {write(out,brick.coordinate.data(),3);write(out,&brick.occupancy);}
}
int main(int argc,char** argv)
{
    try
    {
        if (argc<4) throw std::runtime_error("sample mesh slot edge output | fit input output");
        std::ofstream out(argv[argc-1],std::ios::binary);out.exceptions(std::ios::failbit | std::ios::badbit);
        if (std::string_view(argv[1])=="sample" && argc==6) sample(argv[2],std::stoul(argv[3]),std::stod(argv[4]),out);
        else if (std::string_view(argv[1])=="fit" && argc==4) fit(argv[2],out);
        else if (std::string_view(argv[1])=="redistribute" && argc==4) redistribute(argv[2],out);
        else throw std::runtime_error("Invalid oak bake arguments");
    }
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
