#include "ClkAntexInfo.h"
#include "DateUtil.h"
#include "IgsAvailabilityDb.h"
#include "sqlite3.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <set>
#include <sstream>
#include <regex>
#include <tuple>
#include <unordered_map>

namespace {
std::wstring Wide(std::string_view s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring w(n,L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),w.data(),n);return w;
}
std::string Lower(std::string s) {
    for(char& c:s) if(c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');return s;
}
std::string Text(sqlite3_stmt* s,int i) {
    const auto* p=sqlite3_column_text(s,i);return p?reinterpret_cast<const char*>(p):"";
}
std::string Date(const QueryDate& d) {return std::format("{:04}-{:02}-{:02}",d.year,d.month,d.day);}
std::vector<std::string> Split(const std::string& s,char delimiter) {
    std::vector<std::string> parts;std::istringstream in(s);std::string part;
    while(std::getline(in,part,delimiter))parts.push_back(part);return parts;
}
std::string Pattern(const std::string& key) {
    if(!key.empty()&&key.front()=='['&&key.back()==']') {
        // The fast DuckDB summary serializes ten fields as a JSON array;
        // authoritative route adapters use the unit-separated form below.
        const std::regex token(R"json("([^"]*)"|([0-9]+))json");std::vector<std::string> p;
        for(auto i=std::sregex_iterator(key.begin(),key.end(),token);i!=std::sregex_iterator();++i)
            p.push_back((*i)[1].matched?(*i)[1].str():(*i)[2].str());
        if(p.size()!=10)return {};
        return p[0]+"|%|"+std::format("{:02}{:02}",std::stoi(p[8]),std::stoi(p[9]))+"|"+p[7]+"|"+p[5];
    }
    if(key.find('\x1f')!=std::string::npos) {
        const auto p=Split(key,'\x1f');if(p.size()!=9)return {};
        const auto time=Split(p[8],':');if(time.size()!=2)return {};
        return p[0]+"|%|"+std::format("{:02}{:02}",std::stoi(time[0]),std::stoi(time[1]))+"|"+p[7]+"|"+p[5];
    }
    const auto dated=Split(key,'|');
    if(dated.size()==5)return dated[0]+"|%|"+dated[2]+"|"+dated[3]+"|"+dated[4];
    const auto id=Split(key,'_');
    if(id.size()>=4)return id[0]+"|%|"+id[1]+"|"+id[2]+"|"+id[3];
    return {};
}
std::wstring Escape(std::string_view s) {
    std::wstring out;
    for(wchar_t c:Wide(s)) {
        if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}
        else if(c==L'\n')out+=L"\\n";else if(c==L'\r')out+=L"\\r";
        else if(c==L'\t')out+=L"\\t";else if(c>=32)out+=c;
    }return out;
}
std::string Filename(std::string name) {
    name=Lower(name);
    const auto slash=name.find_last_of("/\\");if(slash!=std::string::npos)name.erase(0,slash+1);
    for(const auto suffix:{".gz",".z"})if(name.ends_with(suffix))name.resize(name.size()-std::char_traits<char>::length(suffix));
    return name;
}
std::string Stem(std::string name) {
    name=Filename(std::move(name));
    if(name.ends_with(".atx2"))name.resize(name.size()-5);
    else if(name.ends_with(".atx"))name.resize(name.size()-4);
    return name;
}
const AntexProductRecord* Match(const ClkPcvsDeclaration& declaration,
    const std::vector<AntexProductRecord>& products,bool& modelFallback) {
    const auto stem=Stem(declaration.model);modelFallback=false;
    if(stem.empty())return nullptr;
    const auto filename=Filename(declaration.model);
    const bool namesFile=filename.ends_with(".atx")||filename.ends_with(".atx2");
    for(const auto& p:products)
        for(const auto& r:p.routes)
            if(namesFile ? Filename(r.fileName)==filename : Stem(r.fileName)==stem)return &p;
    // A bare model is not an exact historical revision. Use ONLY its official
    // current alias, and label this as an unverified version in the UI/report.
    if(!declaration.exactFile&&stem.find('_')==std::string::npos)
        for(const auto& p:products)if(p.currentAlias&&Lower(p.modelName)==stem&&p.variant=="STANDARD") {
            modelFallback=true;return &p;
        }
    return nullptr;
}
PreciseBundleProduct Product(const AntexProductRecord& a) {
    PreciseBundleProduct p;p.type=PreciseBundleProductType::Antex;p.logicalKey=a.logicalKey;
    p.displayId=Wide(a.modelName)+L"_"+Wide(a.variant)+L"_"+(a.currentAlias?L"CURRENT":std::to_wstring(a.revisionWeek));
    p.displayName=Wide(a.producer)+L" "+Wide(a.modelName)+L" 天线校准模型（"+Wide(a.scope)+L"）";p.center=a.producer;
    p.series=a.modelName;p.productLine=a.productFamily;p.referenceFrame=a.referenceFrame;
    p.apcModel=a.calibrationType;p.revisionWeek=a.revisionWeek;p.currentAlias=a.currentAlias;
    p.constellation=a.referenceFrame+" / "+a.productFamily;p.interval=a.formatVersion;
    for(const auto& r:a.routes)p.routes.push_back({r.sourceKey,r.fileName,r.downloadUrl});
    if(!p.routes.empty())p.fileName=p.routes.front().fileName;
    return p;
}
}

ClkHeaderRange LoadClkHeaderRange(const std::string& family,const QueryDate& first,const QueryDate& last) {
    ClkHeaderRange result;const auto pattern=Pattern(family);
    if(pattern.empty()){result.error=L"CLK 产品族标识无法解析";return result;}
    const auto path=ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()/"clk_header.duckdb");
    // Read the published knowledge base; maintenance outputs are published separately.
    std::error_code ec;if(!std::filesystem::is_regular_file(path,ec)){result.error=L"CLK 头信息知识库尚未接入";return result;}
    sqlite3* raw=nullptr;
    if(sqlite3_open_v2(reinterpret_cast<const char*>(path.u8string().c_str()),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        if(raw)sqlite3_close(raw);result.error=L"CLK 头信息知识库暂不可读";return result;
    }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    sqlite3_stmt* statement=nullptr;
    const char* sql="SELECT h.logical_key,h.nominal_date,h.file_name,h.status,p.system,p.antex_model,p.applied_text,CAST(p.exact_file AS INTEGER),p.declaration_kind FROM clk_headers h LEFT JOIN clk_pcvs p USING(logical_key) WHERE h.logical_key LIKE ? ESCAPE '\\' AND h.nominal_date BETWEEN ? AND ? ORDER BY h.nominal_date,p.line_no";
    if(sqlite3_prepare_v2(raw,sql,-1,&statement,nullptr)!=SQLITE_OK){result.error=L"CLK 头信息表读取失败";return result;}
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(statement,sqlite3_finalize);
    // Underscores are literal stream characters, not LIKE wildcards.
    std::string safe;for(char c:pattern){if(c=='_'||c=='\\')safe+='\\';safe+=c;}
    sqlite3_bind_text(statement,1,safe.c_str(),-1,SQLITE_TRANSIENT);
    const auto from=Date(first),to=Date(last);
    sqlite3_bind_text(statement,2,from.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(statement,3,to.c_str(),-1,SQLITE_TRANSIENT);
    int step;
    while((step=sqlite3_step(statement))==SQLITE_ROW) {
        const auto date=Text(statement,1);
        auto& record=result.byDate[date];record.logicalKey=Text(statement,0);record.date=date;
        record.fileName=Text(statement,2);record.status=Text(statement,3);
        // CAST the builder's native BOOLEAN in SQL before crossing the
        // legacy SQLite numeric-accessor boundary.
        if(!Text(statement,5).empty())record.declarations.push_back({Text(statement,4),Text(statement,5),Text(statement,6),Text(statement,8),sqlite3_column_int(statement,7)!=0});
    }
    if(step!=SQLITE_DONE){result.error=L"CLK 头信息读取未完成";result.byDate.clear();}
    return result;
}

std::wstring ClkHeaderDetailsJson(const ClkHeaderRange& headers,const std::vector<QueryDate>& availableDates) {
    struct Group {std::string status,signature;std::vector<ClkPcvsDeclaration> declarations;std::vector<std::pair<std::string,std::string>> ranges;};
    std::vector<Group> groups;std::map<std::string,int> counts;std::string previousDate;
    auto dates=availableDates;
    std::ranges::sort(dates,[](const auto& a,const auto& b){return Date(a)<Date(b);});
    dates.erase(std::unique(dates.begin(),dates.end(),[](const auto& a,const auto& b){return Date(a)==Date(b);}),dates.end());
    for(const auto& day:dates) {
        const auto date=Date(day);const auto it=headers.byDate.find(date);
        const std::string status=it==headers.byDate.end()?"unscanned":it->second.status;
        ++counts[status];std::vector<ClkPcvsDeclaration> declarations;
        if(it!=headers.byDate.end())declarations=it->second.declarations;
        std::ranges::sort(declarations,[](const auto& a,const auto& b){return std::tie(a.system,a.model,a.exactFile,a.kind)<std::tie(b.system,b.model,b.exactFile,b.kind);});
        std::string signature=status;
        for(const auto& d:declarations)signature+="|"+d.system+"|"+d.model+"|"+(d.exactFile?"1":"0")+"|"+d.kind;
        const auto group=std::ranges::find(groups,signature,&Group::signature);
        if(group==groups.end())groups.push_back({status,signature,declarations,{{date,date}}});
        else if(Date(AddDays(day,-1))==group->ranges.back().second)group->ranges.back().second=date;
        else group->ranges.push_back({date,date});
    }
    std::wstring json=L"{\"counts\":{";bool comma=false;
    for(const auto& [status,count]:counts){if(comma)json+=L',';comma=true;json+=std::format(L"\"{}\":{}",Escape(status),count);}
    json+=L"},\"groups\":[";comma=false;
    for(const auto& group:groups) {
        if(comma)json+=L',';comma=true;
        json+=std::format(L"{{\"status\":\"{}\",\"declarations\":[",Escape(group.status));bool inner=false;
        for(const auto& d:group.declarations) {
            if(inner)json+=L',';inner=true;
            json+=std::format(L"{{\"system\":\"{}\",\"model\":\"{}\",\"exact\":{},\"kind\":\"{}\",\"text\":\"{}\"}}",Escape(d.system),Escape(d.model),d.exactFile?L"true":L"false",Escape(d.kind),Escape(d.appliedText));
        }
        json+=L"],\"ranges\":[";inner=false;
        for(const auto& [a,b]:group.ranges){if(inner)json+=L',';inner=true;json+=std::format(L"[\"{}\",\"{}\"]",Wide(a),Wide(b));}
        json+=L"]}";
    }
    std::string utf8;
    if(!headers.error.empty()) {
        int n=WideCharToMultiByte(CP_UTF8,0,headers.error.data(),static_cast<int>(headers.error.size()),nullptr,0,nullptr,nullptr);
        utf8.resize(n);WideCharToMultiByte(CP_UTF8,0,headers.error.data(),static_cast<int>(headers.error.size()),utf8.data(),n,nullptr,nullptr);
    }
    json+=std::format(L"],\"error\":\"{}\"}}",Escape(utf8));
    return json;
}

void ApplyClkDeclaredAntex(const PreciseBundleRequest& request,PreciseBundleCatalog& catalog,
    PreciseBundlePlanResult& plan,std::vector<std::wstring>& warnings) {
    if((request.plan!=PreciseBundlePlan::WumPridePppAr
        &&request.plan!=PreciseBundlePlan::NetworkRtkPppRtk)||!plan.valid)return;
    const auto all=LoadAntexAvailabilityCatalog();
    std::map<std::string,ClkHeaderRange> headerFamilies;
    std::map<std::string,PreciseBundleProduct> selectedProducts;
    std::set<std::string> unmatched,modelOnly;
    int unresolvedDays=0;plan.completeDays=0;plan.missingDates.clear();plan.missingProductTypes.clear();plan.manifest.clear();
    std::set<std::string> manifestKeys;
    for(auto& day:plan.dates) {
        const auto availableClock=std::ranges::find(day.members,PreciseBundleProductType::Clk,&PreciseBundleManifestEntry::type);
        if(availableClock==day.members.end()) {
            // Preserve date-effective calibration candidates for browsing
            // when the clock needed to verify its declared model is missing.
            for(const auto& member:day.members)if(member.type==PreciseBundleProductType::Antex) {
                const auto source=std::ranges::find(catalog.products,member.logicalKey,&PreciseBundleProduct::logicalKey);
                if(source!=catalog.products.end())selectedProducts.try_emplace(source->logicalKey,*source);
            }
            ++unresolvedDays;
        } else {
        std::erase_if(day.members,[](const auto& m){return m.type==PreciseBundleProductType::Antex;});
        std::erase(day.missing,PreciseBundleProductType::Antex);
        const auto clock=std::ranges::find(day.members,PreciseBundleProductType::Clk,&PreciseBundleManifestEntry::type);
        bool resolved=clock!=day.members.end();std::set<std::string> selected;
        if(resolved) {
            const auto pattern=Pattern(clock->logicalKey);
            auto [family,inserted]=headerFamilies.try_emplace(pattern);
            if(inserted)family->second=LoadClkHeaderRange(clock->logicalKey,request.startDate,request.endDate);
            const auto header=family->second.byDate.find(Date(day.date));
            if(header==family->second.byDate.end()||header->second.declarations.empty())resolved=false;
            else for(const auto& d:header->second.declarations) {
                bool fallback=false;const auto* match=Match(d,all.products,fallback);
                if(!match||match->routes.empty()){resolved=false;unmatched.insert(d.model);continue;}
                if(fallback||match->currentAlias||(!d.exactFile&&Stem(d.model).find('_')==std::string::npos))modelOnly.insert(d.model);
                if(!selected.insert(match->logicalKey).second)continue;
                auto product=Product(*match);product.coverageStart=request.startDate;product.coverageEnd=request.endDate;
                const auto key=MakeBundleCompatibilityKey(product);
                day.members.push_back({day.date,PreciseBundleProductType::Antex,product.logicalKey,product.fileName,product.routes,key,product.interval});
                selectedProducts.try_emplace(product.logicalKey,std::move(product));
            }
        }
        if(!resolved){day.missing.push_back(PreciseBundleProductType::Antex);++unresolvedDays;}
        }
        day.complete=day.missing.empty();if(day.complete)++plan.completeDays;
        else {plan.missingDates.push_back(day.date);for(auto type:day.missing)
            if(std::ranges::find(plan.missingProductTypes,type)==plan.missingProductTypes.end())plan.missingProductTypes.push_back(type);}
        if(day.complete)for(const auto& member:day.members) {
            const auto key=std::to_string(static_cast<int>(member.type))+"|"+member.logicalKey+"|"+member.fileName;
            if(manifestKeys.insert(key).second)plan.manifest.push_back(member);
        }
    }
    std::erase_if(catalog.products,[](const auto& p){return p.type==PreciseBundleProductType::Antex;});
    for(auto& [key,product]:selectedProducts)catalog.products.push_back(std::move(product));
    plan.complete=plan.completeDays==plan.expectedDays;
    if(unresolvedDays)warnings.push_back(std::format(L"CLK 的 ANTEX 版本有 {} 天未确认；清单保留可用候选文件，需核对模型。",unresolvedDays));
    if(!modelOnly.empty())warnings.push_back(L"部分 CLK 引用未锁定历史版本的 ANTEX；选用同模型官方当前文件，其内容可能随更新改变。");
    if(!unmatched.empty()) {
        std::wstring text=L"ANTEX 库内未匹配：";int count=0;
        for(const auto& s:unmatched){if(count++)text+=L"、";text+=Wide(s);if(count==5)break;}
        if(unmatched.size()>5)text+=L"等";warnings.push_back(text);
    }
}
