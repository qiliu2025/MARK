#include "StationSelection.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <random>
#include <set>
#include <unordered_map>

namespace {
constexpr double kRadius = 6371.0088;
constexpr double kEpsilon = 1e-7;
using Words = std::vector<std::uint64_t>;
struct Position { double x, y, z; };
struct Group {
    std::size_t representative = 0;
    bool candidate = false;
    bool coordinate = false;
    bool hasCalendar = false;
    Position position{};
    Words available, unknown;
};
struct Matrix {
    std::vector<Group> groups;
    std::vector<std::size_t> rowGroup;
    std::vector<int> dates;
    std::vector<int> candidateAvailable, candidateUnknown;
    bool cancelled = false;
};
bool Coordinate(const StationFile& s) {
    return s.hasCoordinate && std::isfinite(s.latitude) && std::isfinite(s.longitude)
        && std::abs(s.latitude) <= 90 && std::abs(s.longitude) <= 180;
}
Position Unit(const StationFile& s) {
    const double lat = s.latitude * std::numbers::pi / 180;
    const double lon = s.longitude * std::numbers::pi / 180;
    return {std::cos(lat)*std::cos(lon), std::cos(lat)*std::sin(lon), std::sin(lat)};
}
double Chord(const Group& a, const Group& b) {
    if (!a.coordinate || !b.coordinate) return 0;
    return std::clamp(2 - 2 * (a.position.x*b.position.x
        + a.position.y*b.position.y + a.position.z*b.position.z), 0.0, 4.0);
}
double Kilometres(double chord) {
    return 2*kRadius*std::asin(std::sqrt(std::clamp(chord,0.0,4.0))/2);
}
double ChordLimit(double km) {
    const double sine = std::sin(std::min(km/kRadius, std::numbers::pi)/2);
    return 4*sine*sine;
}
bool Bit(const Words& words, std::size_t day) {
    return (words[day/64] & (std::uint64_t{1} << (day%64))) != 0;
}
void Set(Words& words, std::size_t day) {
    words[day/64] |= std::uint64_t{1} << (day%64);
}
template<class F> void EachDay(const Words& words, F visit) {
    for (std::size_t w=0; w<words.size(); ++w) {
        auto value=words[w];
        while(value) {
            visit(w*64+static_cast<std::size_t>(std::countr_zero(value)));
            value &= value-1;
        }
    }
}
int Intersections(const Words& a, const Words& b) {
    int result=0;
    for(std::size_t w=0;w<a.size();++w) result += std::popcount(a[w]&b[w]);
    return result;
}
bool Contains(const Words& available, const Words& mandatory) {
    for(std::size_t w=0;w<available.size();++w)
        if((available[w]&mandatory[w])!=mandatory[w]) return false;
    return true;
}
Words CalendarWords(const std::vector<CalendarBitmapBlock>& blocks, int first, std::size_t days) {
    Words output((days + 63) / 64);
    Words covered(output.size());
    const auto append = [&](long long word, std::uint64_t bits, std::uint64_t mask) {
        if (word < 0 || static_cast<std::size_t>(word) >= output.size()) return;
        const auto index = static_cast<std::size_t>(word);
        output[index] |= bits & ~covered[index];
        covered[index] |= mask;
    };
    for (const auto& block : blocks) {
        for (std::size_t i = 0; i < block.words.size(); ++i) {
            const auto position = static_cast<long long>(block.firstOrdinal) - first + static_cast<long long>(i) * 64;
            if (position >= static_cast<long long>(days)) break;
            if (position + 64 <= 0) continue;
            const auto word = position >= 0 ? position / 64 : (position - 63) / 64;
            const auto shift = static_cast<unsigned>(position - word * 64);
            // Match CalendarHasDay: the first containing block wins, including
            // zero bits, when coverage blocks happen to overlap.
            append(word, block.words[i] << shift, ~std::uint64_t{0} << shift);
            if (shift) append(word + 1, block.words[i] >> (64 - shift), ~std::uint64_t{0} >> (64 - shift));
        }
    }
    if (days % 64) output.back() &= (std::uint64_t{1} << (days % 64)) - 1;
    return output;
}
// Add 64 independent daily counters together using bit planes. This avoids
// visiting every available day of every station in a multi-year candidate pool.
struct DailyCounters {
    std::vector<Words> planes;
    explicit DailyCounters(std::size_t words) : planes(1, Words(words)) {}
    void Add(const Words& bits) {
        for (std::size_t w = 0; w < bits.size(); ++w) {
            auto carry = bits[w];
            for (std::size_t plane = 0; carry; ++plane) {
                if (plane == planes.size()) planes.emplace_back(bits.size());
                const auto next = planes[plane][w] & carry;
                planes[plane][w] ^= carry; carry = next;
            }
        }
    }
    std::vector<int> Values(std::size_t days) const {
        std::vector<int> values(days);
        for (std::size_t d = 0; d < days; ++d)
            for (std::size_t plane = 0; plane < planes.size(); ++plane)
                if ((planes[plane][d / 64] >> (d % 64)) & 1) values[d] |= 1 << plane;
        return values;
    }
};
std::string Identity(const StationFile& s, std::size_t row) {
    std::string key = s.stationId;
    if (key.empty()) key = s.fourCharId;
    for(auto& c:key) if(c>='a'&&c<='z') c=static_cast<char>(c-'a'+'A');
    // Nine-character identities retain the monument number. A reused short
    // code at distinct coordinates is not the same physical station.
    if(key.size()>=9) key.resize(9);
    else if(!key.empty()&&Coordinate(s)) key += std::format("|{:.3f}|{:.3f}",s.latitude,s.longitude);
    return key.empty()?std::format("row:{}",row):key;
}
Matrix BuildMatrix(const std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidates,
    const std::vector<std::size_t>& selected,
    const StationSelectionPlanOptions& options, std::stop_token stop) {
    Matrix m;
    m.rowGroup.assign(stations.size(),std::numeric_limits<std::size_t>::max());
    const std::set<int> eligible(options.eligibleOrdinals.begin(),options.eligibleOrdinals.end());
    for(int day=options.firstOrdinal;day<=options.lastOrdinal;++day)
        if(!options.eligibleDatesKnown||eligible.contains(day)) m.dates.push_back(day);
    const auto words=(m.dates.size()+63)/64;
    std::unordered_map<std::string,std::size_t> identities;
    identities.reserve(candidates.size());
    const auto fullDays = static_cast<std::size_t>(options.lastOrdinal - options.firstOrdinal + 1);
    const bool contiguous = m.dates.size() == fullDays;
    struct CalendarProjection { Words available, unknown; };
    std::unordered_map<const CalendarCoverage*, CalendarProjection> projected;
    const auto append=[&](std::size_t row,bool candidate) {
        if(row>=stations.size()) return;
        if(m.rowGroup[row]!=std::numeric_limits<std::size_t>::max()) {
            m.groups[m.rowGroup[row]].candidate |= candidate;
            return;
        }
        const auto& s=stations[row];
        const auto key=Identity(s,row);
        auto [found,inserted]=identities.emplace(key,m.groups.size());
        if(inserted) {
            Group g;g.representative=row;g.available.resize(words);g.unknown.resize(words);
            m.groups.push_back(std::move(g));
        }
        auto& g=m.groups[found->second];
        m.rowGroup[row]=found->second;g.candidate |= candidate;
        if(!g.coordinate&&Coordinate(s)) {g.coordinate=true;g.position=Unit(s);g.representative=row;}
        if(s.calendarCoverage) g.hasCalendar=true;
        if (contiguous) {
            if (!s.calendarCoverage) {
                std::fill(g.unknown.begin(), g.unknown.end(), ~std::uint64_t{0});
                if (fullDays % 64) g.unknown.back() &= (std::uint64_t{1} << (fullDays % 64)) - 1;
                return;
            }
            const auto* calendar = s.calendarCoverage.get();
            auto [cached, cachedInserted] = projected.try_emplace(calendar);
            if (cachedInserted) {
                const auto& cal = *calendar;
                auto available = CalendarWords(cal.available, options.firstOrdinal, fullDays);
                auto unverified = CalendarWords(cal.unverified, options.firstOrdinal, fullDays);
                const auto eligible = cal.restrictEligibleDates
                    ? CalendarWords(cal.eligible, options.firstOrdinal, fullDays) : Words{};
                cached->second.available.resize(words); cached->second.unknown.resize(words);
                for (std::size_t w = 0; w < words; ++w) {
                    const auto start = static_cast<long long>(options.firstOrdinal) + static_cast<long long>(w) * 64;
                    const auto lo = std::clamp<long long>(static_cast<long long>(cal.firstOrdinal) - start, 0, 64);
                    const auto hi = std::clamp<long long>(static_cast<long long>(cal.lastOrdinal) - start + 1, 0, 64);
                    const auto below = [](long long count) { return count == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1; };
                    const std::uint64_t known = hi > lo ? below(hi) & ~below(lo) : 0;
                    const auto allowed = known & (cal.restrictEligibleDates ? eligible[w] : ~std::uint64_t{0});
                    cached->second.available[w] = available[w] & allowed & ~unverified[w];
                    cached->second.unknown[w] = ~known | (unverified[w] & allowed);
                }
                if (fullDays % 64) cached->second.unknown.back() &= (std::uint64_t{1} << (fullDays % 64)) - 1;
            }
            for (std::size_t w = 0; w < words; ++w) {
                g.available[w] |= cached->second.available[w];
                g.unknown[w] |= cached->second.unknown[w];
            }
            return;
        }
        for(std::size_t d=0;d<m.dates.size();++d) {
            const int day=m.dates[d];
            if(!s.calendarCoverage) {Set(g.unknown,d);continue;}
            const auto& cal=*s.calendarCoverage;
            if(day<cal.firstOrdinal||day>cal.lastOrdinal) {Set(g.unknown,d);continue;}
            if(cal.restrictEligibleDates&&!CalendarHasDay(cal.eligible,day)) continue;
            if(CalendarHasDay(cal.unverified,day)) Set(g.unknown,d);
            else if(CalendarHasDay(cal.available,day)) Set(g.available,d);
        }
    };
    for(auto row:candidates) {
        if(stop.stop_requested()){m.cancelled=true;return m;}
        append(row,true);
    }
    for(auto row:selected) append(row,false);
    DailyCounters availableCounts(words), unknownCounts(words);
    for(auto& g:m.groups) {
        for(std::size_t w=0;w<words;++w) g.unknown[w] &= ~g.available[w];
        if(!g.candidate) continue;
        // A station without coordinates cannot satisfy an enabled distance
        // constraint, so it must not inflate the candidate coverage bound.
        if((options.spatial.minimumDistanceKm>0||options.spatial.maximumNeighborDistanceKm>0)
            && !g.coordinate) continue;
        availableCounts.Add(g.available); unknownCounts.Add(g.unknown);
    }
    m.candidateAvailable = availableCounts.Values(m.dates.size());
    m.candidateUnknown = unknownCounts.Values(m.dates.size());
    return m;
}
bool Connected(const Matrix& m,const std::vector<std::size_t>& selected,double limit) {
    if(limit<=0||selected.size()<2) return true;
    std::vector<bool> visited(selected.size());
    visited[0]=true;std::vector<std::size_t> queue{0};
    const double chord=ChordLimit(limit);
    for(std::size_t cursor=0;cursor<queue.size();++cursor)
        for(std::size_t other=0;other<selected.size();++other)
            if(!visited[other]&&m.groups[selected[other]].coordinate
                && Chord(m.groups[selected[queue[cursor]]],m.groups[selected[other]])<=chord+1e-12) {
                visited[other]=true;queue.push_back(other);
            }
    return queue.size()==selected.size();
}
StationSelectionPlan Report(const Matrix& m,const std::vector<std::size_t>& selected,
    const StationSelectionPlanOptions& options) {
    StationSelectionPlan r;
    r.requested=options.spatial.requestedCount;r.selected=selected.size();
    r.minimumDailyStations=std::max(0,options.minimumDailyStations);
    r.activeDays=static_cast<int>(m.dates.size());
    r.excludedDays=options.lastOrdinal-options.firstOrdinal+1-r.activeDays;
    std::vector<int> counts(m.dates.size()),unknown(m.dates.size());
    for(const auto& g:m.groups) if(g.candidate) ++r.candidates;
    for(auto group:selected) {
        const auto& g=m.groups[group];r.selectedIndices.push_back(g.representative);
        if(g.coordinate) ++r.withCoordinates;
        if(!g.hasCalendar) ++r.missingCalendars;
        EachDay(g.available,[&](auto d){++counts[d];});
        EachDay(g.unknown,[&](auto d){++unknown[d];});
    }
    long long sum=0;
    r.minimumAvailable=counts.empty()?0:*std::min_element(counts.begin(),counts.end());
    std::size_t active=0;
    for(int day=options.firstOrdinal;day<=options.lastOrdinal;++day) {
        StationSelectionDay value;value.ordinal=day;
        value.eligible=active<m.dates.size()&&m.dates[active]==day;
        if(value.eligible) {
            value.available=counts[active];value.unknown=unknown[active];
            value.candidateAvailable=m.candidateAvailable[active];
            value.candidateUnknown=m.candidateUnknown[active];
            sum+=value.available;
            if(value.available<r.minimumDailyStations) ++r.failingDays;
            if(value.unknown>0) ++r.unknownDays;
            ++active;
        }
        r.days.push_back(value);
    }
    if(r.activeDays) r.averageAvailable=static_cast<double>(sum)/r.activeDays;
    r.spacingSatisfied=options.spatial.minimumDistanceKm<=0||r.withCoordinates==r.selected;
    r.neighborSatisfied=options.spatial.maximumNeighborDistanceKm<=0||r.withCoordinates==r.selected;
    double closest=std::numeric_limits<double>::infinity();
    for(auto left:selected) {
        if(!m.groups[left].coordinate) continue;
        double nearest=std::numeric_limits<double>::infinity();
        for(auto right:selected) {
            if(right==left||!m.groups[right].coordinate) continue;
            nearest=std::min(nearest,Kilometres(Chord(m.groups[left],m.groups[right])));
        }
        if(std::isfinite(nearest)) {closest=std::min(closest,nearest);r.maximumNearestKm=std::max(r.maximumNearestKm,nearest);}
    }
    if(std::isfinite(closest)) r.minimumDistanceKm=closest;
    if(selected.size()>1&&r.minimumDistanceKm+kEpsilon<options.spatial.minimumDistanceKm) r.spacingSatisfied=false;
    r.neighborSatisfied &= Connected(m,selected,options.spatial.maximumNeighborDistanceKm);
    r.success=r.selected>0&&r.selected==r.requested&&r.activeDays>0&&r.failingDays==0
        &&r.spacingSatisfied&&r.neighborSatisfied;
    r.status=r.success?L"matched":L"unmet";
    r.message=r.success?L"当前测站集合满足数量、距离与每日可用约束。":L"当前测站集合尚未满足全部选站约束。";
    if(!r.activeDays){r.status=L"no-dates";r.message=L"当前日期筛选未保留任何评估日期。";}
    return r;
}
std::vector<std::size_t> SelectedGroups(const Matrix& m,const std::vector<std::size_t>& rows) {
    std::set<std::size_t> seen;
    for(auto row:rows) if(row<m.rowGroup.size()&&m.rowGroup[row]<m.groups.size()) seen.insert(m.rowGroup[row]);
    return {seen.begin(),seen.end()};
}
bool Valid(const StationSelectionPlanOptions& options) {
    return options.lastOrdinal>=options.firstOrdinal
        &&static_cast<long long>(options.lastOrdinal)-options.firstOrdinal<50'000
        &&options.spatial.requestedCount>0
        &&std::isfinite(options.spatial.minimumDistanceKm)&&options.spatial.minimumDistanceKm>=0
        &&std::isfinite(options.spatial.maximumNeighborDistanceKm)&&options.spatial.maximumNeighborDistanceKm>=0;
}
StationSelectionPlan Invalid() {
    StationSelectionPlan r;r.status=L"invalid";r.message=L"选站参数无效：请检查目标数量、时间范围和距离。";return r;
}
long long Debt(const std::vector<int>& counts,int threshold) {
    long long debt=0;for(int count:counts) debt+=std::max(0,threshold-count);return debt;
}
void ChangeCounts(const Group& g,std::vector<int>& counts,int delta) {
    EachDay(g.available,[&](auto d){counts[d]+=delta;});
}
bool ReplacementSpatial(const Matrix& m,const std::vector<std::size_t>& selected,
    std::size_t remove,std::size_t incoming,const RegionalSelectionOptions& o) {
    const auto& g=m.groups[incoming];
    if((o.minimumDistanceKm>0||o.maximumNeighborDistanceKm>0)&&!g.coordinate) return false;
    const double minimum=ChordLimit(o.minimumDistanceKm);
    for(auto old:selected) if(old!=remove&&Chord(g,m.groups[old])+1e-12<minimum) return false;
    if(o.maximumNeighborDistanceKm<=0) return true;
    auto next=selected;*std::find(next.begin(),next.end(),remove)=incoming;
    return Connected(m,next,o.maximumNeighborDistanceKm);
}
void Repair(const Matrix& m,const std::vector<std::size_t>& eligible,
    std::vector<std::size_t>& selected,const StationSelectionPlanOptions& options,
    std::stop_token stop,std::chrono::steady_clock::time_point deadline) {
    if(selected.size()!=options.spatial.requestedCount) return;
    const int threshold=options.minimumDailyStations;
    std::vector<int> counts(m.dates.size());
    for(auto g:selected) ChangeCounts(m.groups[g],counts,1);
    for(int pass=0;pass<80&&!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline;++pass) {
        const auto before=Debt(counts,threshold);if(!before) return;
        Words need((counts.size()+63)/64);
        for(std::size_t d=0;d<counts.size();++d) if(counts[d]<threshold) Set(need,d);
        std::vector<std::pair<int,std::size_t>> shortlist;
        for(auto g:eligible) if(std::find(selected.begin(),selected.end(),g)==selected.end()) {
            int score=Intersections(m.groups[g].available,need);
            if(score>0) shortlist.emplace_back(score,g);
        }
        const auto limit=std::min<std::size_t>(64,shortlist.size());
        std::partial_sort(shortlist.begin(),shortlist.begin()+limit,shortlist.end(),
            [](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second<b.second;});
        long long best=before;std::size_t add=m.groups.size(),remove=m.groups.size();
        for(std::size_t c=0;c<limit;++c) {
            if(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline) break;
            const auto incoming=shortlist[c].second;
            for(auto outgoing:selected) {
                if(!ReplacementSpatial(m,selected,outgoing,incoming,options.spatial)) continue;
                long long debt=0;
                for(std::size_t d=0;d<counts.size();++d) {
                    const int count=counts[d]+static_cast<int>(Bit(m.groups[incoming].available,d))
                        -static_cast<int>(Bit(m.groups[outgoing].available,d));
                    debt+=std::max(0,threshold-count);
                    if(debt>=best) break;
                }
                if(debt<best){best=debt;add=incoming;remove=outgoing;}
            }
        }
        if(add==m.groups.size()) return;
        ChangeCounts(m.groups[remove],counts,-1);ChangeCounts(m.groups[add],counts,1);
        *std::find(selected.begin(),selected.end(),remove)=add;
    }
}
} // namespace

StationSelectionPlan EvaluateStationSelectionPlan(const std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,const std::vector<std::size_t>& selectedIndices,
    const StationSelectionPlanOptions& options,std::stop_token stop) {
    if(!Valid(options)) return Invalid();
    const auto matrix=BuildMatrix(stations,candidateIndices,selectedIndices,options,stop);
    if(matrix.cancelled){StationSelectionPlan r;r.status=L"cancelled";return r;}
    const auto selected=SelectedGroups(matrix,selectedIndices);
    auto report=Report(matrix,selected,options);
    for(auto group:selected) if(!matrix.groups[group].candidate) {
        report.success=false;report.status=L"outside-scope";
        report.message=L"所选测站中有测站不在当前候选筛选范围内，请重新选站。";break;
    }
    return report;
}

StationSelectionPlan GenerateStationSelectionPlan(const std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,const StationSelectionPlanOptions& options,
    std::stop_token stop) {
    if(!Valid(options)) return Invalid();
    const auto matrix=BuildMatrix(stations,candidateIndices,{},options,stop);
    if(matrix.cancelled){StationSelectionPlan r;r.status=L"cancelled";return r;}
    std::vector<std::size_t> eligible;
    for(std::size_t g=0;g<matrix.groups.size();++g)
        if(matrix.groups[g].candidate&&((options.spatial.minimumDistanceKm<=0
            &&options.spatial.maximumNeighborDistanceKm<=0)||matrix.groups[g].coordinate)) eligible.push_back(g);
    auto empty=Report(matrix,{},options);empty.generated=true;
    if(!empty.activeDays) return empty;
    if(options.minimumDailyStations>static_cast<int>(options.spatial.requestedCount)) {
        empty.status=L"invalid";empty.message=L"每日最低可用站数不能大于目标测站数量。";return empty;
    }
    if(eligible.size()<options.spatial.requestedCount) {
        empty.status=L"candidate-shortage";empty.provenInfeasible=true;
        empty.message=std::format(L"只有 {} 个符合筛选与坐标要求的候选测站，少于目标 {} 个；原有选择未被替换。",
            eligible.size(),options.spatial.requestedCount);return empty;
    }
    for(std::size_t d=0;d<matrix.dates.size();++d) {
        if(matrix.candidateAvailable[d]>=options.minimumDailyStations) continue;
        empty.provenInfeasible=matrix.candidateAvailable[d]+matrix.candidateUnknown[d]<options.minimumDailyStations;
        empty.status=empty.provenInfeasible?L"coverage-shortage":L"unverified-coverage";
        empty.message=std::format(L"候选池在 {} 只有 {} 个已确认可用站、{} 个待核实站，无法保证每日 {} 站；原有选择未被替换。",
            CalendarDateText(matrix.dates[d]),matrix.candidateAvailable[d],matrix.candidateUnknown[d],options.minimumDailyStations);
        return empty;
    }
    if(options.minimumDailyStations<=0) {
        auto copies=stations;std::vector<std::size_t> rows;
        for(auto g:eligible) rows.push_back(matrix.groups[g].representative);
        SelectRegionallyDistributedStations(copies,rows,options.spatial);
        std::vector<std::size_t> chosen;
        for(auto g:eligible) if(copies[matrix.groups[g].representative].selected) chosen.push_back(g);
        auto result=Report(matrix,chosen,options);result.generated=true;
        result.randomSeed=options.spatial.randomSeed;
        if(!result.success){result.status=L"not-found";result.message=L"暂未找到满足目标数量与距离约束的方案；原有选择未被替换。";}
        return result;
    }
    auto seed=options.spatial.randomSeed;
    if(!seed){std::random_device device;seed=(static_cast<std::uint64_t>(device())<<32)|device();}
    std::mt19937_64 random(seed);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    const auto words=(matrix.dates.size()+63)/64;
    Words rare(words);
    for(std::size_t d=0;d<matrix.dates.size();++d)
        if(matrix.candidateAvailable[d]<=options.minimumDailyStations*2) Set(rare,d);
    StationSelectionPlan best=empty;long long bestDebt=std::numeric_limits<long long>::max();
    const double minimumChord=ChordLimit(options.spatial.minimumDistanceKm);
    const double maximumChord=ChordLimit(options.spatial.maximumNeighborDistanceKm);
    for(int attempt=0;attempt<12&&!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline;++attempt) {
        std::vector<std::size_t> chosen;
        std::vector<bool> used(matrix.groups.size());
        std::vector<double> nearest(matrix.groups.size(),4.0);
        std::vector<int> counts(matrix.dates.size());
        while(chosen.size()<options.spatial.requestedCount&&!stop.stop_requested()
            &&std::chrono::steady_clock::now()<deadline) {
            Words need(words),mandatory(words);
            const int remaining=static_cast<int>(options.spatial.requestedCount-chosen.size());
            for(std::size_t d=0;d<counts.size();++d) {
                const int deficit=options.minimumDailyStations-counts[d];
                if(deficit>0) Set(need,d);
                if(deficit>=remaining) Set(mandatory,d);
            }
            double score=-1;std::size_t next=matrix.groups.size();
            for(auto g:eligible) {
                if(used[g]||nearest[g]+1e-12<minimumChord) continue;
                if(!chosen.empty()&&options.spatial.maximumNeighborDistanceKm>0&&nearest[g]>maximumChord+1e-12) continue;
                if(!Contains(matrix.groups[g].available,mandatory)) continue;
                const double coverage=static_cast<double>(Intersections(matrix.groups[g].available,need)
                    +3*Intersections(matrix.groups[g].available,rare))/std::max<std::size_t>(1,counts.size()*4);
                const double geometry=matrix.groups[g].coordinate?nearest[g]/4:0;
                const double jitter=std::generate_canonical<double,32>(random);
                // Increasing coverage emphasis across restarts explores the
                // tradeoff while mandatory dates preserve hard feasibility.
                const double balance=std::min(.92,.45+attempt*.06);
                const double value=coverage*balance+(options.spatial.randomize?jitter:geometry)*(1-balance)
                    +(attempt?jitter*.025:0);
                if(value>score){score=value;next=g;}
            }
            if(next==matrix.groups.size()) break;
            used[next]=true;chosen.push_back(next);ChangeCounts(matrix.groups[next],counts,1);
            for(auto g:eligible) nearest[g]=std::min(nearest[g],Chord(matrix.groups[g],matrix.groups[next]));
        }
        Repair(matrix,eligible,chosen,options,stop,deadline);
        auto report=Report(matrix,chosen,options);report.generated=true;report.randomSeed=seed;
        long long debt=0;for(const auto& day:report.days) if(day.eligible) debt+=std::max(0,options.minimumDailyStations-day.available);
        if(report.selected>best.selected||(report.selected==best.selected&&debt<bestDebt)) {best=report;bestDebt=debt;}
        if(report.success) return report;
    }
    if(stop.stop_requested()){best.status=L"cancelled";best.message=L"选站已取消。";}
    else {best.status=L"not-found";best.message=L"暂未找到同时满足数量、距离与每日可用约束的方案；可扩大候选范围或调整条件。原有选择未被替换。";}
    return best;
}
