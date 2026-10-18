#include "DateUtil.h"
#include "PreciseProducts.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

int wmain() {
    const auto products = BuildPreciseProductCatalog(MakeQueryDate(2025, 1, 1));
    if (products.size() < 100) {
        std::wcerr << L"Product catalog is unexpectedly small: "
                   << products.size() << L"\n";
        return 1;
    }

    std::set<PreciseProductCategory> categories;
    std::set<std::wstring> ids;
    for (const auto& product : products) {
        categories.insert(product.category);
        if (!ids.insert(product.id).second) {
            std::wcerr << L"Duplicate product id: " << product.id << L"\n";
            return 2;
        }
        if (product.mirrors.empty()) {
            std::wcerr << L"Product has no mirror: " << product.id << L"\n";
            return 3;
        }
        for (const auto& mirror : product.mirrors) {
            if (mirror.url.find(L'<') != std::wstring::npos
                || mirror.url.find(L'{') != std::wstring::npos) {
                std::wcerr << L"Unresolved URL template: " << mirror.url << L"\n";
                return 4;
            }
        }
    }
    if (categories.size() != 10) {
        std::wcerr << L"Not every precise-product family is represented.\n";
        return 5;
    }
    for (const wchar_t* required : {
             L"GPS_IGS_F_sp3", L"GCRE_WHU_F_clk", L"IGS_F_erp",
             L"GCRE_CAS_R_osb", L"GPS_COD_dcb", L"GCRE_WHU_F_obx", L"CODG_ion",
             L"IGS_trop", L"IGS_week_snx", L"MGEX_IGS20_atx"}) {
        if (!ids.contains(required)) {
            std::wcerr << L"Missing representative product: " << required << L"\n";
            return 6;
        }
    }
    const auto whuIon = std::find_if(
        products.begin(), products.end(), [](const auto& product) {
            return product.id == L"WHUG_ion";
        });
    const auto whuRapidIon = std::find_if(
        products.begin(), products.end(), [](const auto& product) {
            return product.id == L"WHRG_ion";
        });
    if (whuIon == products.end() || whuRapidIon == products.end()) {
        std::wcerr << L"WHU final/rapid GIM products are missing.\n";
        return 7;
    }
    const auto hasWhuIonosphereMirror = [](const auto& product) {
        return std::any_of(product.mirrors.begin(), product.mirrors.end(),
            [](const auto& mirror) {
                return mirror.url.find(
                    L"/pub/whu/MGEX/ionosphere/") != std::wstring::npos;
            });
    };
    if (!hasWhuIonosphereMirror(*whuIon)
        || !hasWhuIonosphereMirror(*whuRapidIon)) {
        std::wcerr << L"WHU GIM products have no dedicated ionosphere mirror.\n";
        return 8;
    }
    std::wcout << L"Precise product catalog OK: " << products.size()
               << L" logical products.\n";
    return 0;
}
