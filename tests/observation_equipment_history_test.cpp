#include "ObservationEquipmentHistory.h"
#include <iostream>
#include <stdexcept>

void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main() {
    try {
        const std::vector<ObservationEquipmentEpoch> equipment{
            {0,10,20,"RX","one","f1"}, {0,20,31,"RX","two","f1"},
            {0,31,51,"RX","two","f2"},
            {1,10,25,"ANT","a","","NONE","BAM"},
            {1,25,51,"ANT","b","","SCIS","BAM"}};
        const std::vector<ObservationCodeEpoch> codes{
            {10,22,"G:C1C"},{22,41,"G:C1C"}, // Same value across an importer boundary.
            {12,24,"C:C2I"},{14,24,"C:C2I"}, // Overlapping evidence must not split or duplicate.
            {44,51,"G:C1C"}};
        const auto periods = BuildObservationEquipmentHistory(equipment,codes,1,50);
        Check(periods.size()==8,"Unexpected combined boundary count");
        Check(periods.front().firstOrdinal==10,"Invented equipment before first known epoch");
        Check(periods[1].firstOrdinal==12 && periods[1].lastOrdinal==19,"Duplicate signal evidence split identical states");
        Check(periods[2].firstOrdinal==20 && periods[2].lastOrdinal==23,"Receiver transition or same-code merge incorrect");
        Check(periods[2].receiverSerial=="two" && periods[2].antennaSerial=="a","Receiver and antenna not combined");
        Check(periods[4].radome=="SCIS" && periods[4].firstOrdinal==25,"Antenna transition missing");
        Check(periods[5].firmware=="f2" && periods[5].firstOrdinal==31,"Firmware transition missing");
        Check(periods[6].signals.empty() && periods[6].firstOrdinal==41 && periods[6].lastOrdinal==43,"Unknown signal gap extrapolated");
        for(std::size_t i=1;i<periods.size();++i) {
            Check(periods[i-1].lastOrdinal+1==periods[i].firstOrdinal,"Off-by-one gap");
            Check(periods[i-1].Values()!=periods[i].Values(),"Identical neighbours not merged");
        }
        Check(BuildObservationEquipmentHistory({}, {},1,10).empty(),"Empty history invented");
        Check(BuildObservationEquipmentHistory(equipment,codes,21,23).size()==1,"Clipped interval incorrect");
        std::cout << "Combined history: receiver, firmware, antenna, signals, gaps, deduplication and inclusive endpoints passed.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
