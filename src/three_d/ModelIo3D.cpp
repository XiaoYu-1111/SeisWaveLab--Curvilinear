#include "SeisCommon/AtomicFile.hpp"
#include "Seis3D/ModelIo3D.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include "SeisCommon/VtsModelIo.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>

namespace Seis3D::Curvilinear {
namespace {
using namespace SeisWave::VtsModel;
Arrays metadata(const ISimulationBackend3D &s, const ComputeSettings3D &compute) {
    const auto &p = s.parameters();
    Arrays a;
    const auto add = [&](const char *name, double value) { a[name] = {value}; };
    add("SeisWaveModel3DVersion", p.surfaceGrid.empty()?1:2);
    add("NumericalSchemeVersion", 1);
#define PARAM(member) add("Parameter_" #member, p.member)
    PARAM(nx);
    PARAM(ny);
    PARAM(nz);
    PARAM(spacing);
    PARAM(vp);
    PARAM(vs);
    PARAM(density);
    PARAM(layeredMaterial);
    PARAM(dominantFrequency);
    PARAM(sourceX);
    PARAM(sourceY);
    PARAM(sourceZ);
    PARAM(sourceForce);
    PARAM(sourceMoment);
    PARAM(duration);
    PARAM(maxSteps);
    PARAM(cflSafety);
    PARAM(topographyAmplitude);
    PARAM(topographyWavelength);
    PARAM(terrainTransitionDepth);
#undef PARAM
    add("LayerDepthMode", static_cast<int>(p.layerDepthMode));
    add("SourceKind", static_cast<int>(p.source.kind));
    a["SourceDirection"] = {p.source.direction[0], p.source.direction[1], p.source.direction[2]};
    auto &layers = a["LayersThicknessVpVsDensity"];
    auto &lengths = a["LayerNameLengths"], &names = a["LayerNamesUtf8"];
    for (const auto &l : p.layers) {
        layers.insert(layers.end(), {l.thickness, l.vp, l.vs, l.density});
        lengths.push_back(static_cast<double>(l.name.size()));
        for (unsigned char c : l.name)
            names.push_back(c);
    }
    const auto &r = p.receiverArray;
    a["ReceiverArray"] = {static_cast<double>(r.kind),    static_cast<double>(r.startX),
                          static_cast<double>(r.startY),  static_cast<double>(r.z),
                          static_cast<double>(r.countX),  static_cast<double>(r.countY),
                          static_cast<double>(r.strideX), static_cast<double>(r.strideY)};
    const auto &b = p.absorbingBoundary;
    a["AbsorbingBoundary"] = {b.enabled ? 1. : 0., static_cast<double>(b.cells[0]),
                              static_cast<double>(b.cells[1]), static_cast<double>(b.cells[2]), b.strength};
    add("BackendKind", static_cast<int>(s.backendInfo().kind));
    add("CpuThreadsRequested", compute.cpuThreads);
    add("CudaDeviceRequested", compute.cudaDevice);
    add("ReferenceDtSeconds", s.dt());
    add("EffectiveTotalSteps", s.totalSteps());
    add("EffectiveFlattenDepthM", s.grid().terrainTransitionDepth);
    add("CoordinatesZPositiveDown", 1);
    if(!p.surfaceGrid.empty()) {
        const auto& dem=p.surfaceGrid;
        a["SurfaceGridGeometry"]={static_cast<double>(dem.nx),static_cast<double>(dem.ny),dem.spacing,dem.originX,dem.originY,dem.datum};
        a["SurfaceGridElevations"]=dem.elevations;
    }
    return a;
}
const char *pointNames[]{"Vp", "Vs", "Density", "LayerId", "Jacobian"};
double valueAt(const SimulationParameters &p, const Grid3D &g, int field, std::size_t q) {
    if (field == 4)
        return g.jacobian[q];
    const int id = p.layeredMaterial ? materialLayerAtDepth3D(p, materialDepth3D(p, g, q)) : 0;
    if (field == 3)
        return id;
    if (p.layeredMaterial) {
        const auto &l = p.layers[id];
        return field == 0 ? l.vp : field == 1 ? l.vs : l.density;
    }
    return field == 0 ? p.vp : field == 1 ? p.vs : p.density;
}
} // namespace
bool writeModelVts3D(const ISimulationBackend3D &s, const std::filesystem::path &path,
                     const ComputeSettings3D &compute, std::string &error) {
    using namespace SeisWave::VtsModel;
    std::filesystem::path pending;
    try {
        require(s.initialized(), "Initialize the 3D model before saving.");
        const auto &p = s.parameters();
        const auto &g = s.grid();
        auto arrays = metadata(s, compute);
        Checksum hash;
        for (const auto &[name, values] : arrays) {
            hash.name(name);
            for (double v : values) {
                require(std::isfinite(v), "Nonfinite model metadata.");
                hash.number(v);
            }
        }
        const auto nodes = [&](const auto &emit) {
            for (int k = 0; k < p.nz; ++k)
                for (int j = 0; j < p.ny; ++j)
                    for (int i = 0; i < p.nx; ++i)
                        emit(g.index(i + g.ghost, j + g.ghost, k + g.ghost));
        };
        for (int f = 0; f < 5; ++f) {
            hash.name(pointNames[f]);
            nodes([&](std::size_t q) { hash.number(valueAt(p, g, f, q)); });
        }
        hash.name("Coordinates");
        nodes([&](std::size_t q) {
            for (double v : g.coordinates[q])
                hash.number(v);
        });
        arrays["ModelChecksum"] = {static_cast<double>(hash.value >> 32),
                                   static_cast<double>(hash.value & 0xffffffffULL)};
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        pending = path;
        pending += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream out(pending);
        out.imbue(std::locale::classic());
        require(static_cast<bool>(out), "Cannot create 3D model VTS file.");
        out << std::setprecision(17)
            << "<?xml version=\"1.0\"?>\n<VTKFile type=\"StructuredGrid\" version=\"0.1\" "
               "byte_order=\"LittleEndian\">\n"
            << "<StructuredGrid WholeExtent=\"0 " << p.nx - 1 << " 0 " << p.ny - 1 << " 0 " << p.nz - 1
            << "\">\n<FieldData>\n";
        for (const auto &[name, values] : arrays) {
            const int components = name == "SourceDirection"              ? 3
                                   : name == "LayersThicknessVpVsDensity" ? 4
                                   : name == "ModelChecksum"              ? 2
                                                                          : 1;
            out << "<DataArray type=\"Float64\" Name=\"" << name << "\" NumberOfComponents=\"" << components
                << "\" NumberOfTuples=\"" << values.size() / components << "\" format=\"ascii\">";
            for (double v : values)
                out << v << ' ';
            out << "</DataArray>\n";
        }
        out << "</FieldData><Piece Extent=\"0 " << p.nx - 1 << " 0 " << p.ny - 1 << " 0 " << p.nz - 1
            << "\"><PointData Scalars=\"Vp\">\n";
        for (int f = 0; f < 5; ++f) {
            out << "<DataArray type=\"Float64\" Name=\"" << pointNames[f] << "\" format=\"ascii\">\n";
            nodes([&](std::size_t q) { out << valueAt(p, g, f, q) << ' '; });
            out << "\n</DataArray>\n";
        }
        out << "</PointData><Points><DataArray type=\"Float64\" Name=\"Coordinates\" "
               "NumberOfComponents=\"3\" format=\"ascii\">\n";
        nodes([&](std::size_t q) {
            for (double v : g.coordinates[q])
                out << v << ' ';
        });
        out << "\n</DataArray></Points></Piece></StructuredGrid></VTKFile>\n";
        out.close();
        require(static_cast<bool>(out), "3D model VTS write failed.");
        SeisWave::replaceCompletedFile(pending, path);
        pending.clear();
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        if (!pending.empty()) {
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
        }
        error = ex.what();
        return false;
    }
}
bool readModelVts3D(const std::filesystem::path &path, ModelDocument3D &document, std::string &error) {
    using namespace SeisWave::VtsModel;
    try {
        std::ifstream in(path);
        in.imbue(std::locale::classic());
        require(static_cast<bool>(in), "Cannot open 3D model VTS.");
        auto head = tag(in);
        if (head.starts_with("<?xml"))
            head = tag(in);
        require(head.starts_with("<VTKFile ") && attribute(head, "type") == "StructuredGrid",
                "Expected a 3D StructuredGrid model VTS.");
        const auto gridHeader = tag(in);
        require(gridHeader.starts_with("<StructuredGrid "), "Missing StructuredGrid.");
        expected(in, "<FieldData>");
        Arrays arrays;
        Checksum hash;
        while (true) {
            const auto a = tag(in);
            if (a == "</FieldData>")
                break;
            require(a.starts_with("<DataArray ") && arrays.size() < 80, "Invalid model metadata.");
            const auto name = attribute(a, "Name");
            require(!arrays.contains(name), "Duplicate model metadata.");
            const auto size = [&](const char *key, int high) {
                std::size_t end{};
                const auto text = attribute(a, key);
                const double v = std::stod(text, &end);
                require(end == text.size(), "Invalid metadata array size.");
                return integer(v, 0, high);
            };
            const int limit=name=="SurfaceGridElevations"?static_cast<int>(maxSurfaceSamples3D):65536;
            const int components = size("NumberOfComponents", 4), tuples = size("NumberOfTuples", limit);
            require(components > 0 && static_cast<std::size_t>(components) * tuples <= static_cast<std::size_t>(limit),
                    "Oversized model metadata.");
            arrays[name] = readArray(in, a, name == "ModelChecksum" ? nullptr : &hash,
                                     static_cast<std::size_t>(components) * tuples, true);
        }
        const auto scalar = [&](const char *key) {
            const auto it = arrays.find(key);
            require(it != arrays.end() && it->second.size() == 1,
                    std::string("Missing model parameter: ") + key);
            return it->second[0];
        };
        const int version=integer(scalar("SeisWaveModel3DVersion"),1,2);
        require(scalar("NumericalSchemeVersion") == 1,
                "Unsupported 3D model schema or numerical scheme.");
        ModelDocument3D next;
        auto &p = next.parameters;
#define REAL(member) p.member = scalar("Parameter_" #member)
#define INT(member, low, high) p.member = integer(scalar("Parameter_" #member), low, high)
        INT(nx, 8, maxPhysicalNodesPerAxis3D);
        INT(ny, 8, maxPhysicalNodesPerAxis3D);
        INT(nz, 8, maxPhysicalNodesPerAxis3D);
        require(static_cast<std::size_t>(p.nx+2*ghostWidth3D)*(p.ny+2*ghostWidth3D)*(p.nz+2*ghostWidth3D)<=maxPaddedNodes3D,
                "Model exceeds the total padded-node budget.");
        REAL(spacing);
        REAL(vp);
        REAL(vs);
        REAL(density);
        REAL(dominantFrequency);
        INT(sourceX, -1, p.nx - 1);
        INT(sourceY, -1, p.ny - 1);
        INT(sourceZ, 0, p.nz - 1);
        REAL(sourceForce);
        REAL(sourceMoment);
        REAL(duration);
        INT(maxSteps, 0, 20000);
        REAL(cflSafety);
        REAL(topographyAmplitude);
        REAL(topographyWavelength);
        REAL(terrainTransitionDepth);
#undef REAL
#undef INT
        p.layeredMaterial = integer(scalar("Parameter_layeredMaterial"), 0, 1) != 0;
        p.layerDepthMode = static_cast<LayerDepthMode3D>(integer(scalar("LayerDepthMode"), 0, 2));
        p.source.kind = static_cast<SeisWave::SourceKind>(integer(scalar("SourceKind"), 0, 1));
        const auto &direction = arrays.at("SourceDirection");
        require(direction.size() == 3, "Invalid source direction.");
        std::copy(direction.begin(), direction.end(), p.source.direction.begin());
        const auto &layers = arrays.at("LayersThicknessVpVsDensity"),
                   &lengths = arrays.at("LayerNameLengths"), &names = arrays.at("LayerNamesUtf8");
        require(layers.size() % 4 == 0 && layers.size() <= 32 && lengths.size() == layers.size() / 4,
                "Invalid layer table.");
        std::size_t cursor = 0;
        p.layers.clear();
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            const auto n = static_cast<std::size_t>(integer(lengths[i], 0, 8192));
            require(n <= names.size() - cursor, "Invalid UTF8 layer-name length.");
            std::string name;
            for (std::size_t c = 0; c < n; ++c)
                name.push_back(static_cast<char>(integer(names[cursor++], 0, 255)));
            p.layers.push_back(
                {name, layers[4 * i], layers[4 * i + 1], layers[4 * i + 2], layers[4 * i + 3]});
        }
        require(cursor == names.size(), "Unused layer-name bytes.");
        const auto &r = arrays.at("ReceiverArray");
        require(r.size() == 8, "Invalid receiver array.");
        p.receiverArray = {static_cast<ReceiverArrayKind3D>(integer(r[0], 0, 3)),
                           integer(r[1], -1, p.nx-1),
                           integer(r[2], -1, p.ny-1),
                           integer(r[3], 0, p.nz-1),
                           integer(r[4], 1, 1024),
                           integer(r[5], 1, 1024),
                           integer(r[6], 1, maxPhysicalNodesPerAxis3D),
                           integer(r[7], 1, maxPhysicalNodesPerAxis3D)};
        const auto &b = arrays.at("AbsorbingBoundary");
        require(b.size() == 5, "Invalid absorbing settings.");
        p.absorbingBoundary = {integer(b[0], 0, 1) != 0,
                               {integer(b[1], 1, 64), integer(b[2], 1, 64), integer(b[3], 1, 64)},
                               b[4]};
        next.backend = static_cast<BackendKind3D>(integer(scalar("BackendKind"), 0, 1));
        next.compute.cpuThreads = integer(scalar("CpuThreadsRequested"), 0, 64);
        next.compute.cudaDevice = integer(scalar("CudaDeviceRequested"), -1, 100000);
        next.referenceDt = scalar("ReferenceDtSeconds");
        require(next.referenceDt > 0 && scalar("CoordinatesZPositiveDown") == 1,
                "Invalid model conventions.");
        if(version==2) {
            const auto& geometry=arrays.at("SurfaceGridGeometry");
            require(geometry.size()==6,"Invalid embedded DEM geometry.");
            auto& dem=p.surfaceGrid;
            dem.nx=integer(geometry[0],2,maxSurfaceAxis3D);dem.ny=integer(geometry[1],2,maxSurfaceAxis3D);
            dem.spacing=geometry[2];dem.originX=geometry[3];dem.originY=geometry[4];dem.datum=geometry[5];
            dem.elevations=std::move(arrays.at("SurfaceGridElevations"));
            require(dem.validate(error),error);
        }else require(!arrays.contains("SurfaceGridGeometry")&&!arrays.contains("SurfaceGridElevations"),"DEM metadata requires model schema 2.");
        const auto extent = "0 " + std::to_string(p.nx - 1) + " 0 " + std::to_string(p.ny - 1) + " 0 " +
                            std::to_string(p.nz - 1);
        require(attribute(gridHeader, "WholeExtent") == extent, "Model/grid extent mismatch.");
        const auto piece = tag(in);
        require(piece.starts_with("<Piece ") && attribute(piece, "Extent") == extent,
                "Invalid piece extent.");
        require(tag(in).starts_with("<PointData"), "Missing model arrays.");
        const auto count = static_cast<std::size_t>(p.nx) * p.ny * p.nz;
        for (const auto *name : pointNames) {
            const auto a = tag(in);
            require(a.starts_with("<DataArray ") && attribute(a, "Name") == name,
                    "Missing model point array.");
            readArray(in, a, &hash, count, false);
        }
        expected(in, "</PointData>");
        expected(in, "<Points>");
        const auto coordinates = tag(in);
        require(attribute(coordinates, "Name") == "Coordinates" &&
                    attribute(coordinates, "NumberOfComponents") == "3",
                "Invalid model coordinates.");
        readArray(in, coordinates, &hash, count * 3, false);
        expected(in, "</Points>");
        expected(in, "</Piece>");
        expected(in, "</StructuredGrid>");
        expected(in, "</VTKFile>");
        in >> std::ws;
        require(in.peek() == std::char_traits<char>::eof(), "Trailing model data.");
        const auto &digest = arrays.at("ModelChecksum");
        require(digest.size() == 2 && digest[0] == static_cast<double>(hash.value >> 32) &&
                    digest[1] == static_cast<double>(hash.value & 0xffffffffULL),
                "Model checksum mismatch.");
        document = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
} // namespace Seis3D::Curvilinear
