#pragma once
#include <array>
#include <complex>
#include <string>
namespace SeisWave::Rheology {
inline constexpr int maxMechanisms=3;
struct Settings {
    bool enabled{};
    int mechanisms{3};
    double minFrequency{5},maxFrequency{100},referenceFrequency{30},qp{100},qs{60};
    bool operator==(const Settings&)const=default;
};
struct Material {
    int mechanisms{};
    double pRelaxed{},sRelaxed{},pUnrelaxed{},sUnrelaxed{},maxRelativeQError{};
    std::array<double,maxMechanisms> rates{},deltaP{},deltaS{};
};
bool validateSettings(const Settings&,std::string&);
bool fitMaterial(double vp,double vs,double density,double qp,double qs,const Settings&,Material&,std::string&);
// Laplace-domain modulus C(s)=C_R+sum Delta_C*s/(s+rate).
std::complex<double> modulus(const Material&,bool shear,std::complex<double> s);
double qualityFactor(const Material&,bool shear,double frequency);
double phaseVelocity(const Material&,bool shear,double density,double frequency);
}
