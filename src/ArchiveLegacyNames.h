#pragma once
#include <string>
#include <unordered_map>

inline std::string ArchiveReprocessingProject(const std::string& archive) {
    if (archive == "REPRO1") return "R01";
    if (archive == "REPRO2") return "R02";
    if (archive == "REPRO3") return "R03";
    return {};
}

inline std::string ArchiveReprocessingCenter(const std::string& prefix) {
    static const std::unordered_map<std::string,std::string> values{
        {"CO1","COD"},{"CO2","COD"},{"CF1","COD"},{"CF2","COD"},
        {"GF1","GFZ"},{"GF2","GFZ"},{"GR1","GRG"},{"GR2","GRG"},
        {"IG1","IGS"},{"IG2","IGS"},{"IGB","IGS"},
        {"JP1","JPL"},{"JP2","JPL"},{"MI1","MIT"},{"MI2","MIT"},
        {"SI1","SIO"},{"SI2","SIO"},{"EM1","EMR"},{"EM2","EMR"},
        {"ES1","ESA"},{"ES2","ESA"},{"GT2","GFZ"},
        {"PD1","PDR"},{"UL1","ULR"},{"UL2","ULR"}};
    const auto it=values.find(prefix);
    return it==values.end()?prefix:it->second;
}
