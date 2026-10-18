#include "sqlite3.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
void Check(sqlite3* db,int status) {
    if(status!=SQLITE_OK && status!=SQLITE_ROW && status!=SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void ParameterizedHistoryLookup(sqlite3* db) {
    Check(db,sqlite3_exec(db,
        "CREATE TABLE site_logs(source_url VARCHAR,station_id VARCHAR,network_mask BIGINT);"
        "INSERT INTO site_logs VALUES ('https://example.org/log/TEST00USA_20260101.log','TEST00USA',3);",
        nullptr,nullptr,nullptr));
    sqlite3_stmt* stmt=nullptr;
    Check(db,sqlite3_prepare_v2(db,
        "SELECT source_url,station_id,network_mask FROM site_logs WHERE (?1=0 OR (network_mask & ?1)!=0);",
        -1,&stmt,nullptr));
    Check(db,sqlite3_bind_int64(stmt,1,3));
    Require(sqlite3_step(stmt)==SQLITE_ROW,"History query returned no row");
    Require(sqlite3_column_count(stmt)==3,"Bound result schema was truncated");
    Require(sqlite3_column_type(stmt,0)==SQLITE_TEXT,"Source URL lost its text type");
    const auto* source=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
    Require(source && std::string(source)=="https://example.org/log/TEST00USA_20260101.log","History source URL lost");
    const auto* id=reinterpret_cast<const char*>(sqlite3_column_text(stmt,1));
    Require(id && std::string(id)=="TEST00USA","Station ID lost");
    Require(std::string(source)=="https://example.org/log/TEST00USA_20260101.log","Reading another column overwrote source URL");
    Require(sqlite3_column_int64(stmt,2)==3,"Network mask decoded incorrectly");
    Check(db,sqlite3_finalize(stmt));
}
void BindResetAndNull(sqlite3* db) {
    sqlite3_stmt* stmt=nullptr;
    Check(db,sqlite3_prepare_v2(db,"SELECT ?1 AS first_value,?2 AS other_value;",-1,&stmt,nullptr));
    Check(db,sqlite3_bind_text(stmt,1,"设备履历与历史日期",-1,SQLITE_TRANSIENT));
    Check(db,sqlite3_bind_text(stmt,2,"second-column-with-a-long-value",-1,SQLITE_TRANSIENT));
    Require(sqlite3_step(stmt)==SQLITE_ROW,"Bound text query returned no row");
    Require(sqlite3_column_count(stmt)==2,"Bound text schema lost columns");
    const auto* first=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
    const auto* second=reinterpret_cast<const char*>(sqlite3_column_text(stmt,1));
    Require(first && second && std::string(first)=="设备履历与历史日期","UTF-8 text/pointer changed");
    Require(sqlite3_column_bytes(stmt,0)==static_cast<int>(std::strlen("设备履历与历史日期")),"Text byte count incorrect");
    Check(db,sqlite3_reset(stmt));
    Check(db,sqlite3_clear_bindings(stmt));
    Check(db,sqlite3_bind_int64(stmt,1,20261005));
    Check(db,sqlite3_bind_null(stmt,2));
    Require(sqlite3_step(stmt)==SQLITE_ROW,"Rebound number query returned no row");
    Require(sqlite3_column_type(stmt,0)==SQLITE_INTEGER && sqlite3_column_int64(stmt,0)==20261005,
            "Result types not refreshed after rebinding");
    Require(sqlite3_column_type(stmt,1)==SQLITE_NULL && sqlite3_column_text(stmt,1)==nullptr,"NULL not preserved");
    Check(db,sqlite3_finalize(stmt));
}
void BitmapParameters(sqlite3* db) {
    std::array<unsigned char,512> bits{};
    for(std::size_t n=0;n<bits.size();++n)bits[n]=static_cast<unsigned char>(n%251);
    sqlite3_stmt* stmt=nullptr;
    Check(db,sqlite3_prepare_v2(db,"SELECT ?1 AS bits,?2 AS name;",-1,&stmt,nullptr));
    for(const int size:{6,512}) {
        Check(db,sqlite3_bind_blob(stmt,1,bits.data(),size,SQLITE_TRANSIENT));
        Check(db,sqlite3_bind_text(stmt,2,"bitmap-with-a-neighbouring-text-column",-1,SQLITE_TRANSIENT));
        Require(sqlite3_step(stmt)==SQLITE_ROW,"Bound bitmap returned no row");
        Require(sqlite3_column_type(stmt,0)==SQLITE_BLOB,"Bound bitmap lost BLOB type");
        Require(sqlite3_column_bytes(stmt,0)==size,"Bound bitmap byte length incorrect");
        const void* data=sqlite3_column_blob(stmt,0);
        (void)sqlite3_column_text(stmt,1);
        Require(data && std::memcmp(data,bits.data(),size)==0,"Bitmap changed after adjacent text read");
        Check(db,sqlite3_reset(stmt));
    }
    Check(db,sqlite3_finalize(stmt));
}
void ChunkBoundaries(sqlite3* db) {
    sqlite3_stmt* stmt=nullptr;
    Check(db,sqlite3_prepare_v2(db,
        "SELECT 'https://example.org/log/'||CAST(i AS VARCHAR),i FROM range(4097) t(i) WHERE i>=?1;",
        -1,&stmt,nullptr));
    Check(db,sqlite3_bind_int(stmt,1,0));
    int n=0,status=0;
    while((status=sqlite3_step(stmt))==SQLITE_ROW) {
        const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
        Require(text && std::string(text)=="https://example.org/log/"+std::to_string(n),"Text row/chunk mismatch");
        Require(sqlite3_column_int(stmt,1)==n,"Numeric row/chunk mismatch");
        ++n;
    }
    Require(status==SQLITE_DONE && n==4097,"Rows lost across chunk boundaries");
    Check(db,sqlite3_finalize(stmt));
}
}
int main() {
    sqlite3* db=nullptr;
    try {
        Check(db,sqlite3_open(":memory:",&db));
        ParameterizedHistoryLookup(db);BindResetAndNull(db);BitmapParameters(db);ChunkBoundaries(db);
        Check(db,sqlite3_close(db));db=nullptr;
        std::cout<<"PASS: bound schemas, source URL history keys, per-column pointers, reset/NULL, bitmaps and chunk boundaries\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';sqlite3_close(db);return 1;}
}
