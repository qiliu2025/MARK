#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

// The web UI supplies the saved language; native prompts start in English.
namespace NativeUiLanguage {

inline std::atomic_bool english{true};

inline bool IsEnglish() noexcept {
    return english.load(std::memory_order_relaxed);
}

inline void SetLanguage(const std::wstring_view language) noexcept {
    if (language == L"en") english.store(true, std::memory_order_relaxed);
    else if (language == L"zh") english.store(false, std::memory_order_relaxed);
}

inline std::wstring Text(const std::wstring_view original) {
    if (!IsEnglish()) return std::wstring(original);
    using Translation = std::pair<std::wstring_view, std::wstring_view>;
    static constexpr Translation translations[] = {
        {L"新版工作台初始化失败。请确认 Microsoft Edge WebView2 Runtime 与程序 resources 目录完整。", L"Unable to initialize the workbench. Check the Microsoft Edge WebView2 Runtime and the application's resources folder."},
        {L"无法启动新版工作台。请确认 Microsoft Edge WebView2 Runtime 已安装。", L"Unable to start the workbench. Check that Microsoft Edge WebView2 Runtime is installed."},
        {L"无法初始化 Windows 公共控件。", L"Unable to initialize Windows controls."},
        {L"无法注册程序窗口类。", L"Unable to register application windows."},
        {L"无法创建主窗口。", L"Unable to create the main window."},
        {L"完整性比例必须是 0 到 100 之间的数字。", L"Completeness must be a number from 0 to 100."},
        {L"可用天数必须是非负整数。", L"Available days must be a nonnegative integer."},
        {L"每日测站数必须是非负整数。", L"Daily station count must be a nonnegative integer."},
        {L"每日测站比例必须是 0 到 100 之间的数字。", L"Daily station percentage must be a number from 0 to 100."},
        {L"保存目录已更改，但无法记住到下次启动。", L"The save folder changed, but could not be saved for the next session."},
        {L"无法读取精密产品保存目录。", L"Unable to read the precise product save folder."},
        {L"请先选择保存目录。", L"Choose a save folder first."},
        {L"无法创建或打开保存目录。", L"Unable to create or open the save folder."},
        {L"无法在文件资源管理器中打开保存目录。", L"Unable to open the save folder in File Explorer."},
        {L"无法保存下载设置。请检查用户配置目录的写入权限。", L"Unable to save download settings. Check write access to the user configuration folder."},
        {L"请选择有效的起止日期。", L"Choose valid start and end dates."},
        {L"无法打开高级筛选窗口。", L"Unable to open advanced filters."},
        {L"请至少选择一个站网。", L"Select at least one station network."},
        {L"日期无效。", L"Invalid date."},
        {L"结束日期无效。", L"Invalid end date."},
        {L"结束日期不能早于起始日期。", L"The end date cannot precede the start date."},
        {L"最小纬度不能大于最大纬度。", L"Minimum latitude cannot exceed maximum latitude."},
        {L"无法打开文件夹选择器。", L"Unable to open the folder picker."},
        {L"选择 GNSS 数据保存目录", L"Choose the GNSS data save folder"},
        {L"选择精密产品保存目录", L"Choose the precise product save folder"},
        {L"Site Log 元数据仍在加载，当前列表尚未完成接收机、天线或卫星系统筛选。请等待加载完成后再下载。", L"Site Log metadata is still loading. Receiver, antenna and satellite system filters are not complete. Wait for loading to finish before downloading."},
        {L"请先在地图或列表中选择至少一个测站。", L"Select at least one station on the map or in the list."},
        {L"请选择保存目录。", L"Choose a save folder."},
        {L"取消解压与 RINEX 转换吗？已完成的结果会保留，未完成文件保留原件。", L"Cancel extraction and RINEX conversion? Completed results and unfinished original files will be kept."},
        {L"确定取消本次下载吗？\n已经下载完成的文件会保留，未完成的临时文件会由下载器清理。", L"Cancel this download?\nCompleted files will be kept; unfinished temporary files will be removed."},
        {L"日期范围无效：终止日期必须不早于起始日期。", L"Invalid date range: the end date cannot precede the start date."},
        {L"请选择有效日期。", L"Choose a valid date."},
        {L"起始日期无效。", L"Invalid start date."},
        {L"请选择有效的终止日期。", L"Choose a valid end date."},
        {L"终止日期无效。", L"Invalid end date."},
        {L"请先选择一个快速下载方案。", L"Choose a quick download plan first."},
        {L"方案产品的兼容性元数据不完整，请检查后使用普通产品查询下载。", L"The plan's product compatibility metadata is incomplete. Review the products and use the standard product search to download."},
        {L"方案兼容性提示", L"Plan compatibility"},
        {L"请先选择至少一个精密产品。", L"Select at least one precise product."},
        {L"所选范围预计需要处理超过 25 万个产品日期，已为避免卡顿和内存耗尽而停止。\n请缩短日期范围，或减少已选产品后再下载。", L"The selection exceeds 250,000 product dates and was stopped to avoid excessive memory use.\nShorten the date range or select fewer products."},
        {L"无法生成下载清单：所选范围需要的内存超过当前进程可用容量。\n请缩短日期范围或减少已选产品后重试。", L"Unable to build the download list: the selection exceeds available process memory.\nShorten the date range or select fewer products and try again."},
        {L"下载清单过大", L"Download list too large"},
        {L"所选产品在当前范围内没有可下载日期。", L"The selected products have no downloadable dates in this range."},
        {L"自定义方案至少需要一个有效产品规则。", L"A custom plan requires at least one valid product rule."},
        {L"自定义快速方案", L"Custom quick plan"},
        {L"请输入大于 0 的测站数量。", L"Enter a station count greater than zero."},
        {L"最小站间距不能大于最大相邻距离。", L"Minimum station spacing cannot exceed maximum neighbor distance."},
        {L"当前候选范围内没有测站。请修改国家、经纬度或地图框选范围。", L"No stations are available in the current area. Change the country, coordinates or map selection."},
        {L"GNSS 数据下载器", L"MARK"},
        {L"解压与 RINEX 转换\n\n本次待处理 ", L"Extraction and RINEX conversion\n\nFiles to process: "},
        {L" 个文件。解压后，将 Compact RINEX（d / crx）转为标准 RINEX（o / rnx）。\n", L"\nAfter extraction, Compact RINEX (d / crx) will be converted to standard RINEX (o / rnx).\n"},
        {L"每个文件处理成功后删除对应压缩包及中间 Compact RINEX；失败或取消保留原文件。\n\n", L"After each successful conversion, its compressed archive and intermediate Compact RINEX will be removed. Original files are kept if processing fails or is cancelled.\n\n"},
        {L"  建议额外预留：", L"  Recommended extra space: "},
        {L"；当前可用：", L"; available: "},
        {L"这是包含临时文件的保守估算，实际大小取决于文件内容；处理中仍会检查剩余空间。", L"This conservative estimate includes temporary files. Actual size depends on file contents; free space is checked during processing."},
        {L"空间不足。请先清理上述磁盘，再点击“重试”重新检查；点击“取消”保留下载文件。", L"Insufficient disk space. Free space on the volumes above and click Retry to check again, or click Cancel to keep the downloaded files."},
        {L"现在开始处理吗？", L"Start processing now?"},
        // Fragments used in numeric validation and result messages.
        {L"阈值必须在 ", L" threshold must be between "},
        {L"必须是 ", L" must be a number from "},
        {L" 到 ", L" to "},
        {L" 之间。", L"."},
        {L"最小纬度", L"Minimum latitude"},
        {L"最大纬度", L"Maximum latitude"},
        {L"最小经度", L"Minimum longitude"},
        {L"最大经度", L"Maximum longitude"},
        {L"精密产品处理完成：成功 ", L"Precise product processing complete: succeeded "},
        {L"下载已取消：成功 ", L"Download cancelled: succeeded "},
        {L" 下载结束：成功 ", L" download complete: succeeded "},
        {L"下载结束：成功 ", L"Download complete: succeeded "},
        {L"，已存在跳过 ", L", already present "},
        {L"，已存在 ", L", already present "},
        {L"，失败 ", L", failed "},
        {L"。已完成的文件保留在 ", L". Completed files were kept in "},
        {L"。统一保存到 ", L". Saved to "},
        {L"。保存到 ", L". Saved to "},
        {L" 下各年/DOY目录。", L" in year/DOY folders."},
        {L"部分失败项目：", L"Some failed items:"},
        {L"日期范围 ", L"Date range "},
        {L" 至 ", L" to "},
        {L"解压与 RINEX 转换因空间不足暂停：成功 ", L"Extraction and RINEX conversion paused due to insufficient space: succeeded "},
        {L"解压与 RINEX 转换已取消：成功 ", L"Extraction and RINEX conversion cancelled: succeeded "},
        {L"解压与 RINEX 转换完成：成功 ", L"Extraction and RINEX conversion complete: succeeded "},
        {L"失败文件保留原件。", L"Original files were kept for failed items."},
    };
    std::wstring result(original);
    for (const auto& [chinese, translated] : translations) {
        std::size_t position = 0;
        while ((position = result.find(chinese, position)) != std::wstring::npos) {
            result.replace(position, chinese.size(), translated);
            position += translated.size();
        }
    }
    return result;
}

inline int ShowMessageBox(const HWND window, const wchar_t* text,
    const wchar_t* caption, const UINT flags) {
    const auto translatedText = Text(text != nullptr ? text : L"");
    const auto translatedCaption = Text(caption != nullptr ? caption : L"");
    return ::MessageBoxExW(window, translatedText.c_str(), translatedCaption.c_str(), flags,
        IsEnglish() ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
                    : MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED));
}

}  // namespace NativeUiLanguage
