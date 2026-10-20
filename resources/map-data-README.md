# Offline map data

MARK includes an offline reference map for viewing station positions and selecting spatial ranges.

## Data sources and licenses

- Natural Earth provides the land, coastlines, reference lines and geographic labels. Its data are in the public domain. The bundled map uses selected fields and converted geometries. See [Natural Earth terms of use](https://www.naturalearthdata.com/about/terms-of-use/) and `map-sources/natural-earth-public-domain-README-20260929.txt`.
- DataV administrative geometry is used for coordinate classification: https://geo.datav.aliyun.com/areas_v3/bound/100000_full.json.
- OCHA oPt / FISS COD-AB v01 supplies additional coordinate-classification geometry, originally provided by the Palestinian Ministry of Planning. This geometry was merged and combined with other reference data. Source: https://data.humdata.org/dataset/cod-ab-pse. License: [CC BY-IGO 3.0](https://creativecommons.org/licenses/by/3.0/igo/legalcode). The modifications are not endorsed by the data providers.
- Station-coordinate evidence includes public station pages from the Nevada Geodetic Laboratory: https://geodesy.unr.edu/NGLStationPages/.

Country and region names, source codes and coordinates are retained as operational metadata.
