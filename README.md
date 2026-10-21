# MARK

**Metadata-Guided Automation of Resource Workflows**

MARK is a publicly available, locally deployable research software system for organizing and using historical Global Navigation Satellite System (GNSS) observations and precise products. It brings file-availability records, time-dependent station metadata, and product-series information from distributed archives into searchable knowledge bases. Configurable rules then enable researchers to identify observations and precise products that meet study-specific requirements and prepare their automated acquisition.

[![Download MARK for Windows](docs/images/download-windows.svg)](https://github.com/qiliu2025/MARK/releases/download/v1.0.0/MARK-v1.0.0-Windows-x64.exe)

**v1.0.0 · Windows 10/11 x64**

[Release details](https://github.com/qiliu2025/MARK/releases/latest) · [User manual](docs/MARK_user_manual_v1.0.0.pdf)

**To install MARK, use the Windows download button above or [Releases](https://github.com/qiliu2025/MARK/releases/latest).** GitHub's **Code → Download ZIP** contains source code only, not the installer or knowledge bases. If you already downloaded the source ZIP, open [DOWNLOAD_MARK.html](DOWNLOAD_MARK.html) for the installation links.

**软件下载请点击上方 Windows 下载按钮，或前往 [Releases](https://github.com/qiliu2025/MARK/releases/latest)。** GitHub 的 **Code → Download ZIP** 仅包含源码，不含软件安装包和知识库。已下载源码 ZIP 的用户可打开 [DOWNLOAD_MARK.html](DOWNLOAD_MARK.html) 获取下载链接。

## Overview

Global GNSS observation networks and archives of precise products have expanded over decades, providing extensive resources for high-precision positioning, reference-frame maintenance, orbit determination, and geospace research. **However, the increasing volume and diversity of GNSS data and products have exposed limitations in systematic organization and automated utilization. Fragmented historical records and diverse application requirements leave the organization and utilization of GNSS observations and precise products heavily dependent on expert knowledge and manual configuration.** The challenge becomes more significant when a research task requires **historical availability and multiple data attributes to be considered together**. Suitable observations may depend on the receiver and antenna installed on each observation date, sampling interval, GNSS constellations and signals, continuity of date coverage, and geographic distribution. Precise-product preparation involves different constraints, including product category, provider, processing level, sampling interval, and availability throughout the study period. Stations may appear in several archives, and equipment configurations change over time. Consequently, preparing historical observations and products across sources can still depend heavily on archive-specific expertise, repeated checks, and manual configuration. 

MARK was developed to connect **historical resource organization** with **automated, research-oriented utilization**. The software organizes historical observation-file records and precise-product information from distributed sources into two dedicated knowledge bases. Observation availability is associated with station equipment and observation attributes applicable on the corresponding dates, while precise products are indexed by historical availability and product-series characteristics. Researchers specify their study conditions through configurable rules. MARK applies the specified conditions to the indexed records to identify eligible observations, select stations and product series, and generate download plans for acquiring the original files. Importantly, the **prebuilt knowledge bases are distributed with the software**, allowing different research teams to apply their own requirements to the same historical resource information. The indexes can be queried locally without a network connection, while file acquisition uses the original data providers.

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

- **Cross-source historical GNSS resource organization.** Search indexed observation-file availability across public GNSS networks and archives, with stations shared by multiple sources reconciled in the historical station inventory. Search precise and auxiliary products from multiple institutions and services through a separate product knowledge base.

- **Date-dependent station and observation information.** Examine historical receiver, antenna and radome information, RINEX versions, sampling intervals, GNSS constellations, and available observation codes. Equipment validity periods from station logs and information from RINEX headers allow resource selection to use metadata applicable to the observation dates, rather than relying only on current station configurations.

- **Study-specific observation and network selection.** Combine temporal availability, qualified date coverage, station network, equipment, sampling interval, constellation or signal requirements, and geographic constraints. Select stations manually, randomly, or using spatially uniform selection, with additional network constraints where applicable. Predefined observation presets can be reused for recurring study requirements.

- **Space-weather and observation-quality screening.** Select historical dates using **Kp, Dst, and F10.7** indices. Apply available daily **IGS/MGEX** observation-quality criteria, including observation completeness, RMS multipath, and cycle-slip counts, with independent constellation selection for each indicator.

- **Historical precise-product selection.** Query product series by category, provider, processing level, sampling interval, and availability over the requested dates. The product index also retains selected associated information, including GIM contributing-station records and product information related to UPD generation. Create and reuse custom multi-rule presets, including configurations for the input products needed by **PPP-AR** and **PPP-RTK** processing.

- **Automated acquisition from indexed records.** Generate file lists and separate observation and precise-product download plans from selected historical records. Retrieve original files through supported **HTTPS, FTP, FTPS, and SFTP** routes, with parallel transfers, pause/resume, retries, alternative sources when available, and per-task results and completion summaries in the download folder's `logs/` directory.


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
