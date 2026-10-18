# Third-party data notices

Original MARK code and resources are supplied under the noncommercial scientific research license in `LICENSE`. Commercial use of that material requires prior written permission. Bundled third-party software and public reference data keep their respective licenses and rights, including their permissions for commercial use. The separate knowledge bases are governed by `KNOWLEDGE_BASE_LICENSE.md`.

## DuckDB

The application links the bundled Windows x64 DuckDB v1.5.5 library (source ID `d8cdaa33fd`). DuckDB is distributed under the MIT license. The license, API headers and import library are in `third_party/duckdb/`. Official source URLs and version information are recorded in `third_party/duckdb/SOURCE_ORIGIN.json`; the supplied library and C API header are sufficient to compile MARK.

- https://duckdb.org/
- https://github.com/duckdb/duckdb

## Microsoft WebView2

The bundled Microsoft.Web.WebView2 SDK is version 1.0.2903.40. Its distributed license has the BSD 3-clause terms; retain `third_party/webview2/sdk/LICENSE.txt` and the original, untranslated `third_party/webview2/sdk/NOTICE.txt` when redistributing the SDK or its components.

The Microsoft Edge WebView2 Evergreen Runtime is a separately installed runtime, not the SDK and not bundled by this repository. Its distribution is governed by Microsoft's runtime terms. The application uses it to host the local UI.

- https://developer.microsoft.com/en-us/microsoft-edge/webview2/
- https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.2903.40

## 7-Zip

The original 7-Zip 26.03 command-line executable and library in `resources/download-tools/` are used as separate download postprocessing tools. Most code is LGPL-2.1-or-later; some code in `7z.dll` is subject to additional unRAR restrictions, and some uses BSD licenses. The original combined notice is at `resources/download-tools/7zip-LICENSE.txt`.

Retain the notice and satisfy the applicable corresponding-source obligations when redistributing these binaries. Use the version-matched 7-Zip source from the official project; the distribution preparation also provides its third-party source archive.

- https://www.7-zip.org/
- https://www.7-zip.org/download.html

## RNXCMP / CRX2RNX

CRX2RNX from RNXCMP 4.2.0 is included as a separate utility for restoring Hatanaka-compressed RINEX observations. The original terms and package readme are included at `resources/download-tools/RNXCMP-LICENSE.txt` and `resources/download-tools/RNXCMP-README.txt`. The license applies the Geospatial Information Authority of Japan's website terms, with the exception stated in the original license, and requires the reference below when modified or combined software is distributed.

Hatanaka, Y. (2008), *A Compression Format and Tools for GNSS Observation Data*, Bulletin of the Geospatial Information Authority of Japan, 55, 21–30.

- https://www.gsi.go.jp/ENGLISH/Bulletin55.html
- https://www.gsi.go.jp/ENGLISH/page_e30286.html
- https://terras.gsi.go.jp/ja/crx2rnx.html

## curl / libcurl

This application statically embeds curl/libcurl 8.21.0 development snapshot for HTTP(S), FTP and
FTPS file transfers. curl is distributed under the curl license.

- https://curl.se/
- https://curl.se/docs/copyright.html

The separate `curl-libressl.exe` is the unmodified official Windows x64
curl 8.21.0_7 binary, verified against the upstream package hash. Its full
dependency copyright notices and build manifest are preserved in
`third_party/curl/official-notices/` in the source repository and
`licenses/curl-official/` in the portable release. These include LibreSSL,
zlib-ng 2.3.3, Brotli 1.2.0, zstd 1.5.7, libpsl 0.23.2, libssh2 1.11.1,
nghttp2 1.70.0, nghttp3 1.18.0 and ngtcp2 1.25.0. The MPL-2.0 Public Suffix
List and certificate data source origins are also retained in that directory.

## LibreSSL Portable

The embedded libcurl uses LibreSSL Portable 4.3.2 as its TLS backend. LibreSSL
Portable and its imported OpenBSD sources are distributed under the licenses
included in `third_party/libressl-src/COPYING` and the individual source files.

- https://github.com/libressl/portable
- https://cdn.openbsd.org/pub/OpenBSD/LibreSSL/

## SQLite

This application embeds the SQLite 3.53.4 amalgamation to store the local GNSS
file-availability index. SQLite source code is in the public domain.

- https://www.sqlite.org/
- https://www.sqlite.org/download.html

## International GNSS Service (IGS)

This application displays IGS station metadata and provides links to openly distributed
GNSS observation data. Users should acknowledge IGS and the contributing station
operators/agencies in derived work.

- https://www.igs.org/
- https://www.igs.org/data-access/
- https://files.igs.org/pub/station/general/

## flag-icons

The country flag atlas used by the station list is generated from flag-icons
7.5.0, distributed under the MIT license. The source SVGs and license are in
`third_party/flag-icons/`.

- https://github.com/lipis/flag-icons

## Natural Earth map data

The offline map uses Natural Earth Admin-0 country boundaries (50m) and land
data. Natural Earth data are public domain; the bundled files are used only
for drawing the application's offline map.

- https://www.naturalearthdata.com/
- https://github.com/nvkelso/natural-earth-vector

## IGS observation data centers

The local IGS availability index forms a station-day union from CDDIS, BKG,
Wuhan University, KASI, IGN, ESA/GSSC, and SOPAC/GARNER. Directory access
uses the public HTTP(S), FTP, or FTP-SSL interfaces exposed by each center.
Archive terms and attribution requirements remain those of the respective
operator.

- https://cddis.nasa.gov/archive/gnss/data/daily/
- https://igs.bkg.bund.de/access
- https://igs.bkg.bund.de/root_ftp/
- ftp://igs.gnsswhu.cn/pub/gps/data/daily/
- ftp://nfs.kasi.re.kr/gps/data/daily/
- ftp://igs.ign.fr/pub/igs/data/
- https://gssc.esa.int/activities/ftp-and-web-access-to-gnss-repository/
- ftp://gssc.esa.int/gnss/data/daily/
- http://garner.ucsd.edu/pub/rinex/

## NOAA National CORS Network

The application uses the anonymous NOAA CORS public-data bucket for station-day
file discovery and downloads, and an embedded compact snapshot of NOAA's
ITRF2020 coordinate list.

- https://geodesy.noaa.gov/CORS/data.shtml
- https://noaa-cors-pds.s3.amazonaws.com/

## EUREF Permanent GNSS Network

EPN station metadata is derived from the EPN Central Bureau station list. Daily
observation files are queried from the BKG EUREF Regional Data Centre.

- https://www.epncb.oma.be/
- https://igs.bkg.bund.de/root_ftp/EUREF/

## Hong Kong SatRef

The application queries public RINEX observation data operated by the Survey and
Mapping Office of the Lands Department, Hong Kong.

- https://www.geodetic.gov.hk/en/rinex/ftp_RINEX.htm
- https://rinex.geodetic.gov.hk/

## Geoscience Australia GNSS Data Centre

The application enumerates and downloads anonymous public daily RINEX objects from
the official GA GNSS Data Centre S3 repository. Restricted `analysis/` objects are not
queried.

- https://data.gnss.ga.gov.au/docs/home/gnss-data.html
- https://data.gnss.ga.gov.au/docs/rinex-file-s3/v1.0/s3-access.html

## INEGI RGNA

The application queries the public INEGI RGNA web service and asks that service to
bundle the station's available hourly RINEX 3 files. No personal credentials are stored.

- https://www.inegi.org.mx/app/geo2/rgna/

## SONEL

SONEL daily GNSS observations are accessed through its anonymous FTP service. Users
should acknowledge SONEL and the original data providers in derived work.

- https://www.sonel.org/Data-access
- ftp://ftp.sonel.org/gps/data/

## South Africa TrigNet

The application reads post-processing RINEX data from the anonymous TrigNet FTP
archive. Registration is relevant to TrigNet's real-time service, not these public
post-processing files.

- https://www.trignet.co.za/
- ftp://ftp.trignet.co.za/

## SIRGAS

The application embeds a compact snapshot of the official SIRGAS active-station
membership list and SIRGAS2022 coordinates. Observation files remain distributed
by the original station operators and data centres.

- https://sirgas.ipgh.org/maps/stations/stations-list.php
- https://sirgas.ipgh.org/en/gnss-network/stations/

## IBGE RBMC

Brazilian SIRGAS-member observations are queried from the public RBMC RINEX 3
and historical daily archives operated by IBGE.

- https://www.ibge.gov.br/geociencias/informacoes-sobre-posicionamento-geodesico/rede-geodesica/16258-rede-brasileira-de-monitoramento-continuo-dos-sistemas-gnss-rbmc.html
- https://geoftp.ibge.gov.br/informacoes_sobre_posicionamento_geodesico/rbmc/

## IGN Argentina RAMSAC

Argentine SIRGAS-member observations are queried from the public RAMSAC
station and RINEX services.

- https://www.ign.gob.ar/NuestrasActividades/Geodesia/Ramsac/DescargaRinex
- https://ramsac.ign.gob.ar/

## IGM Uruguay REGNA-ROU

Current Uruguayan SIRGAS-member observations are read from the official
anonymous post-processing FTP service. The separate historical SFTP archive
is not accessed in this version.

- https://igm.gub.uy/2016/05/20/servicios-regna-rou/
- ftp://pp.igm.gub.uy/

## IGN France RGP

RGP daily observations are accessed through the official ENSG anonymous FTP
mirror published by IGN France. RGP data are distributed under France's Open
Licence.

- https://rgp.ign.fr/DONNEES/serveurs.php
- ftp://rgpdata.ensg.eu/pub/data/

## Scripps SOPAC/GARNER

The application queries all observation files listed in the public GARNER
daily RINEX archive; it does not filter them through a SIRGAS membership list.

- https://sopac-csrc.ucsd.edu/index.php/data-download/
- http://garner.ucsd.edu/pub/rinex/

## Centro Sismológico Nacional, Universidad de Chile

The CSN option queries publicly listed Chilean GNSS observation files from the
official Centro Sismológico Nacional daily archive. The site uses a public
browser-verification challenge; no user account or personal credentials are
stored.

- https://gps.csn.uchile.cl/data/

## NRCan Canadian Active Control System (CACS)

Canadian daily observations and the current station coordinates are queried
from Natural Resources Canada services. Data use is subject to the Open
Government Licence – Canada and the conditions shown in the archive index.

- https://cacsa.nrcan.gc.ca/gps/data/gpsdata/
- https://webapp.csrs-scrs.nrcan-rncan.gc.ca/geod/process/getStations.php

## IGAC Colombia MAGNA-ECO

Daily observations are queried from the official IGAC FTP structure using the
fixed public service credentials documented by IGAC. Coordinates are read from
the official Colombia en Mapas ArcGIS service. The application does not treat
these public service credentials as a personal user account.

- https://www.igac.gov.co/
- ftp://srvstorage.igac.gov.co/Data/MAGNA-ECO/
- https://www.colombiaenmapas.gov.co/

## ANATI/IGNTG Panama CORS

Panama daily observation ZIP files are queried and downloaded from the
official SFTP service using the fixed public credentials published on the
IGNTG CORS page. Windows OpenSSH is used for the SFTP transport.

- https://ignpanama.anati.gob.pa/index.php/cors
- sftp://cors.anati.gob.pa/

## IGN Honduras CORS

The application queries the seven station folders exposed by the official IGN
Honduras repository and downloads only ZIP links explicitly listed for the
selected date. The packages may contain receiver-native data rather than
already converted RINEX.

- https://www.ign.hn/repositorio?path=EstacionesCors

## Natural Earth

The embedded `ne_110m_land` world map data comes from Natural Earth. Natural Earth data
is in the public domain.

- https://www.naturalearthdata.com/about/terms-of-use/
- https://github.com/nvkelso/natural-earth-vector
