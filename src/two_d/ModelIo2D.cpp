#include "SeisCommon/AtomicFile.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "SeisCommon/VtsModelIo.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace Seis2D::Curvilinear {
namespace {
using namespace SeisWave::VtsModel;
Arrays metadata(const Simulation &s, const ComputeSettings3D &compute) {
    const auto &p = s.parameters();
    Arrays a;
    const auto add = [&](const char *name, double value) { a[name] = {value}; };
    const bool pmlSchema=p.absorber!=Absorber::Sponge||p.cpml!=CfsPmlSettings{};
    const bool fp32=p.precision==Precision::Float32;
    const bool physicalSource=p.sourceSigmaM>0;
    const bool surveySchema=p.autoSurfaceReceivers||p.receiverTargetCount!=0;
    add("SeisWaveModel2DVersion",surveySchema?7:physicalSource?6:fp32?5:p.externalAbsorption?4:pmlSchema?3:2);
    if(surveySchema)a["SurfaceReceiverLayout"]={double(p.autoSurfaceReceivers),double(p.receiverTargetCount)};
    if(fp32||physicalSource||surveySchema)add("CalculationPrecision",static_cast<int>(p.precision));
    if(pmlSchema||p.externalAbsorption||fp32||physicalSource||surveySchema)a["CfsPmlSettings"]={double(static_cast<int>(p.absorber)),p.cpml.targetReflection,p.cpml.kappaMax,p.cpml.frequency};
    if(p.externalAbsorption||fp32||physicalSource||surveySchema)add("ExternalAbsorption",p.externalAbsorption);
    if(physicalSource||surveySchema)a["PhysicalSourceKernelM"]={p.sourceSigmaM,p.sourceRadiusM};
    a["GSLSSettings"]={double(p.attenuation.enabled),double(p.attenuation.mechanisms),p.attenuation.minFrequency,p.attenuation.maxFrequency,p.attenuation.referenceFrequency,p.attenuation.qp,p.attenuation.qs};
    auto& layerQ=a["LayerQpQs"];for(const auto& l:p.layers)layerQ.insert(layerQ.end(),{l.qp,l.qs});
#define PARAM(member) add("Parameter_" #member, p.member)
    PARAM(nx); PARAM(nz); PARAM(spacing); PARAM(vp); PARAM(vs); PARAM(density);
    PARAM(relief); PARAM(wavelength); PARAM(flattenDepth); PARAM(frequency); PARAM(duration);
    PARAM(cfl); PARAM(forcePerLength); PARAM(momentPerLength); PARAM(sourceX); PARAM(sourceZ);
    PARAM(maxSteps); PARAM(absorbing); PARAM(spongeX); PARAM(spongeZ); PARAM(spongeStrength);
    PARAM(receiverStart); PARAM(receiverCount); PARAM(receiverStride); PARAM(receiverDepth);
    PARAM(sampleIntervalMs); PARAM(layered); PARAM(importedSurface); PARAM(surfaceDatum);
#undef PARAM
    add("SourceKind", static_cast<int>(p.source.kind));
    a["SourceDirection"] = {p.source.direction[0], p.source.direction[1], p.source.direction[2]};
    auto &layers = a["LayersThicknessVpVsDensity"];
    for (const auto &layer : p.layers)
        layers.insert(layers.end(), {layer.thickness, layer.vp, layer.vs, layer.density});
    auto &samples = a["SurfaceSamplesXElevation"];
    for (const auto &v : p.surfaceSamples) samples.insert(samples.end(), {v[0], v[1]});
    add("BackendKind", static_cast<int>(s.backend().kind));
    add("CpuThreadsRequested", compute.cpuThreads);
    add("CudaDeviceRequested", compute.cudaDevice);
    add("ReferenceDtSeconds", s.dt());
    add("RecordIntervalSeconds", s.recordDt());
    add("EffectiveTotalSteps", s.totalSteps());
    add("EffectiveFlattenDepthM", s.grid().flattenDepth);
    add("CoordinatesZPositiveDown", 1);
    return a;
}
const char *pointNames[]{"Vp", "Vs", "Density", "Jacobian", "MetricA", "MetricB"};
double pointValue(const Grid &g, int f, std::size_t q) {
    switch (f) {
    case 0: return g.vp[q];
    case 1: return g.vs[q];
    case 2: return g.density[q];
    case 3: return g.jacobian[q];
    case 4: return g.metricA[q];
    default: return g.metricB[q];
    }
}
} // namespace

bool writeModelVts(const Simulation &s, const std::filesystem::path &path,
                   const ComputeSettings3D &compute, std::string &error) {
    std::filesystem::path pending;
    try {
        require(s.initialized(), "Initialize the 2D model before saving.");
        const auto &p = s.parameters();
        const auto &g = s.grid();
        const char* pointType=p.precision==Precision::Float32?"Float32":"Float64";
        auto arrays = metadata(s, compute);
        Checksum hash;
        for (const auto &[name, values] : arrays) {
            hash.name(name);
            for (double value : values) {
                require(std::isfinite(value), "Cannot save nonfinite model metadata.");
                hash.number(value);
            }
        }
        const auto nodes = [&](const auto &emit) {
            for (int k = 0; k < p.nz; ++k)
                for (int i = 0; i < p.nx; ++i) emit(g.modelIndex(i,k));
        };
        for (int f = 0; f < 6; ++f) {
            hash.name(pointNames[f]);
            nodes([&](std::size_t q) { hash.number(pointValue(g, f, q)); });
        }
        hash.name("Coordinates");
        nodes([&](std::size_t q) { hash.number(g.coordinates[q][0]); hash.number(0); hash.number(g.coordinates[q][1]); });
        arrays["ModelChecksum"] = {static_cast<double>(hash.value >> 32),
                                    static_cast<double>(hash.value & 0xffffffffULL)};
        pending = path;
        pending += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream out(pending);
        require(static_cast<bool>(out), "Cannot create 2D model VTS file.");
        out.imbue(std::locale::classic());
        out << std::setprecision(17)
            << "<?xml version=\"1.0\"?>\n<VTKFile type=\"StructuredGrid\" version=\"0.1\" byte_order=\"LittleEndian\">\n"
            << "<StructuredGrid WholeExtent=\"0 " << p.nx - 1 << " 0 0 0 " << p.nz - 1 << "\">\n<FieldData>\n";
        for (const auto &[name, values] : arrays) {
            const int components = name == "SourceDirection" ? 3 : name == "LayersThicknessVpVsDensity" ? 4
                                   : name == "SurfaceSamplesXElevation" || name == "ModelChecksum" ? 2 : 1;
            out << "<DataArray type=\"Float64\" Name=\"" << name << "\" NumberOfComponents=\"" << components
                << "\" NumberOfTuples=\"" << values.size() / components << "\" format=\"ascii\">";
            for (double v : values) { require(std::isfinite(v), "Cannot save nonfinite model metadata."); out << v << ' '; }
            out << "</DataArray>\n";
        }
        out << "</FieldData>\n<Piece Extent=\"0 " << p.nx - 1 << " 0 0 0 " << p.nz - 1
            << "\"><PointData Scalars=\"Vp\">\n";
        for (int f = 0; f < 6; ++f) {
            out << "<DataArray type=\""<<pointType<<"\" Name=\"" << pointNames[f] << "\" format=\"ascii\">\n";
            nodes([&](std::size_t q) { out << pointValue(g, f, q) << ' '; });
            out << "\n</DataArray>\n";
        }
        out << "</PointData><Points><DataArray type=\""<<pointType<<"\" Name=\"Coordinates\" NumberOfComponents=\"3\" format=\"ascii\">\n";
        nodes([&](std::size_t q) { out << g.coordinates[q][0] << " 0 " << g.coordinates[q][1] << ' '; });
        out << "\n</DataArray></Points></Piece></StructuredGrid></VTKFile>\n";
        out.close();
        require(static_cast<bool>(out), "2D model VTS write failed.");
        // Same-directory replace only after a complete write, preserving an existing model on failure.
        SeisWave::replaceCompletedFile(pending, path);
        pending.clear();
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        if (!pending.empty()) {
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
        }
        error = ex.what(); return false;
    }
}

bool readModelVts(const std::filesystem::path &path, ModelDocument &document, std::string &error) {
    try {
        std::ifstream in(path);
        in.imbue(std::locale::classic());
        require(static_cast<bool>(in), "Cannot open model VTS file.");
        auto head = tag(in);
        if (head.starts_with("<?xml")) head = tag(in);
        require(head.starts_with("<VTKFile ") && attribute(head, "type") == "StructuredGrid",
                "Expected a SeisWave 2D StructuredGrid model VTS.");
        const auto gridHeader = tag(in);
        require(gridHeader.starts_with("<StructuredGrid "), "Missing StructuredGrid.");
        expected(in, "<FieldData>");
        Arrays arrays;
        Checksum hash;
        while (true) {
            const auto a = tag(in);
            if (a == "</FieldData>") break;
            require(a.starts_with("<DataArray ") && arrays.size() < 80, "Invalid model metadata.");
            const auto name = attribute(a, "Name");
            require(!arrays.contains(name), "Duplicate model metadata: " + name);
            const auto parsedInt = [&](const char *key, int high) {
                const auto text = attribute(a, key);
                std::size_t end{};
                const auto value = std::stod(text, &end);
                require(end == text.size(), "Invalid array size.");
                return integer(value, 0, high);
            };
            const int components = parsedInt("NumberOfComponents", 4), tuples = parsedInt("NumberOfTuples", 1000000);
            require(components > 0 && static_cast<std::size_t>(components) * tuples <= 2000000,
                    "Oversized model metadata.");
            arrays[name] = readArray(in, a, name == "ModelChecksum" ? nullptr : &hash,
                                      static_cast<std::size_t>(components) * tuples, true);
        }
        const auto scalar = [&](const char *key) {
            const auto it = arrays.find(key);
            require(it != arrays.end() && it->second.size() == 1, std::string("Missing model parameter: ") + key);
            return it->second[0];
        };
        const int version=integer(scalar("SeisWaveModel2DVersion"),1,7);
        ModelDocument next;
        auto &p = next.parameters;
#define REAL(member) p.member = scalar("Parameter_" #member)
#define INT(member, low, high) p.member = integer(scalar("Parameter_" #member), low, high)
        INT(nx, 8, maxAxisNodes); INT(nz, 8, maxAxisNodes);
        REAL(spacing); REAL(vp); REAL(vs); REAL(density); REAL(relief); REAL(wavelength);
        REAL(flattenDepth); REAL(frequency); REAL(duration); REAL(cfl); REAL(forcePerLength);
        REAL(momentPerLength); INT(sourceX, -1, p.nx - 1); INT(sourceZ, 0, p.nz - 1);
        INT(maxSteps, 0, maxTimeSteps); INT(spongeX, 1, maxSpongeCells); INT(spongeZ, 1, maxSpongeCells); REAL(spongeStrength);
        INT(receiverStart, 0, p.nx - 1); INT(receiverCount, 1, 1024); INT(receiverStride, 1, maxAxisNodes);
        INT(receiverDepth, 0, p.nz - 1); REAL(sampleIntervalMs); REAL(surfaceDatum);
        p.absorbing = integer(scalar("Parameter_absorbing"), 0, 1) != 0;
        p.layered = integer(scalar("Parameter_layered"), 0, 1) != 0;
        p.importedSurface = integer(scalar("Parameter_importedSurface"), 0, 1) != 0;
#undef REAL
#undef INT
        p.source.kind = static_cast<SeisWave::SourceKind>(integer(scalar("SourceKind"), 0, 1));
        const auto &direction = arrays.at("SourceDirection");
        require(direction.size() == 3, "Invalid source direction.");
        std::copy(direction.begin(), direction.end(), p.source.direction.begin());
        if(version>=6){const auto& kernel=arrays.at("PhysicalSourceKernelM");require(kernel.size()==2&&
            ((version>=7&&kernel[0]==0&&kernel[1]==0)||(kernel[0]>0&&kernel[1]>=kernel[0]&&p.source.kind==SeisWave::SourceKind::DirectionalForce)),"Invalid physical source kernel metadata.");p.sourceSigmaM=kernel[0];p.sourceRadiusM=kernel[1];}
        if(version>=7){const auto& layout=arrays.at("SurfaceReceiverLayout");require(layout.size()==2,"Invalid surface receiver policy.");
            p.autoSurfaceReceivers=integer(layout[0],0,1)!=0;p.receiverTargetCount=integer(layout[1],0,1024);}
        const auto &layers = arrays.at("LayersThicknessVpVsDensity");
        require(layers.size() % 4 == 0 && layers.size() <= 64, "Invalid model layer table.");
        p.layers.clear();
        for (std::size_t i = 0; i < layers.size(); i += 4) p.layers.push_back({layers[i], layers[i+1], layers[i+2], layers[i+3]});
        if(version>=2){const auto& q=arrays.at("GSLSSettings");require(q.size()==7,"Invalid GSLS settings.");p.attenuation={integer(q[0],0,1)!=0,integer(q[1],1,3),q[2],q[3],q[4],q[5],q[6]};
            require(SeisWave::Rheology::validateSettings(p.attenuation,error),error.c_str());const auto& layerQ=arrays.at("LayerQpQs");require(layerQ.size()==2*p.layers.size(),"Layer Q table differs from material table.");
            for(std::size_t i=0;i<p.layers.size();++i){p.layers[i].qp=layerQ[2*i];p.layers[i].qs=layerQ[2*i+1];}}
        if(version>=5)p.precision=static_cast<Precision>(integer(scalar("CalculationPrecision"),0,1));
        if(version>=4)p.externalAbsorption=integer(scalar("ExternalAbsorption"),0,1)!=0;
        if(version>=3){const auto& a=arrays.at("CfsPmlSettings");require(a.size()==4,"Invalid CFS-PML metadata.");p.absorber=static_cast<Absorber>(integer(a[0],0,1));p.cpml={a[1],a[2],a[3]};
            require(p.cpml.targetReflection>=1e-8&&p.cpml.targetReflection<=.1&&p.cpml.kappaMax>=1&&p.cpml.kappaMax<=20&&p.cpml.frequency>=0&&p.cpml.frequency<=1e6,"Invalid CFS-PML profile settings.");}
        const auto &samples = arrays.at("SurfaceSamplesXElevation");
        require(samples.size() % 2 == 0, "Invalid model surface samples.");
        p.surfaceSamples.clear();
        for (std::size_t i = 0; i < samples.size(); i += 2) p.surfaceSamples.push_back({samples[i], samples[i+1]});
        next.compute.cpuThreads = integer(scalar("CpuThreadsRequested"), 0, 64);
        next.compute.cudaDevice = integer(scalar("CudaDeviceRequested"), -1, 100000);
        next.backend = static_cast<BackendKind3D>(integer(scalar("BackendKind"), 0, 1));
        next.referenceDt = scalar("ReferenceDtSeconds");
        require(next.referenceDt > 0 && scalar("CoordinatesZPositiveDown") == 1, "Invalid model time step / coordinate convention.");
        const auto extent = "0 " + std::to_string(p.nx - 1) + " 0 0 0 " + std::to_string(p.nz - 1);
        require(attribute(gridHeader, "WholeExtent") == extent, "Model dimensions disagree with VTS extent.");
        const auto piece = tag(in);
        require(piece.starts_with("<Piece ") && attribute(piece, "Extent") == extent, "Invalid model piece extent.");
        require(tag(in).starts_with("<PointData"), "Missing point material data.");
        const auto count = static_cast<std::size_t>(p.nx) * p.nz;
        for (const auto *name : pointNames) {
            const auto a = tag(in);
            require(a.starts_with("<DataArray ") && attribute(a, "Name") == name, "Missing model point array.");
            readArray(in, a, &hash, count, false,version>=5);
        }
        expected(in, "</PointData>"); expected(in, "<Points>");
        const auto coordinates = tag(in);
        require(attribute(coordinates, "Name") == "Coordinates" && attribute(coordinates, "NumberOfComponents") == "3", "Invalid model coordinates.");
        readArray(in, coordinates, &hash, count * 3, false,version>=5);
        expected(in, "</Points>"); expected(in, "</Piece>"); expected(in, "</StructuredGrid>"); expected(in, "</VTKFile>");
        in >> std::ws;
        require(in.peek() == std::char_traits<char>::eof(), "Unexpected trailing VTS content.");
        const auto &digest = arrays.at("ModelChecksum");
        require(digest.size() == 2 && digest[0] == static_cast<double>(hash.value >> 32) &&
                    digest[1] == static_cast<double>(hash.value & 0xffffffffULL), "Model checksum mismatch (file changed or damaged).");
        document = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &ex) { error = ex.what(); return false; }
}
} // namespace Seis2D::Curvilinear
