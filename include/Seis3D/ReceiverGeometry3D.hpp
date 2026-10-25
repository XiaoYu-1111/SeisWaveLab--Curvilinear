#pragma once
#include "CurvilinearSolver.hpp"
#include <algorithm>
namespace Seis3D::Curvilinear {
inline constexpr int maxReceiverTraces3D=1024;
struct ReceiverLimits3D {int x{},y{},total{},xAtCurrentY{},yAtCurrentX{};};
// Inclusive final node: start + (count - 1) * stride <= N - 1.
inline ReceiverLimits3D receiverLimits3D(const SimulationParameters& p) {
    ReceiverLimits3D out;const auto& a=p.receiverArray;
    if(p.nx<8||p.ny<8||p.nz<8||p.nx>maxPhysicalNodesPerAxis3D||p.ny>maxPhysicalNodesPerAxis3D||p.nz>maxPhysicalNodesPerAxis3D||a.kind<ReceiverArrayKind3D::AutoXLine||a.kind>ReceiverArrayKind3D::GridXY||a.z<0||a.z>=p.nz)return out;
    if(a.kind==ReceiverArrayKind3D::AutoXLine)return {p.nx-4,1,std::min(maxReceiverTraces3D,p.nx-4),std::min(maxReceiverTraces3D,p.nx-4),1};
    const int x=a.startX==-1?p.nx/2:a.startX,y=a.startY==-1?p.ny/2:a.startY;
    if(x<0||x>=p.nx||y<0||y>=p.ny)return out;
    const auto capacity=[](int n,int start,int stride){return stride>0?1+(n-1-start)/stride:0;};
    out.x=a.kind==ReceiverArrayKind3D::YLine?1:capacity(p.nx,x,a.strideX);
    out.y=a.kind==ReceiverArrayKind3D::XLine?1:capacity(p.ny,y,a.strideY);
    out.total=std::min(maxReceiverTraces3D,out.x*out.y);
    const int cx=a.kind==ReceiverArrayKind3D::YLine?1:a.countX,cy=a.kind==ReceiverArrayKind3D::XLine?1:a.countY;
    out.xAtCurrentY=cy>0&&cy<=out.y?std::min(out.x,maxReceiverTraces3D/cy):0;
    out.yAtCurrentX=cx>0&&cx<=out.x?std::min(out.y,maxReceiverTraces3D/cx):0;
    return out;
}
inline bool fitReceiverCounts3D(SimulationParameters& p) {
    const auto limits=receiverLimits3D(p);if(!limits.total)return false;auto& a=p.receiverArray;
    if(a.kind==ReceiverArrayKind3D::AutoXLine)return true;
    a.countX=a.kind==ReceiverArrayKind3D::YLine?1:std::clamp(a.countX,1,limits.x);
    a.countY=a.kind==ReceiverArrayKind3D::XLine?1:std::clamp(a.countY,1,std::min(limits.y,maxReceiverTraces3D/a.countX));
    return true;
}
inline bool receiverNodes3D(const SimulationParameters& p,std::vector<std::array<int,3>>& nodes,std::string& error) {
    nodes.clear();error.clear();const auto& a=p.receiverArray;const auto limits=receiverLimits3D(p);
    if(!limits.total){error="Invalid receiver grid, mode, start, active stride or depth index.";return false;}
    if(a.kind==ReceiverArrayKind3D::AutoXLine){
        if(p.nx-4>maxReceiverTraces3D){error="Auto X line exceeds 1024 traces; select a custom line with a larger receiver stride.";return false;}
        for(int i=2;i<p.nx-2;++i)nodes.push_back({i,p.ny/2,a.z});return true;
    }
    const int cx=a.kind==ReceiverArrayKind3D::YLine?1:a.countX,cy=a.kind==ReceiverArrayKind3D::XLine?1:a.countY;
    const int x=a.startX==-1?p.nx/2:a.startX,y=a.startY==-1?p.ny/2:a.startY;
    if(cx<1||cy<1||cx>limits.x||cy>limits.y||static_cast<std::int64_t>(cx)*cy>maxReceiverTraces3D){error="Receiver counts exceed the start/stride capacity or the 1024-trace total limit. Review capacity or fit counts.";return false;}
    nodes.reserve(cx*cy);for(int j=0;j<cy;++j)for(int i=0;i<cx;++i)nodes.push_back({x+i*(a.kind==ReceiverArrayKind3D::YLine?0:a.strideX),y+j*(a.kind==ReceiverArrayKind3D::XLine?0:a.strideY),a.z});
    return true;
}
}
