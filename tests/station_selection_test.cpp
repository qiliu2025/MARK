#include "StationSelection.h"
#include "DateUtil.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <memory>
#include <numeric>
#include <iostream>
#include <string>
#include <vector>

namespace {

StationFile Station(
    std::string id,
    const double latitude,
    const double longitude) {
    StationFile station;
    station.stationId = std::move(id);
    station.hasCoordinate = true;
    station.latitude = latitude;
    station.longitude = longitude;
    return station;
}

std::size_t SelectedCount(const std::vector<StationFile>& stations) {
    return static_cast<std::size_t>(std::count_if(
        stations.begin(),
        stations.end(),
        [](const StationFile& station) { return station.selected; }));
}

}  // namespace

namespace {
void Availability(StationFile& station, int first, const std::vector<int>& available,
    const std::vector<int>& unknown = {}, int length = 4) {
    auto coverage = std::make_shared<CalendarCoverage>();
    coverage->firstOrdinal = first; coverage->lastOrdinal = first + length - 1;
    CalendarBitmapBlock known{first, std::vector<std::uint64_t>((length + 63) / 64)};
    auto provisional = known;
    for (int day : available) known.words[day / 64] |= std::uint64_t{1} << (day % 64);
    for (int day : unknown) provisional.words[day / 64] |= std::uint64_t{1} << (day % 64);
    coverage->available.push_back(std::move(known));
    coverage->unverified.push_back(std::move(provisional));
    station.calendarCoverage = coverage;
}
bool CoverageTests() {
    const int first = CalendarOrdinal(2020, 1, 1);
    StationSelectionPlanOptions options;
    options.firstOrdinal = first; options.lastOrdinal = first + 3;
    options.spatial = {2, 50, 0, false, 12345};
    options.minimumDailyStations = 1;
    std::vector<StationFile> stations{Station("AAAA00AAA", 0, 0), Station("BBBB00BBB", 0, 180), Station("CCCC00CCC", 0, 90)};
    Availability(stations[0],first,{0,1}); Availability(stations[1],first,{0,1}); Availability(stations[2],first,{2,3});
    stations[0].selected=true;
    const auto complementary=GenerateStationSelectionPlan(stations,{0,1,2},options);
    if(!complementary.success||complementary.selected!=2||complementary.minimumAvailable<1
        ||complementary.minimumDistanceKm<50||!stations[0].selected||stations[2].selected) {
        std::cerr<<"FAIL: complementary daily coverage and non-mutating generation\n";return false;
    }
    const auto bad=EvaluateStationSelectionPlan(stations,{0,1,2},{0,1},options);
    if(bad.success||bad.failingDays!=2||bad.days.size()!=4||bad.activeDays!=4) {
        std::cerr<<"FAIL: missing dates retained in selected-set report\n";return false;
    }
    options.eligibleDatesKnown=true;options.eligibleOrdinals={first,first+1};
    const auto filtered=EvaluateStationSelectionPlan(stations,{0,1,2},{0,1},options);
    if(!filtered.success||filtered.activeDays!=2||filtered.excludedDays!=2||filtered.days[2].eligible) {
        std::cerr<<"FAIL: only explicit date exclusions change the calendar\n";return false;
    }
    options.eligibleDatesKnown=false;options.minimumDailyStations=2;
    const auto shortage=GenerateStationSelectionPlan(stations,{0,1,2},options);
    if(shortage.success||!shortage.provenInfeasible||shortage.status!=L"coverage-shortage") {
        std::cerr<<"FAIL: necessary candidate coverage bound\n";return false;
    }
    Availability(stations[1],first,{0,1},{2,3});
    const auto provisional=GenerateStationSelectionPlan(stations,{0,1,2},options);
    if(provisional.success||provisional.provenInfeasible||provisional.status!=L"unverified-coverage") {
        std::cerr<<"FAIL: provisional observations are not confirmed coverage\n";return false;
    }
    options.minimumDailyStations=1;options.spatial.requestedCount=1;options.spatial.minimumDistanceKm=0;
    std::vector<StationFile> mirrors{Station("AAAA00AAA",0,0),Station("AAAA00AAA",0,0)};
    Availability(mirrors[0],first,{0,1});Availability(mirrors[1],first,{2,3});
    const auto unionResult=EvaluateStationSelectionPlan(mirrors,{0,1},{0,1},options);
    if(!unionResult.success||unionResult.selected!=1||unionResult.minimumAvailable!=1) {
        std::cerr<<"FAIL: same physical station union without double counting\n";return false;
    }
    Availability(mirrors[0],first,{0,1,2,3},{2});
    const auto unknownPrecedence=EvaluateStationSelectionPlan(mirrors,{0},{0},options);
    if(unknownPrecedence.success||unknownPrecedence.days[2].available!=0||unknownPrecedence.days[2].unknown!=1) {
        std::cerr<<"FAIL: unverified flag takes precedence within a source\n";return false;
    }
    options.spatial.requestedCount=4;options.minimumDailyStations=0;
    options.spatial.maximumNeighborDistanceKm=40;
    std::vector<StationFile> islands{Station("A",0,0),Station("B",0,.1),Station("C",0,100),Station("D",0,100.1)};
    for(auto& s:islands)Availability(s,first,{0,1,2,3});
    const auto disconnected=EvaluateStationSelectionPlan(islands,{0,1,2,3},{0,1,2,3},options);
    if(disconnected.success||disconnected.neighborSatisfied||disconnected.maximumNearestKm>40) {
        std::cerr<<"FAIL: regional connectivity is stronger than a nearest-neighbor check\n";return false;
    }
    options.spatial.maximumNeighborDistanceKm=0;options.spatial.minimumDistanceKm=50;
    const auto crowded=EvaluateStationSelectionPlan(islands,{0,1,2,3},{0,1,2,3},options);
    if(crowded.success||crowded.spacingSatisfied) {std::cerr<<"FAIL: manual selection spacing verification\n";return false;}
    options.spatial={2,0,0,true,98765};options.minimumDailyStations=1;
    for(auto& s:stations)Availability(s,first,{0,1,2,3});
    const auto randomA=GenerateStationSelectionPlan(stations,{0,1,2},options);
    const auto randomB=GenerateStationSelectionPlan(stations,{0,1,2},options);
    if(!randomA.success||randomA.selectedIndices!=randomB.selectedIndices||randomA.randomSeed!=98765) {
        std::cerr<<"FAIL: seeded constrained random selection reproducibility\n";return false;
    }
    options.spatial.requestedCount=5;
    if(GenerateStationSelectionPlan(stations,{0,1,2},options).status!=L"candidate-shortage") {
        std::cerr<<"FAIL: exact target count feasibility\n";return false;
    }
    options.spatial.requestedCount=2;
    options.spatial.minimumDistanceKm=20050;
    const auto jointFailure=GenerateStationSelectionPlan(stations,{0,1,2},options);
    if(jointFailure.success||jointFailure.provenInfeasible||jointFailure.status!=L"not-found") {
        std::cerr<<"FAIL: bounded spatial/coverage search failure is not proof of infeasibility\n";return false;
    }
    options.spatial.minimumDistanceKm=0;
    if(EvaluateStationSelectionPlan(stations,{0,1},{0,2},options).status!=L"outside-scope") {
        std::cerr<<"FAIL: current candidate scope verification\n";return false;
    }
    std::stop_source cancelled;cancelled.request_stop();
    if(GenerateStationSelectionPlan(stations,{0,1,2},options,cancelled.get_token()).status!=L"cancelled") {
        std::cerr<<"FAIL: cancellation\n";return false;
    }
    options.eligibleDatesKnown=true;options.eligibleOrdinals.clear();
    if(EvaluateStationSelectionPlan(stations,{0,1,2},{0,1},options).status!=L"no-dates") {
        std::cerr<<"FAIL: empty explicit evaluation calendar\n";return false;
    }
    // Five years, thousands of candidates, one fixed 50-station set. All
    // stations omit different dates; the selected set must preserve >=35.
    options.eligibleDatesKnown=false;options.lastOrdinal=first+1825;
    options.spatial={50,50,0,false,20260929};options.minimumDailyStations=35;
    std::vector<StationFile> globe;
    for(int i=0;i<2500;++i) {
        auto s=Station(std::format("S{:08}",i),std::asin(1-2*(i+.5)/2500)*180/std::acos(-1),
            std::fmod(i*137.507764,360)-180);
        std::vector<int> days;
        for(int day=0;day<1826;++day) if((day+i*7)%20!=0&&(day+i*11)%31!=0) days.push_back(day);
        Availability(s,first,days,{},1826);globe.push_back(std::move(s));
    }
    std::vector<std::size_t> candidates(globe.size());std::iota(candidates.begin(),candidates.end(),0);
    const auto begin=std::chrono::steady_clock::now();
    const auto years=GenerateStationSelectionPlan(globe,candidates,options);
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    if(!years.success||years.selected!=50||years.activeDays!=1826||years.minimumAvailable<35
        ||years.minimumDistanceKm<50||years.failingDays!=0) {
        std::cerr<<"FAIL: five-year fixed GIM station set\n";return false;
    }
    std::cout<<"FIVE-YEAR COVERAGE PASS: 2500 candidates, 50 stations, min="<<years.minimumAvailable
        <<", spacing="<<years.minimumDistanceKm<<" km, seconds="<<seconds<<"\n";
    return true;
}
}

int main() {
    const QueryDate saturday = MakeQueryDate(2026, 8, 1);
    const QueryDate weekStart = GpsWeekStartForDate(saturday);
    const QueryDate weekEnd = AddDays(weekStart, 6);
    if (FormatDate(weekStart) != L"2026-07-26"
        || FormatDate(weekEnd) != L"2026-08-01"
        || GpsWeekNumber(weekStart) != GpsWeekNumber(weekEnd)
        || weekStart.dayOfYear != 207
        || weekEnd.dayOfYear != 213) {
        std::cerr << "FAIL: GPS week date range\n";
        return 7;
    }

    // Longitude wraps at the date line: +179 and -179 are close, not far.
    std::vector<StationFile> dateline{
        Station("EAST", 0.0, 179.0),
        Station("WEST", 0.0, -179.0),
        Station("ZERO", 0.0, 0.0)};
    const auto datelineResult = SelectGloballyDistributedStations(dateline, 2);
    if (datelineResult.selected != 2
        || dateline[0].selected == dateline[1].selected) {
        std::cerr << "FAIL: date-line geometry\n";
        return 1;
    }

    std::vector<StationFile> globe{
        Station("N", 90.0, 0.0),
        Station("S", -90.0, 0.0),
        Station("E", 0.0, 90.0),
        Station("W", 0.0, -90.0),
        Station("A", 0.0, 0.0),
        Station("P", 0.0, 180.0)};
    const auto globeResult = SelectGloballyDistributedStations(globe, 4);
    if (SelectedCount(globe) != 4
        || globeResult.minimumSeparationDegrees < 89.9) {
        std::cerr << "FAIL: spherical maximin distribution\n";
        return 2;
    }

    StationFile missing;
    missing.stationId = "NONE";
    std::vector<StationFile> mixed{
        Station("A", 0.0, 0.0),
        Station("B", 0.0, 180.0),
        missing};
    const auto mixedResult = SelectGloballyDistributedStations(mixed, 3);
    if (mixedResult.selected != 3
        || mixedResult.selectedWithCoordinates != 2
        || mixedResult.selectedWithoutCoordinates != 1) {
        std::cerr << "FAIL: coordinate fallback\n";
        return 3;
    }

    std::vector<StationFile> scoped{
        Station("OUT-W", 0.0, -120.0),
        Station("IN-A", 0.0, 10.0),
        Station("IN-B", 0.0, 20.0),
        Station("OUT-E", 0.0, 120.0)};
    const auto scopedResult = SelectRegionallyDistributedStations(
        scoped,
        {1, 2},
        RegionalSelectionOptions{2, 0.0, 0.0});
    if (scopedResult.selected != 2
        || scoped[0].selected || !scoped[1].selected
        || !scoped[2].selected || scoped[3].selected) {
        std::cerr << "FAIL: candidate scope\n";
        return 4;
    }

    std::vector<StationFile> spaced{
        Station("A", 0.0, 0.0),
        Station("TOO-CLOSE", 0.0, 0.1),
        Station("B", 0.0, 1.0),
        Station("C", 0.0, 2.0)};
    const auto spacedResult = SelectRegionallyDistributedStations(
        spaced,
        {0, 1, 2, 3},
        RegionalSelectionOptions{3, 50.0, 0.0});
    if (spacedResult.selected != 3
        || spaced[1].selected
        || spacedResult.achievedMinimumDistanceKm < 50.0) {
        std::cerr << "FAIL: minimum distance constraint\n";
        return 5;
    }

    std::vector<StationFile> connected{
        Station("C0", 0.0, 0.0),
        Station("C1", 0.0, 0.2),
        Station("C2", 0.0, 0.4),
        Station("ISOLATED", 0.0, 10.0)};
    const auto connectedResult = SelectRegionallyDistributedStations(
        connected,
        {0, 1, 2, 3},
        RegionalSelectionOptions{3, 0.0, 30.0});
    if (connectedResult.selected != 3
        || connectedResult.largestConnectedCandidateCount != 3
        || connected[3].selected
        || connectedResult.achievedMaximumNearestNeighborKm > 30.01) {
        std::cerr << "FAIL: maximum neighbor connectivity\n";
        return 6;
    }

    if (!CoverageTests()) return 8;
    std::cout << "STATION SELECTION + DAILY COVERAGE PASS\n";
    return 0;
}
