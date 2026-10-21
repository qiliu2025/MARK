# MARK

**Metadata-Guided Automation of Resource Workflows**

MARK is a publicly available, locally deployable research software system for organizing and using historical Global Navigation Satellite System (GNSS) observations and precise products. It brings file-availability records, time-dependent station metadata, and product-series information from distributed archives into searchable knowledge bases. Configurable rules then enable researchers to identify observations and precise products that meet study-specific requirements and prepare their automated acquisition.

[![Download MARK for Windows](docs/images/download-windows.svg)](https://github.com/qiliu2025/MARK/releases/download/v1.0.0/MARK-v1.0.0-Windows-x64.exe)

**v1.0.0 · Windows 10/11 x64**

[Release details](https://github.com/qiliu2025/MARK/releases/latest) · [User manual](docs/MARK_user_manual_v1.0.0.pdf)

**To install MARK, use the Windows download button above or [Releases](https://github.com/qiliu2025/MARK/releases/latest).** GitHub's **Code → Download ZIP** contains source code only, not the installer or knowledge bases. If you already downloaded the source ZIP, open [DOWNLOAD_MARK.html](DOWNLOAD_MARK.html) for the installation links.

**软件下载请点击上方 Windows 下载按钮，或前往 [Releases](https://github.com/qiliu2025/MARK/releases/latest)。** GitHub 的 **Code → Download ZIP** 仅包含源码，不含软件安装包和知识库。已下载源码 ZIP 的用户可打开 [DOWNLOAD_MARK.html](DOWNLOAD_MARK.html) 获取下载链接。

## Overview

GNSS observation networks and precise-product archives have accumulated extensive historical resources across institutions and data centers. Preparing data for a particular study requires more than locating files: researchers may need to account for observation dates, changes in station equipment, available signals, geographic coverage, and the characteristics of precise-product series.

MARK organizes the relevant historical information in advance and makes it available through local knowledge bases and executable selection workflows. Researchers can apply different conditions to the same indexed records without independently reconstructing historical resource information from multiple providers for each study. This approach supports systematic preparation of GNSS observations and precise products for long-term positioning, reference-frame, orbit-determination, and geospace research.

## Data coverage

The historical resource indexes included with **MARK v1.0.0** cover:

| Resource | Coverage | Indexed records |
| --- | --- | ---: |
| GNSS observations | **13,830 distinct stations** from **38 public observation sources** | **53,633,924 station-days** |
| Precise and auxiliary products | **13 product categories** from **55 institutions and services** | **6,411,748 historical file records** |

The release includes **64 DuckDB knowledge bases**, with indexed data dates through **2026-08-01 (inclusive)**. Observation and precise-product information are organized for their respective search and selection workflows.

**Local queries work offline.** The knowledge bases contain historical resource records and metadata rather than copies of the original observation files and precise products. Downloading selected files requires network access to the original providers and any authorization required by those providers.

**Reported performance:** In the tests described in the MARK manuscript, knowledge-base queries and download-list generation each took less than one second. These timings exclude the transfer of original files and apply to the reported test conditions.

## Key capabilities

- **Historical observation information:** Search across observation sources and station histories. Examine daily file availability, receiver and antenna configurations, radomes, RINEX versions, sampling intervals, GNSS constellations, and available observation codes. Historical equipment information is associated with the dates to which it applies.
- **Study-specific observation selection:** Combine date coverage, station networks, geographic regions, receiver and antenna models, constellation and signal criteria, and available reference-frame or GIM-site information. Select stations manually or use random and spatially uniform selection with configurable spatial constraints. Reuse predefined observation presets for recurring research requirements.
- **Space-weather and observation-quality conditions:** Select dates using historical **Kp, Dst, and F10.7** indices. Where available, apply daily **IGS/MGEX** quality criteria, with independent constellation selection for observation completeness, RMS multipath, and cycle-slip counts.
- **Precise-product discovery:** Search historical product series by category, provider, processing level, sampling interval, and availability over a selected period. Inspect date coverage and save multi-rule custom presets for repeated queries, including product preparation for PPP-AR and PPP-RTK applications.
- **Automated file acquisition:** Generate download plans from matching historical file records and retrieve original files through supported **HTTPS, FTP, FTPS, and SFTP** routes. Separate observation and product tasks support concurrent transfers, pause/resume, retries, and alternative sources when available. Per-task results and completion summaries are saved in the download folder's `logs/` directory.


## Screenshots

### Observation search

![MARK observation search with station metadata and a global station map](docs/images/observations.png)

### Precise products

![MARK precise-product search with coverage and product details](docs/images/precise-products.png)

## Install and run

1. Download `MARK-v1.0.0-Windows-x64.exe` using the button above or from [Releases](https://github.com/qiliu2025/MARK/releases/latest).
2. Run the installer and select an **empty folder**. Setup checks the required disk space and prepares the included knowledge bases.
3. Launch MARK using its shortcut or `software/MARK.exe` in the selected installation folder.

**Requirements:** Windows 10/11 x64 and Microsoft Edge WebView2 Evergreen Runtime. SFTP sources require Windows OpenSSH Client. Python and a separate 7-Zip installation are not required.

The installed package occupies approximately **10.6 GB** and includes the user manual and the corresponding MARK source archive. New installations start in English and remember subsequent language choices. Keep `software/` and `knowledge-base/` together.

## Documentation

- [User manual](docs/MARK_user_manual_v1.0.0.pdf)
- [Build instructions](docs/BUILD.md)

## Licenses

The original MARK source code is available for **noncommercial scientific research** under [LICENSE](LICENSE). Use, inspection, modification, and compilation are permitted within the terms of that license. **Commercial use requires prior written permission** from the MARK copyright holders. This is a **source-available research license**, not an unrestricted open-source license. Third-party components retain their respective licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The distributed knowledge bases are governed by [separate proprietary terms](KNOWLEDGE_BASE_LICENSE.md) and are not covered by the application source license. Free use of the released knowledge bases through the official MARK client is permitted for noncommercial scientific research, subject to those terms. Original data providers retain their rights and attribution requirements.

## Feedback and contributions

Questions, feedback, and research collaboration or contribution inquiries are welcome at **qi.liu@henu.edu.cn** or **Yuzhi.Zhang@henu.edu.cn**. You can also report problems through [GitHub Issues](https://github.com/qiliu2025/MARK/issues). When reporting an issue, please include the MARK version, relevant date range, observation source or product, and applicable files from `logs/`.
