#include "SeisCommon/Gsls.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: seis_gsls_fit_curves materials.csv output_directory");
    const auto path=[](const char* p){const std::string s=p;return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),s.size()));};
    const auto input=path(argv[1]),output=std::filesystem::absolute(path(argv[2])).lexically_normal();
    const auto relative=output.lexically_relative(std::filesystem::path(SEIS3D_OUTPUT_ROOT));
    if(relative.empty()||relative=="."||*relative.begin()=="..")throw std::runtime_error("Output must remain below project out");
    std::ifstream in(input);if(!in)throw std::runtime_error("Cannot open material CSV");
    std::filesystem::create_directories(output);
    std::ofstream curves(output/"gsls_fit_curves.csv"),branches(output/"gsls_branch_coefficients.csv");
    curves<<std::setprecision(17)<<"layer,frequency_hz,target_qp,target_qs,fitted_qp,fitted_qs,vp_phase_m_s,vs_phase_m_s,vp_ref_m_s,vs_ref_m_s\n";
    branches<<std::setprecision(17)<<"layer,branch,rate_rad_s,delta_M_Pa,delta_mu_Pa,M_relaxed_Pa,mu_relaxed_Pa\n";
    SeisWave::Rheology::Settings settings;settings.enabled=true;settings.mechanisms=3;
    settings.minFrequency=3;settings.maxFrequency=45;settings.referenceFrequency=15;
    std::string line;std::getline(in,line);int count=0;double maxError=0;
    while(std::getline(in,line)){
        if(line.empty())continue;
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int layer;double thickness,vp,vs,rho,qp,qs;std::string extra;
        if(!(row>>layer>>thickness>>vp>>vs>>rho>>qp>>qs)||(row>>extra))throw std::runtime_error("Expected seven numeric material columns");
        SeisWave::Rheology::Material m;std::string error;
        if(!SeisWave::Rheology::fitMaterial(vp,vs,rho,qp,qs,settings,m,error))throw std::runtime_error(error);
        maxError=std::max(maxError,m.maxRelativeQError);++count;
        for(int l=0;l<m.mechanisms;++l)branches<<layer<<','<<l<<','<<m.rates[l]<<','<<m.deltaP[l]<<','<<m.deltaS[l]<<','<<m.pRelaxed<<','<<m.sRelaxed<<'\n';
        std::vector<double> frequencies{15};
        for(int i=0;i<=320;++i)frequencies.push_back(3*std::pow(15.,i/320.));
        std::sort(frequencies.begin(),frequencies.end());
        for(double f:frequencies)curves<<layer<<','<<f<<','<<qp<<','<<qs<<','
            <<SeisWave::Rheology::qualityFactor(m,false,f)<<','<<SeisWave::Rheology::qualityFactor(m,true,f)<<','
            <<SeisWave::Rheology::phaseVelocity(m,false,rho,f)<<','<<SeisWave::Rheology::phaseVelocity(m,true,rho,f)<<','<<vp<<','<<vs<<'\n';
    }
    curves.close();branches.close();if(!curves||!branches||!count)throw std::runtime_error("Empty material set or write failure");
    std::cout<<"Materials="<<count<<" max_relative_Q_fit_error="<<maxError<<"; same GSLS fitter as the solver, not an independent propagation benchmark\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
